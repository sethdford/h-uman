#!/usr/bin/env python3
"""
Phase 1b — LIVE speculative-decoding speedup benchmark (opt-in, heavy).

Measures generation tokens/sec on a real target model WITH vs WITHOUT a draft
model, on the same prompts. Spec decode wins when the draft (much smaller,
same tokenizer) proposes tokens the target accepts — each accepted token skips
a full target forward pass. The win scales with the target/draft size ratio.

Default target is the production Gemma-4 26B; draft auto-selects to the E2B
sibling (same tokenizer). Loading the 26B needs ~14GB RAM and is slow — this
is NOT part of the always-run suite.

Run:
    python3 scripts/bench_spec_decode_live.py [--target <id>] [--draft <id>]
                                              [--max-tokens N] [--reps N]
"""
from __future__ import annotations

import argparse
import importlib.util
import os
import sys
import time


def _load():
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location(
        "mlx_server_bench", os.path.join(here, "mlx-server.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


PROMPTS = [
    "Tell me about your day in a couple of sentences.",
    "What's a good plan for a focused morning of coding?",
    "Give me a short, warm reply to a friend who's stressed.",
]


def _run(m, draft_model, max_tokens, reps):
    from mlx_lm import stream_generate
    tok = m._MLX_TOKENIZER
    total_tokens = 0
    total_time = 0.0
    from_draft = 0
    for _ in range(reps):
        for p in PROMPTS:
            prompt = tok.apply_chat_template(
                [{"role": "user", "content": p}],
                tokenize=False, add_generation_prompt=True)
            ids = tok.encode(prompt)
            kw = {"prompt": ids, "max_tokens": max_tokens}
            if draft_model is not None:
                kw["draft_model"] = draft_model
            t0 = time.perf_counter()
            n = 0
            for it in stream_generate(m._MLX_MODEL, tok, **kw):
                n += 1
                if getattr(it, "from_draft", False):
                    from_draft += 1
            total_time += time.perf_counter() - t0
            total_tokens += n
    tps = total_tokens / total_time if total_time > 0 else 0.0
    return tps, total_tokens, from_draft


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="mlx-community/gemma-4-26b-a4b-it-4bit")
    ap.add_argument("--draft", default="")
    ap.add_argument("--max-tokens", type=int, default=64)
    ap.add_argument("--reps", type=int, default=2)
    args = ap.parse_args()

    m = _load()
    if not m.have_mlx_lm():
        print("SKIP: mlx_lm not installed")
        return 0
    print(f"[bench] loading target {args.target} (heavy) ...", flush=True)
    if not m._try_load_mlx_model(args.target):
        print(f"SKIP: could not load {args.target}")
        return 0

    draft_id = args.draft or m._default_draft_for_model(args.target)
    if not draft_id:
        print("SKIP: no draft model resolved for this target")
        return 0
    print(f"[bench] loading draft {draft_id} ...", flush=True)
    if not m._try_load_draft_model(draft_id):
        print(f"SKIP: could not load draft {draft_id}")
        return 0
    if not m._tokenizers_compatible(m._MLX_TOKENIZER, m._MLX_DRAFT_TOKENIZER):
        print("SKIP: draft tokenizer incompatible with target")
        return 0

    # Warm up (Metal kernel compile) so timings are steady-state.
    _run(m, None, 8, 1)

    base_tps, base_n, _ = _run(m, None, args.max_tokens, args.reps)
    spec_tps, spec_n, fd = _run(m, m._MLX_DRAFT_MODEL, args.max_tokens, args.reps)
    accept = (100.0 * fd / spec_n) if spec_n else 0.0
    speedup = (spec_tps / base_tps) if base_tps > 0 else 0.0

    print(f"\n  target : {args.target}")
    print(f"  draft  : {draft_id}")
    print(f"  baseline (no draft) : {base_tps:.1f} tok/s")
    print(f"  speculative decode  : {spec_tps:.1f} tok/s  "
          f"({accept:.0f}% draft-accepted)")
    print(f"  SPEEDUP             : {speedup:.2f}x")
    return 0


if __name__ == "__main__":
    sys.exit(main())
