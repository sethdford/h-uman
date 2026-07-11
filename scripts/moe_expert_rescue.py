#!/usr/bin/env python3
"""Expert-steering rescue attempts: can better expert SELECTION make
router-logit bias actually steer warmth?

The first pass (docs/research/2026-07-09-moe-expert-steering.md) selected
the top response-span frequency-differential experts with uniform ±bias and
found NO warmth shift (63 vs baseline 75 at +2). Two selector hypotheses
could rescue it:

  visible : select from VISIBLE-span differentials only (the first-pass set
            was dominated by thought-span tokens; visible-span experts fire
            on actual reply tokens — df up to 0.71 vs 0.47)
  mass    : bias magnitude proportional to the WEIGHT-MASS differential
            (how much an expert contributes, not just whether it fires),
            normalized so the strongest expert gets the full strength

Arms are judged with the same local trait judge as the first pass and
compared against that run's baseline/resid arms (greedy decoding => the
prior generations are deterministic and reusable).

Usage:
  python3 scripts/moe_expert_rescue.py \
      --profile .../results/moe_profile_warmth_human.npz \
      --prior .../results/moe_compare_response_n4.json
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np

LAB = Path("/Users/sethford/Projects/persona-steering-lab")
sys.path.insert(0, str(LAB))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from moe_expert_steering_smoke import collapse_metrics, pick_experts  # noqa: E402
from moe_steering_compare import MODEL, ADAPTER, judge_score  # noqa: E402
from src.model_utils import (  # noqa: E402
    encode_prompt,
    generate_text,
    layer_owner,
    load_model,
)


class WeightedBiasRouter:
    """BiasedRouter generalized to per-expert signed magnitudes."""

    def __init__(self, inner, bias_np: np.ndarray):
        import mlx.core as mx
        object.__setattr__(self, "inner", inner)
        object.__setattr__(self, "_bias", mx.array(bias_np.astype(np.float32)))

    def __getattr__(self, name):
        return getattr(object.__getattribute__(self, "inner"), name)

    def __call__(self, x):
        import mlx.core as mx
        inner = object.__getattribute__(self, "inner")
        bias = object.__getattribute__(self, "_bias")
        xn = mx.fast.rms_norm(x, inner.scale * inner._root_size, inner.eps)
        scores = inner.proj(xn) + bias.astype(x.dtype)
        k = inner.config.top_k_experts
        idx = mx.argpartition(scores, kth=-k, axis=-1)[..., -k:]
        w = mx.take_along_axis(scores, idx, axis=-1)
        w = mx.softmax(w, axis=-1)
        return idx, w * inner.per_expert_scale[idx]


def mass_differential_bias(profile, top_n: int, n_experts: int):
    """{layer: bias_vector} — signed weight-mass differential, normalized so
    max |delta| == 1.0 across the selected set."""
    tok = json.loads(str(profile["tok_n"]))
    n_p = tok["pos"]["thought"] + tok["pos"]["visible"]
    n_n = tok["neg"]["thought"] + tok["neg"]["visible"]
    m_p = (profile["mass_pos_thought"] + profile["mass_pos_visible"]) / n_p
    m_n = (profile["mass_neg_thought"] + profile["mass_neg_visible"]) / n_n
    dm = m_p - m_n                                   # (L, E) mass-rate delta
    flat = np.argsort(-np.abs(dm), axis=None)[:top_n]
    sel = np.zeros_like(dm, dtype=bool)
    sel[np.unravel_index(flat, dm.shape)] = True
    dm_sel = np.where(sel, dm, 0.0)
    dm_sel = dm_sel / np.abs(dm_sel).max()
    out = {}
    for li in range(dm.shape[0]):
        if np.any(dm_sel[li] != 0.0):
            out[li] = dm_sel[li].astype(np.float32)
    return out


def visible_bias(profile, top_n: int, n_experts: int):
    """{layer: bias_vector} from visible-span frequency differentials,
    uniform +/-1 signs (first-pass shape, better span)."""
    picked = pick_experts(profile, "visible", top_n, z_min=6.0, df_min=0.30)
    out = {}
    for li, experts in picked.items():
        v = np.zeros(n_experts, dtype=np.float32)
        for e, s in experts:
            v[e] = s
        out[li] = v
    return out


def install(model, layer_bias: dict, strength: float):
    owner = layer_owner(model)
    restore = []
    for li, bias in layer_bias.items():
        block = owner.layers[li]
        restore.append((block, block.router))
        block.router = WeightedBiasRouter(block.router, bias * strength)
    return restore


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--profile", required=True)
    ap.add_argument("--prior", required=True,
                    help="moe_compare_response_n4.json from the first pass "
                         "(baseline / resid arms reused — greedy is deterministic)")
    ap.add_argument("--top-n", type=int, default=24)
    ap.add_argument("--max-tokens", type=int, default=900)
    ap.add_argument("--arms", default="expert_visible@2,expert_visible@3,expert_mass@2,expert_mass@4")
    ap.add_argument("--out", default=str(LAB / "results" / "moe_rescue_n4.json"))
    args = ap.parse_args()

    trait = json.loads((LAB / "data" / "traits" / "warmth.json").read_text())
    prior = json.loads(Path(args.prior).read_text())
    questions = list(dict.fromkeys(r["question"] for r in prior["results"]))

    profile = np.load(args.profile)
    n_experts = json.loads(str(profile["meta"]))["n_experts"]
    selectors = {
        "expert_visible": visible_bias(profile, args.top_n, n_experts),
        "expert_mass": mass_differential_bias(profile, args.top_n, n_experts),
    }
    for name, lb in selectors.items():
        n_sel = sum(int((v != 0).sum()) for v in lb.values())
        print(f"[rescue] {name}: {n_sel} experts on layers {sorted(lb)}", flush=True)

    model, tokenizer = load_model(MODEL, adapter_path=ADAPTER)

    results = []
    t0 = time.time()
    for qi, q in enumerate(questions):
        prompt = encode_prompt(tokenizer, None, q)
        for arm in args.arms.split(","):
            base, _, param = arm.partition("@")
            strength = float(param) if param else 2.0
            restore = install(model, selectors[base], strength)
            try:
                text = generate_text(model, tokenizer, prompt, args.max_tokens)
            finally:
                for block, orig in restore:
                    block.router = orig
            visible = text.split("<channel|>", 1)[-1].strip() \
                if "<channel|>" in text else None
            m = collapse_metrics(text)
            results.append({"question": q, "arm": arm, "metrics": m,
                            "visible": visible, "raw": text})
            print(f"[{time.time()-t0:5.0f}s] q{qi} {arm:<18} "
                  f"rep={m['top_unigram_frac']:.2f}/{m['max_repeat_run']} "
                  f"{'EXITED' if visible else 'STUCK-IN-THOUGHT'}", flush=True)

    for r in results:
        r["warmth"] = judge_score(trait["eval_prompt"], r["question"],
                                  r["visible"]) if r["visible"] else None

    # Aggregate new arms next to the prior run's arms for one table.
    print(f"\n{'arm':<18} n exited judged warmth_mean")
    agg = {}
    prior_rows = prior["results"]
    for arm in list(dict.fromkeys(r["arm"] for r in prior_rows)) + \
            list(dict.fromkeys(r["arm"] for r in results)):
        rows = [r for r in prior_rows + results if r["arm"] == arm]
        scores = [r["warmth"] for r in rows if r.get("warmth") is not None]
        ex = sum(1 for r in rows if r["visible"])
        mean = round(float(np.mean(scores)), 1) if scores else None
        agg[arm] = {"n": len(rows), "exited": ex, "warmth_mean": mean,
                    "n_judged": len(scores)}
        print(f"{arm:<18} {len(rows)} {ex:>5} {len(scores):>6} {str(mean):>10}")

    Path(args.out).write_text(json.dumps(
        {"args": vars(args), "aggregate": agg, "results": results}, indent=1))
    print(f"[rescue] saved {args.out}")


if __name__ == "__main__":
    main()
