#!/usr/bin/env python3
"""Statistical benchmark for the local MLX (Gemma 4) server.

Hits POST /v1/chat/completions a configurable number of times across a set
of prompts and reports per-condition median/mean/stddev for:
  - prompt_tokens, completion_tokens
  - TTFT (stream only)
  - total elapsed
  - generation tok/s

Results are written to a JSON file so before/after runs can be diffed.

Usage:
    scripts/bench-gemma-perf.py --tag fused-kv4 --n 5 --out /tmp/bench-fused-kv4.json

The --tag becomes a label inside the persisted JSON. To compare two runs:
    scripts/bench-gemma-perf.py --compare /tmp/bench-baseline.json /tmp/bench-fused-kv4.json
"""
import argparse
import json
import math
import os
import statistics
import sys
import time
import urllib.error
import urllib.request


DEFAULT_URL = "http://127.0.0.1:8741/v1/chat/completions"

PROMPTS = [
    ("short_ack",    "Reply with a single 'OK'."),
    ("short_reply",  "Reply in one sentence: what is the meaning of life?"),
    ("mid_factual",  "Summarize quantum entanglement in 2 sentences."),
    ("mid_creative", "Write a 50-word poem about Apple Silicon."),
    ("long_reply",   "Reply in one paragraph (~80 words) about your favorite kind of music."),
]

SYSTEM = "You are Seth's assistant. Be concise but warm. Reply directly without showing your reasoning."


def health(url_base: str) -> dict:
    try:
        with urllib.request.urlopen(url_base.replace("/v1/chat/completions", "/health"), timeout=4) as r:
            return json.loads(r.read())
    except Exception as e:
        return {"error": str(e)}


def call_once(url: str, prompt: str, stream: bool, max_tokens: int = 200) -> dict:
    body = json.dumps({
        "model": "local",
        "messages": [
            {"role": "system", "content": SYSTEM},
            {"role": "user", "content": prompt},
        ],
        "max_tokens": max_tokens,
        "temperature": 0.7,
        "stream": stream,
    }).encode()
    req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"})
    t0 = time.time()
    pt = ct = 0
    first = None
    text_chunks = []
    if stream:
        with urllib.request.urlopen(req, timeout=240) as r:
            for raw in r:
                line = raw.decode("utf-8", errors="replace").rstrip()
                if not line.startswith("data: ") or line.endswith("[DONE]"):
                    continue
                ch = json.loads(line[6:])
                d = ch.get("choices", [{}])[0].get("delta", {})
                if d.get("content"):
                    if first is None:
                        first = time.time()
                    text_chunks.append(d["content"])
                u = ch.get("usage")
                if u:
                    pt = u.get("prompt_tokens", pt)
                    ct = u.get("completion_tokens", ct)
    else:
        with urllib.request.urlopen(req, timeout=240) as r:
            d = json.loads(r.read())
        u = d.get("usage", {})
        pt = u.get("prompt_tokens", 0)
        ct = u.get("completion_tokens", 0)
        first = time.time()
        text_chunks = [d["choices"][0]["message"]["content"]]
    el = time.time() - t0
    ttft = (first - t0) if first else el
    tps = ct / el if el > 0 else 0.0
    return {
        "prompt_tokens": pt,
        "completion_tokens": ct,
        "ttft": ttft,
        "total": el,
        "tps": tps,
        "sample": "".join(text_chunks)[:80],
    }


def _stats(values):
    if not values:
        return {"n": 0}
    return {
        "n": len(values),
        "mean": statistics.fmean(values),
        "median": statistics.median(values),
        "stdev": statistics.stdev(values) if len(values) > 1 else 0.0,
        "min": min(values),
        "max": max(values),
    }


def run(args):
    print(f"Bench tag: {args.tag}")
    print(f"URL: {args.url}")
    h = health(args.url)
    print("server health:")
    for k in ("model", "engine", "lm_prompt_cache_active", "kv_bits", "kv_quant_scheme",
              "speculative_decoding", "draft_tokens"):
        if k in h:
            print(f"  {k}: {h[k]}")

    print()
    print("warmup (1 call)...")
    try:
        call_once(args.url, "hi", stream=False, max_tokens=20)
    except Exception as e:
        print(f"warmup failed: {e}", file=sys.stderr)

    results = {"tag": args.tag, "url": args.url, "iterations": args.n,
               "started": int(time.time()), "health": h, "conditions": {}}

    for stream in (False, True):
        for tag, prompt in PROMPTS:
            cond = f"{'stream' if stream else 'nstream'}_{tag}"
            ttfts, totals, tpses, cts, pts = [], [], [], [], []
            samples = []
            errors = []
            for i in range(args.n):
                try:
                    r = call_once(args.url, prompt, stream=stream,
                                  max_tokens=args.max_tokens)
                    ttfts.append(r["ttft"])
                    totals.append(r["total"])
                    tpses.append(r["tps"])
                    cts.append(r["completion_tokens"])
                    pts.append(r["prompt_tokens"])
                    samples.append(r["sample"])
                except Exception as e:
                    errors.append(str(e))
                    print(f"  ! {cond} iter {i+1}: {e}", file=sys.stderr)
            results["conditions"][cond] = {
                "prompt": prompt,
                "ttft": _stats(ttfts),
                "total": _stats(totals),
                "tps": _stats(tpses),
                "completion_tokens": _stats(cts),
                "prompt_tokens": _stats(pts),
                "errors": errors,
                "sample": samples[0] if samples else None,
            }
            stat = results["conditions"][cond]
            print(f"  {cond:32}  n={stat['tps']['n']:>2}  "
                  f"tps={stat['tps'].get('median', 0):6.2f}  "
                  f"ttft={stat['ttft'].get('median', 0):5.2f}s  "
                  f"tot={stat['total'].get('median', 0):6.2f}s  "
                  f"gen={stat['completion_tokens'].get('median', 0):5.0f}")

    if args.out:
        with open(args.out, "w") as f:
            json.dump(results, f, indent=2)
        print(f"\nwrote: {args.out}")
    return results


def _delta(before, after):
    if not before or not after or before == 0:
        return "n/a"
    pct = (after - before) / before * 100
    return f"{pct:+5.1f}%"


def compare(args):
    with open(args.compare[0]) as f:
        a = json.load(f)
    with open(args.compare[1]) as f:
        b = json.load(f)
    print(f"BEFORE: {a['tag']}  (n={a['iterations']})")
    print(f"AFTER:  {b['tag']}  (n={b['iterations']})")
    print()
    print(f"{'condition':36} {'tps_before':>12} {'tps_after':>12} {'Δ tps':>10}   "
          f"{'ttft_before':>10} {'ttft_after':>10} {'Δ ttft':>10}")
    print("-" * 120)
    keys = sorted(set(a["conditions"].keys()) | set(b["conditions"].keys()))
    for k in keys:
        ca = a["conditions"].get(k, {})
        cb = b["conditions"].get(k, {})
        tps_a = ca.get("tps", {}).get("median", 0)
        tps_b = cb.get("tps", {}).get("median", 0)
        ttft_a = ca.get("ttft", {}).get("median", 0)
        ttft_b = cb.get("ttft", {}).get("median", 0)
        print(f"{k:36} {tps_a:>9.2f}    {tps_b:>9.2f}   {_delta(tps_a, tps_b):>8}   "
              f"{ttft_a:>7.2f}s   {ttft_b:>7.2f}s   {_delta(ttft_a, ttft_b):>8}")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--url", default=DEFAULT_URL)
    p.add_argument("--n", type=int, default=5, help="iterations per condition")
    p.add_argument("--max-tokens", type=int, default=200)
    p.add_argument("--tag", default=time.strftime("%Y%m%d-%H%M%S"))
    p.add_argument("--out", default=None, help="write results JSON here")
    p.add_argument("--compare", nargs=2, metavar=("BEFORE", "AFTER"),
                   help="diff two prior bench output files")
    args = p.parse_args()
    if args.compare:
        compare(args)
        return
    run(args)


if __name__ == "__main__":
    main()
