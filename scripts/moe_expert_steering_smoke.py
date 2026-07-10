#!/usr/bin/env python3
"""Mid-layer steering smoke: residual injection (collapses) vs expert-level
routing bias (should not).

Arms (all greedy, same question, no trait instruction in the prompt):
  baseline        unsteered
  resid_L2        residual vector at layer 2, alpha = 0.22*norm  (current workaround)
  resid_L22       residual vector at layer 22, alpha = 21        (collapse repro)
  expert_logit    router-logit bias on warmth-differential experts (mid layers)
  expert_scale    post-softmax weight scaling on the same experts

Expert arms consume the profile produced by moe_expert_profiler.py.
A collapse metric (top-unigram fraction + max immediate-repeat run) is
reported per arm alongside the generated text.

Usage:
  python3 scripts/moe_expert_steering_smoke.py \
      --profile /Users/sethford/Projects/persona-steering-lab/results/moe_profile_warmth_human.npz \
      [--strength 2.0] [--top-n 24] [--arms baseline,resid_L22,expert_logit]

Private model load; never touches :8741 / :8743.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from pathlib import Path

import numpy as np

LAB = Path("/Users/sethford/Projects/persona-steering-lab")
sys.path.insert(0, str(LAB))

from src.model_utils import (  # noqa: E402
    encode_prompt,
    generate_text,
    install_steering,
    layer_owner,
    load_model,
    restore_layer,
)

MODEL = "mlx-community/gemma-4-26b-a4b-it-4bit"
ADAPTER = "/Users/sethford/.human/adapters/persona"

QUESTIONS = [
    "hey, rough day at work. you around?",
    "just got some good news!! guess what",
]


class BiasedRouter:
    """Router replacement that adds a per-expert bias to the routing logits
    BEFORE top-k selection (SteerMoE-style). Replicates gemma4
    Router.__call__ exactly, plus the bias term.
    """

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
        expert_scores = inner.proj(xn) + bias.astype(x.dtype)
        k = inner.config.top_k_experts
        top_k_indices = mx.argpartition(expert_scores, kth=-k, axis=-1)[..., -k:]
        top_k_weights = mx.take_along_axis(expert_scores, top_k_indices, axis=-1)
        top_k_weights = mx.softmax(top_k_weights, axis=-1)
        top_k_weights = top_k_weights * inner.per_expert_scale[top_k_indices]
        return top_k_indices, top_k_weights


class ScaledRouter:
    """Router replacement that leaves selection untouched and multiplies the
    post-softmax weights of chosen experts (contribution-level steering)."""

    def __init__(self, inner, scale_np: np.ndarray):
        import mlx.core as mx
        object.__setattr__(self, "inner", inner)
        object.__setattr__(self, "_scale", mx.array(scale_np.astype(np.float32)))

    def __getattr__(self, name):
        return getattr(object.__getattribute__(self, "inner"), name)

    def __call__(self, x):
        inner = object.__getattribute__(self, "inner")
        scale = object.__getattribute__(self, "_scale")
        idx, w = inner(x)
        return idx, w * scale[idx].astype(w.dtype)


def collapse_metrics(text: str) -> dict:
    toks = text.split()
    tail = toks[-120:] if len(toks) > 120 else toks
    if not tail:
        return {"top_unigram_frac": 0.0, "max_repeat_run": 0, "n_words": 0}
    top = Counter(tail).most_common(1)[0][1] / len(tail)
    run = best = 1
    for a, b in zip(tail, tail[1:]):
        run = run + 1 if a == b else 1
        best = max(best, run)
    return {"top_unigram_frac": round(top, 3), "max_repeat_run": best,
            "n_words": len(toks)}


def pick_experts(profile: dict, span: str, top_n: int, z_min: float,
                 df_min: float) -> dict[int, list[tuple[int, float]]]:
    """Return {layer: [(expert, sign), ...]} for the strongest differential
    experts in the given span, capped at top_n across all layers."""
    d = profile
    tok_n = json.loads(str(d["tok_n"]))
    if span == "response":
        sp = d["sel_pos_thought"] + d["sel_pos_visible"]
        sn = d["sel_neg_thought"] + d["sel_neg_visible"]
        n_p = tok_n["pos"]["thought"] + tok_n["pos"]["visible"]
        n_n = tok_n["neg"]["thought"] + tok_n["neg"]["visible"]
    else:
        sp, sn = d[f"sel_pos_{span}"], d[f"sel_neg_{span}"]
        n_p, n_n = tok_n["pos"][span], tok_n["neg"][span]
    f_p, f_n = sp / n_p, sn / n_n
    pool = (sp + sn) / (n_p + n_n)
    se = np.sqrt(pool * (1 - pool) * (1 / n_p + 1 / n_n))
    with np.errstate(divide="ignore", invalid="ignore"):
        z = np.where(se > 0, (f_p - f_n) / se, 0.0)
    df = f_p - f_n
    mask = (np.abs(z) >= z_min) & (np.abs(df) >= df_min)
    cand = [(-abs(z[l, e]), int(l), int(e), float(np.sign(df[l, e])))
            for l, e in np.argwhere(mask)]
    cand.sort()
    picked: dict[int, list[tuple[int, float]]] = {}
    for _, l, e, s in cand[:top_n]:
        picked.setdefault(l, []).append((e, s))
    return picked


def install_expert_steering(model, picked, strength, mode, n_experts):
    """Install router wrappers; returns restore list [(block, original)]."""
    owner = layer_owner(model)
    restore = []
    for li, experts in picked.items():
        block = owner.layers[li]
        if mode == "logit":
            bias = np.zeros(n_experts, dtype=np.float32)
            for e, s in experts:
                bias[e] = strength * s
            wrapper = BiasedRouter(block.router, bias)
        else:
            scale = np.ones(n_experts, dtype=np.float32)
            for e, s in experts:
                scale[e] = strength if s > 0 else 1.0 / strength
            wrapper = ScaledRouter(block.router, scale)
        restore.append((block, block.router))
        block.router = wrapper
    return restore


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--profile", required=True)
    ap.add_argument("--model", default=MODEL)
    ap.add_argument("--adapter", default=ADAPTER)
    ap.add_argument("--strength", type=float, default=2.0,
                    help="logit-bias magnitude (logit mode) or weight multiplier (scale mode)")
    ap.add_argument("--top-n", type=int, default=24)
    ap.add_argument("--span", default="response", choices=["thought", "visible", "response"])
    ap.add_argument("--z-min", type=float, default=4.0)
    ap.add_argument("--df-min", type=float, default=0.02)
    ap.add_argument("--max-tokens", type=int, default=900,
                    help="must be large enough to exit the thought channel")
    ap.add_argument("--arms", default="baseline,resid_L2,resid_L22,expert_logit,expert_scale",
                    help="comma list; resid_L22@<alpha> / expert_logit@<strength> "
                         "/ expert_scale@<mult> override the default dose per arm")
    ap.add_argument("--resid-layer", type=int, default=22)
    ap.add_argument("--resid-alpha", type=float, default=21.0)
    ap.add_argument("--n-questions", type=int, default=len(QUESTIONS))
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    profile = np.load(args.profile)
    v_all_hat = profile["v_all_hat"]
    residual_norm = profile["residual_norm"]
    n_experts = json.loads(str(profile["meta"]))["n_experts"]

    picked = pick_experts(profile, args.span, args.top_n, args.z_min, args.df_min)
    n_picked = sum(len(v) for v in picked.values())
    print(f"[smoke] {n_picked} experts across layers {sorted(picked)} "
          f"(span={args.span}, top_n={args.top_n})", flush=True)

    model, tokenizer = load_model(args.model, adapter_path=args.adapter)
    owner = layer_owner(model)

    arms = args.arms.split(",")
    results = []
    for q in QUESTIONS[: args.n_questions]:
        prompt = encode_prompt(tokenizer, None, q)
        for arm in arms:
            base, _, param = arm.partition("@")
            dose = float(param) if param else None
            restore_resid = None
            restore_routers = []
            if base == "resid_L2":
                a = dose if dose is not None else 0.22 * float(residual_norm[2])
                restore_resid = (2, install_steering(model, 2, v_all_hat[2], a))
            elif base == "resid_L22":
                li = args.resid_layer
                a = dose if dose is not None else args.resid_alpha
                restore_resid = (li, install_steering(model, li, v_all_hat[li], a))
            elif base == "expert_logit":
                s = dose if dose is not None else args.strength
                restore_routers = install_expert_steering(
                    model, picked, s, "logit", n_experts)
            elif base == "expert_scale":
                s = dose if dose is not None else args.strength
                restore_routers = install_expert_steering(
                    model, picked, s, "scale", n_experts)

            try:
                text = generate_text(model, tokenizer, prompt, args.max_tokens)
            finally:
                if restore_resid:
                    restore_layer(model, restore_resid[0], restore_resid[1])
                for block, orig in restore_routers:
                    block.router = orig

            m = collapse_metrics(text)
            visible = text.split("<channel|>", 1)[-1].strip() if "<channel|>" in text else "(never left thought channel)"
            results.append({"question": q, "arm": arm, "metrics": m,
                            "visible": visible, "raw": text})
            print(f"\n=== [{arm}] q={q[:40]!r}\n"
                  f"    collapse: {m}\n"
                  f"    visible: {visible[:300]}", flush=True)

    out = args.out or (Path(args.profile).with_suffix("").as_posix()
                       + f"_smoke_s{args.strength}_n{args.top_n}.json")
    Path(out).write_text(json.dumps(
        {"args": vars(args), "picked": {str(k): v for k, v in picked.items()},
         "results": results}, indent=1))
    print(f"\n[smoke] saved {out}")


if __name__ == "__main__":
    main()
