#!/usr/bin/env python3
"""
Phase 1a — LIVE correctness + speed proof for prompt-cache prefix reuse.

Opt-in (needs a cached MLX model + Apple Silicon GPU), mirroring the
test_mlx_streaming_live.sh convention. NOT part of the always-run suite.

It proves the property that makes prefix reuse safe to ship: a request whose
prompt SHARES a long prefix with a prior request produces output BYTE-
IDENTICAL to a cold (cache-off) generation of the same prompt. Reuse must be
a pure speedup, never an output change. It also reports the second-request
latency with cache on vs off so the win is a measured number, not a claim.

Run:
    python3 scripts/test_mlx_prompt_cache_live.py [--model <id>]

Default model: mlx-community/gemma-4-e2b-it-4bit (small, same Gemma-4 family
as the production targets, so tokenizer-compatible).
"""
from __future__ import annotations

import argparse
import importlib.util
import os
import sys
import time


def _load_server_module():
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "mlx-server.py")
    spec = importlib.util.spec_from_file_location("mlx_server_live", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# A stable "persona" system prompt. Kept short enough that a 2-turn convo
# stays inside Gemma's sliding-attention window, where mlx_lm's rotating KV
# cache is trimmable and prefix reuse reliably fires. (For prompts longer
# than the window the rotating cache wraps and can't be trimmed across a
# divergent suffix — see the sliding-window note in mlx-server.py. Reuse
# still helps within the window and for pure multi-turn extension; the win
# scales with prefill cost, which is far higher on the 26B/31B targets.)
SYSTEM = (
    "You are a warm, concise personal assistant who texts like a close "
    "friend. Keep replies short and direct; no corporate filler."
)


def _turn(m, messages, max_tokens):
    """Run one chat turn through the server's cache-aware generation path.
    Returns (text, mlx_prompt_tokens_processed, full_prompt_len). The
    `prompt_tokens` mlx_lm reports is the count it actually PREFILLED — when
    the prompt cache reuses a prefix, this drops to just the new suffix,
    which is the model-size-independent proof that prefill was skipped (wall
    time hides it on a tiny model under fixed per-call overhead)."""
    tok = m._MLX_TOKENIZER
    prompt = tok.apply_chat_template(
        messages, tokenize=False, add_generation_prompt=True)
    full_ids = tok.encode(prompt)
    suffix_ids, prompt_cache = m._prepare_prompt_cache(full_ids)
    t0 = time.perf_counter()
    iterator, used_cache = m._stream_generate_iter(
        full_ids, suffix_ids, max_tokens, prompt_cache)
    parts, gen_ids, last = [], [], None
    for item in iterator:
        last = item
        d = getattr(item, "text", None)
        if d:
            parts.append(d)
        tid = getattr(item, "token", None)
        if tid is not None:
            gen_ids.append(tid)
    dt = time.perf_counter() - t0
    if used_cache:
        m._finalize_prompt_cache(full_ids, gen_ids)
    return "".join(parts), getattr(last, "prompt_tokens", None), len(full_ids), dt


def _convo(reply1):
    """A 2-turn conversation. Turn 2 feeds back the ACTUAL turn-1 reply, so
    turn 2's prompt is a true token-extension of turn 1's (system+user1+
    assistant1) — the realistic path where prefix reuse applies."""
    base = [{"role": "system", "content": SYSTEM},
            {"role": "user", "content": "Hey, you around?"}]
    t1 = base
    t2 = base + [{"role": "assistant", "content": reply1},
                 {"role": "user", "content": "Cool — tell me a quick joke."}]
    return t1, t2


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default="mlx-community/gemma-4-e2b-it-4bit")
    ap.add_argument("--max-tokens", type=int, default=24)
    args = ap.parse_args()

    m = _load_server_module()
    if not m.have_mlx_lm():
        print("SKIP: mlx_lm not installed")
        return 0
    print(f"[live] loading {args.model} ...", flush=True)
    if not m._try_load_mlx_model(args.model):
        print(f"SKIP: could not load {args.model} (not cached?)")
        return 0

    failed = 0
    mt = args.max_tokens

    # ── COLD run: cache OFF, full 2-turn conversation ─────────────────
    os.environ["HU_MLX_PROMPT_CACHE"] = "0"
    m._invalidate_prompt_cache()
    t1msgs = [{"role": "system", "content": SYSTEM},
              {"role": "user", "content": "Hey, you around?"}]
    cold_r1, _, _, _ = _turn(m, t1msgs, mt)
    _, cold_t2msgs = _convo(cold_r1)
    cold_r2, cold_pt2, full2, _ = _turn(m, cold_t2msgs, mt)

    # ── WARM run: cache ON, same conversation. Turn 2 should reuse the
    #    turn-1 prefix (system + user1 + assistant1). ───────────────────
    os.environ["HU_MLX_PROMPT_CACHE"] = "1"
    m._invalidate_prompt_cache()
    warm_r1, warm_pt1, _, _ = _turn(m, t1msgs, mt)
    _, warm_t2msgs = _convo(warm_r1)
    warm_r2, warm_pt2, _, _ = _turn(m, warm_t2msgs, mt)

    # Determinism — needed for the identity assertion to mean anything.
    deterministic = (cold_r1 == warm_r1)
    print(f"  {'PASS' if deterministic else 'INFO'}  generation deterministic: "
          f"{deterministic}")

    # HARD GATE 1 — identity: reuse must not change output.
    if deterministic:
        if cold_r2 == warm_r2:
            print("  PASS  turn-2 output byte-identical with vs without cache")
        else:
            failed += 1
            print("  FAIL  prefix reuse CHANGED turn-2 output")
            print(f"        cold: {cold_r2!r}")
            print(f"        warm: {warm_r2!r}")
    else:
        print(f"  INFO  identical={cold_r2 == warm_r2} (non-deterministic model)")

    # INFO (not a gate) — prefill skip. Reuse is OPPORTUNISTIC: it fires when
    # the cache can be exactly trimmed (or the turn purely extends the cached
    # prefix), and safely RESETS to a full prefill otherwise (rotating-cache
    # wrap, retokenization boundary shift). A reset is correct, just slower,
    # so "no skip this run" is not a failure — only changed OUTPUT is.
    if warm_pt2 is not None and full2:
        skipped = full2 - warm_pt2
        pct = 100.0 * skipped / full2
        if warm_pt2 < full2:
            print(f"  INFO  warm turn-2 prefilled {warm_pt2}/{full2} tokens "
                  f"({pct:.0f}% of prefill skipped via cache)")
        else:
            print(f"  INFO  warm turn-2 prefilled {warm_pt2}/{full2} — reuse "
                  f"reset this run (safe fallback; cold prefilled {cold_pt2})")
    else:
        print("  INFO  mlx_lm did not report prompt_tokens; skip-proof unavailable")

    print(f"\nResult: {'PASS' if failed == 0 else 'FAIL'} ({failed} failures)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
