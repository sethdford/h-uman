#!/usr/bin/env python3
"""Expert-activation profiler for gemma-4 MoE (SteerMoE-style, arXiv 2509.09660).

Residual-stream injection at mid/late layers collapses gemma-4-26b-a4b into
token repetition because each DecoderLayer's Router reads the SAME hidden
state the injection perturbs (RASA, arXiv 2602.04448: the failure is
expert-routing disruption, not representation damage). The fix direction is
to steer at the EXPERT level: find experts whose activation frequency
differs between trait-positive and trait-negative generations, then bias
only those experts.

This script does the measurement pass:

  1. Re-forwards the pos/neg extraction transcripts (produced by
     persona-steering-lab src/extract.py) through the model teacher-forced.
  2. Records, per (condition, span, layer, expert):
       - top-k selection counts        (was the expert routed to?)
       - routing weight mass           (how much did it contribute?)
       - full router logit sums        (for bias-magnitude calibration)
     over RESPONSE tokens only, split into thought-channel vs visible spans.
  3. Simultaneously re-captures per-layer mean residual activations to
     rebuild the all-layer persona vectors (v_all) — the original extraction
     only saved the early-layer v_hat, and we need the mid-layer (L22)
     residual vector to reproduce the collapse for comparison.
  4. Writes an npz of raw accumulators + a JSON summary of the top
     trait-differential experts with two-proportion z-scores.

Usage:
  python3 scripts/moe_expert_profiler.py --trait warmth_human [--limit 4]

Runs against a PRIVATE model load — never touches the serving processes on
:8741 (production) or :8743 (lab).
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

from src.model_utils import (  # noqa: E402
    CapturingLayer,
    capture_mean_activations,
    encode_continuation,
    encode_prompt,
    install_capture,
    layer_owner,
    load_model,
    restore_all,
)

MODEL = "mlx-community/gemma-4-26b-a4b-it-4bit"
ADAPTER = "/Users/sethford/.human/adapters/persona"

THOUGHT_OPEN = "<|channel>"
THOUGHT_CLOSE = "<channel|>"

CONDS = ("pos", "neg")
SPANS = ("thought", "visible")


class RouterRecorder:
    """Delegating wrapper around a gemma4 Router.

    Passes the real (top_k_indices, top_k_weights) through untouched and
    stashes them alongside the FULL router logits, recomputed with the
    router's own weights (Router.__call__ does not expose its logits; the
    extra norm+proj is one 2816x128 matmul per layer — negligible).
    """

    def __init__(self, inner, layer_idx: int):
        object.__setattr__(self, "inner", inner)
        object.__setattr__(self, "layer_idx", layer_idx)
        object.__setattr__(self, "last", None)

    def __getattr__(self, name):
        return getattr(object.__getattribute__(self, "inner"), name)

    def __call__(self, x):
        import mlx.core as mx

        inner = object.__getattribute__(self, "inner")
        idx, w = inner(x)
        xn = mx.fast.rms_norm(x, inner.scale * inner._root_size, inner.eps)
        scores = inner.proj(xn)
        object.__setattr__(self, "last", (idx, w, scores))
        return idx, w


def install_router_recorders(model) -> list[RouterRecorder]:
    """Replace layer.router with a recorder on every MoE layer.

    The layer list may already hold CapturingLayer wrappers; reach through
    to the real DecoderLayer via .inner when present.
    """
    owner = layer_owner(model)
    recorders = []
    for i, layer in enumerate(owner.layers):
        block = layer.inner if isinstance(layer, CapturingLayer) else layer
        if not getattr(block, "enable_moe", False):
            continue
        rec = RouterRecorder(block.router, i)
        block.router = rec
        recorders.append(rec)
    return recorders


def restore_routers(model, recorders: list[RouterRecorder]) -> None:
    owner = layer_owner(model)
    for rec in recorders:
        i = object.__getattribute__(rec, "layer_idx")
        layer = owner.layers[i]
        block = layer.inner if isinstance(layer, CapturingLayer) else layer
        block.router = object.__getattribute__(rec, "inner")


def split_response_spans(tokenizer, response: str) -> tuple[list[int], list[int]]:
    """Encode the response as (thought_ids, visible_ids).

    gemma-4 brackets deliberation between '<|channel>' and '<channel|>'; the
    visible reply follows the close marker. Half the extraction transcripts
    never close the channel (max-tokens hit mid-thought) — those contribute
    thought tokens only. The split-then-encode approach keeps span
    boundaries exact because the markers are dedicated tokens.
    """
    if THOUGHT_CLOSE in response:
        thought_txt, visible_txt = response.split(THOUGHT_CLOSE, 1)
        thought = encode_continuation(tokenizer, thought_txt + THOUGHT_CLOSE)
        visible = encode_continuation(tokenizer, visible_txt)
    else:
        thought = encode_continuation(tokenizer, response)
        visible = []
    return thought, visible


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--trait", default="warmth_human",
                    help="extraction tag: results/extraction_<trait>.json")
    ap.add_argument("--model", default=MODEL)
    ap.add_argument("--adapter", default=ADAPTER)
    ap.add_argument("--limit", type=int, default=None,
                    help="only first N transcript items (smoke)")
    ap.add_argument("--out-dir", default=str(LAB / "results"))
    args = ap.parse_args()

    transcript = json.loads(
        (LAB / "results" / f"extraction_{args.trait}.json").read_text())
    if args.limit:
        transcript = transcript[: args.limit]

    trait_name = args.trait.split("_")[0]
    spec = json.loads((LAB / "data" / "traits" / f"{trait_name}.json").read_text())

    print(f"[profiler] loading {args.model} + adapter {args.adapter}", flush=True)
    t0 = time.time()
    model, tokenizer = load_model(args.model, adapter_path=args.adapter)
    owner = layer_owner(model)
    n_layers = len(owner.layers)
    moe_layers = [i for i, l in enumerate(owner.layers)
                  if getattr(l, "enable_moe", False)]
    n_experts = owner.layers[moe_layers[0]].config.num_experts if moe_layers else 0
    top_k = owner.layers[moe_layers[0]].config.top_k_experts if moe_layers else 0
    print(f"[profiler] {n_layers} layers, MoE on {len(moe_layers)} of them "
          f"({n_experts} experts, top-{top_k}) — loaded in {time.time()-t0:.0f}s",
          flush=True)

    wrappers = install_capture(model)                # residual capture
    recorders = install_router_recorders(model)      # router capture

    # Accumulators: cond x span x layer x expert
    sel = {c: {s: np.zeros((n_layers, n_experts), dtype=np.int64)
               for s in SPANS} for c in CONDS}
    mass = {c: {s: np.zeros((n_layers, n_experts), dtype=np.float64)
                for s in SPANS} for c in CONDS}
    logit_sum = {c: {s: np.zeros((n_layers, n_experts), dtype=np.float64)
                     for s in SPANS} for c in CONDS}
    tok_n = {c: {s: 0 for s in SPANS} for c in CONDS}

    # Residual-vector rebuild (v_all): mean activation over ALL response
    # tokens, same pooling as src/extract.py.
    acts = {c: [] for c in CONDS}
    norm_sum, norm_n = None, 0

    t0 = time.time()
    for n, item in enumerate(transcript):
        cond = item["condition"]
        instr = spec["instruction"][item["pair"]][cond]
        prompt_ids = encode_prompt(tokenizer, instr, item["question"])
        thought_ids, visible_ids = split_response_spans(tokenizer, item["response"])
        tokens = list(prompt_ids) + thought_ids + visible_ids
        resp_start = len(prompt_ids)
        vis_start = resp_start + len(thought_ids)

        pooled, norms = capture_mean_activations(model, wrappers, tokens, resp_start)
        acts[cond].append(pooled)
        norm_sum = norms if norm_sum is None else norm_sum + norms
        norm_n += 1

        span_slices = {"thought": slice(resp_start, vis_start),
                       "visible": slice(vis_start, len(tokens))}
        for rec in recorders:
            li = object.__getattribute__(rec, "layer_idx")
            idx_a, w_a, sc_a = object.__getattribute__(rec, "last")
            import mlx.core as mx
            idx_np = np.array(idx_a[0])          # (T, K)
            # bfloat16 has no numpy buffer protocol — upcast in MLX first
            w_np = np.array(w_a[0].astype(mx.float32), dtype=np.float64)
            sc_np = np.array(sc_a[0].astype(mx.float32), dtype=np.float64)  # (T, E)
            for span, sl in span_slices.items():
                if sl.stop <= sl.start:
                    continue
                ii = idx_np[sl].reshape(-1)
                ww = w_np[sl].reshape(-1)
                np.add.at(sel[cond][span][li], ii, 1)
                np.add.at(mass[cond][span][li], ii, ww)
                logit_sum[cond][span][li] += sc_np[sl].sum(axis=0)
        for span, sl in span_slices.items():
            tok_n[cond][span] += max(0, sl.stop - sl.start)

        if (n + 1) % 6 == 0:
            print(f"  {n+1}/{len(transcript)} "
                  f"({(n+1)/(time.time()-t0):.2f} fwd/s)", flush=True)

    restore_routers(model, recorders)
    restore_all(model)

    if acts["pos"] and acts["neg"]:
        h_pos = np.stack(acts["pos"])
        h_neg = np.stack(acts["neg"])
        v_all = h_pos.mean(axis=0) - h_neg.mean(axis=0)   # (L, d)
        v_all_hat = v_all / np.linalg.norm(v_all, axis=-1, keepdims=True)
    else:  # --limit smoke may only cover one condition
        v_all_hat = np.zeros((n_layers, 1), dtype=np.float32)
    residual_norm = norm_sum / norm_n

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    tag = f"moe_profile_{args.trait}" + (f"_limit{args.limit}" if args.limit else "")
    npz_path = out_dir / f"{tag}.npz"
    np.savez(
        npz_path,
        moe_layers=np.array(moe_layers),
        v_all_hat=v_all_hat.astype(np.float32),
        residual_norm=residual_norm.astype(np.float32),
        **{f"sel_{c}_{s}": sel[c][s] for c in CONDS for s in SPANS},
        **{f"mass_{c}_{s}": mass[c][s] for c in CONDS for s in SPANS},
        **{f"logit_{c}_{s}": logit_sum[c][s] for c in CONDS for s in SPANS},
        tok_n=json.dumps(tok_n),
        meta=json.dumps({"trait": args.trait, "model": args.model,
                         "adapter": args.adapter, "n_items": len(transcript),
                         "n_experts": n_experts, "top_k": top_k}),
    )
    print(f"[profiler] saved {npz_path}")
    summarize(npz_path)


def summarize(npz_path: Path, z_min: float = 4.0, df_min: float = 0.02) -> None:
    """Print + save the top trait-differential experts per span."""
    d = np.load(npz_path)
    tok_n = json.loads(str(d["tok_n"]))
    meta = json.loads(str(d["meta"]))
    summary = {"meta": meta, "tok_n": tok_n, "spans": {}}

    for span in SPANS + ("response",):
        if span == "response":
            sp = d["sel_pos_thought"] + d["sel_pos_visible"]
            sn = d["sel_neg_thought"] + d["sel_neg_visible"]
            n_p = tok_n["pos"]["thought"] + tok_n["pos"]["visible"]
            n_n = tok_n["neg"]["thought"] + tok_n["neg"]["visible"]
        else:
            sp, sn = d[f"sel_pos_{span}"], d[f"sel_neg_{span}"]
            n_p, n_n = tok_n["pos"][span], tok_n["neg"][span]
        if n_p == 0 or n_n == 0:
            continue
        f_p, f_n = sp / n_p, sn / n_n
        pool = (sp + sn) / (n_p + n_n)
        se = np.sqrt(pool * (1 - pool) * (1 / n_p + 1 / n_n))
        with np.errstate(divide="ignore", invalid="ignore"):
            z = np.where(se > 0, (f_p - f_n) / se, 0.0)
        df = f_p - f_n

        hits = np.argwhere((np.abs(z) >= z_min) & (np.abs(df) >= df_min))
        rows = sorted(
            ({"layer": int(l), "expert": int(e), "f_pos": round(float(f_p[l, e]), 4),
              "f_neg": round(float(f_n[l, e]), 4), "delta_f": round(float(df[l, e]), 4),
              "z": round(float(z[l, e]), 2)} for l, e in hits),
            key=lambda r: -abs(r["z"]))
        summary["spans"][span] = {"n_pos_tok": n_p, "n_neg_tok": n_n,
                                  "n_differential": len(rows), "top": rows[:60]}
        print(f"\n[{span}] pos={n_p} neg={n_n} tokens; "
              f"{len(rows)} experts with |z|>={z_min} & |df|>={df_min}")
        for r in rows[:15]:
            print(f"  L{r['layer']:>2} E{r['expert']:>3}  "
                  f"f+={r['f_pos']:.3f} f-={r['f_neg']:.3f} "
                  f"df={r['delta_f']:+.3f} z={r['z']:+.1f}")

    out = npz_path.with_suffix("").as_posix() + "_summary.json"
    Path(out).write_text(json.dumps(summary, indent=1))
    print(f"\n[profiler] summary -> {out}")


if __name__ == "__main__":
    main()
