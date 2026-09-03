#!/usr/bin/env python3
"""Regenerate classifier-trial ai_response fields against the LIVE production
head + :8741 (contract C4, authorship-gap nightly, stage (a)).

Takes the base contexts file (~/blind_ab_run/classifier_trials.json --
context + real_seth pairs harvested from real threads) and re-generates
ai_response for each context through:
  - the CURRENT production system prompt (eval_blinded_ab.production_system_prompt,
    which shells out to tools/dump_prompt_head -- the same head production
    builds, not an authored stand-in), and
  - the CURRENT model actually serving on :8741,
so the nightly authorship-gap number tracks today's system rather than a
frozen snapshot from whenever classifier_trials.json was hand-built.

Pattern follows /tmp/gen_v6_trials.py (temperature 0.7, X-HU-Priority: batch,
one request in flight at a time -- :8741 is a single-threaded production
server; a second concurrent caller just queues behind this one, it does not
load a second model).

Refuses (exit non-zero, writes nothing) when the base file is missing/empty,
the production head cannot be built, or fewer than --min-ok generations
succeed -- see .claude/rules/no-number-without-a-measurement.md. A file that
silently drops failed trials must never look like a complete run.
"""
import argparse
import json
import os
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


def load_base(path):
    if not os.path.isfile(path):
        sys.exit(f"REFUSING: base contexts file not found: {path}; nothing written")
    try:
        data = json.load(open(path))
    except Exception as e:
        sys.exit(f"REFUSING: could not parse {path} ({type(e).__name__}: {e}); nothing written")
    trials = data.get("trials") if isinstance(data, dict) else data
    if not trials:
        sys.exit(f"REFUSING: {path} has zero trials; nothing written")
    return trials


def build_head():
    """The production system prompt, via the real head-build path. Never an
    authored stand-in -- see eval_blinded_ab.production_system_prompt's own
    docstring for why that distinction is load-bearing."""
    try:
        from eval_blinded_ab import production_system_prompt
    except Exception as e:
        sys.exit(f"REFUSING: could not import production_system_prompt ({type(e).__name__}: {e}); nothing written")
    try:
        head = production_system_prompt()
    except SystemExit as e:
        sys.exit(f"REFUSING: {e}")
    if not head or len(head) < 500:
        sys.exit(f"REFUSING: production head is {len(head or '')} bytes (expected multi-KB); nothing written")
    return head


def generate_one(mlx_url, head, context, max_tokens, temperature, timeout_s):
    body = {
        "model": "GLM-4.5-Air-4bit",  # cosmetic -- the server serves whatever is loaded, ignores this field
        "max_tokens": max_tokens,
        "temperature": temperature,
        "messages": [
            {"role": "system", "content": head},
            {"role": "user", "content": context},
        ],
    }
    req = urllib.request.Request(
        mlx_url,
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json", "X-HU-Priority": "batch"},
    )
    resp = json.load(urllib.request.urlopen(req, timeout=timeout_s))
    return (resp["choices"][0]["message"]["content"] or "").strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--base", default=os.path.expanduser("~/blind_ab_run/classifier_trials.json"))
    ap.add_argument("--out", required=True)
    ap.add_argument("--mlx-url", default=os.environ.get("HU_MLX_URL", "http://127.0.0.1:8741/v1/chat/completions"))
    ap.add_argument("--max-tokens", type=int, default=120)
    ap.add_argument("--temperature", type=float, default=0.7)
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--min-ok", type=int, default=20)
    a = ap.parse_args()

    base = load_base(a.base)
    head = build_head()

    out_trials = []
    n = len(base)
    for i, t in enumerate(base, 1):
        if not isinstance(t, dict):
            continue
        ctx, real = t.get("context"), t.get("real_seth")
        if not ctx or not real:
            print(f"[{i}/{n}] skip: missing context/real_seth", file=sys.stderr, flush=True)
            continue
        try:
            txt = generate_one(a.mlx_url, head, ctx, a.max_tokens, a.temperature, a.timeout)
        except Exception as e:
            print(f"[{i}/{n}] ERROR {type(e).__name__}: {e}", file=sys.stderr, flush=True)
            continue
        if not txt or txt.startswith("[timeout]"):
            print(f"[{i}/{n}] empty/timeout", file=sys.stderr, flush=True)
            continue
        out_trials.append({
            "i": t.get("i", f"item_{i:02d}"),
            "context": ctx,
            "real_seth": real,
            "ai_response": txt,
        })
        if i % 10 == 0:
            print(f"[{i}/{n}] {len(out_trials)} ok so far", file=sys.stderr, flush=True)

    if len(out_trials) < a.min_ok:
        sys.exit(f"REFUSING: only {len(out_trials)}/{n} generations succeeded "
                 f"(< --min-ok {a.min_ok}); nothing written")

    out = {
        "trials": out_trials,
        "adapter": "production :8741 + live head (nightly regen)",
        "date": time.strftime("%Y-%m-%d"),
        "source_base": a.base,
        "n_base": n,
    }
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    json.dump(out, open(a.out, "w"), indent=2)
    print(f"wrote {len(out_trials)}/{n} trials -> {a.out}")


if __name__ == "__main__":
    main()
