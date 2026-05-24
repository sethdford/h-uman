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
import subprocess
import sys
import time
import urllib.error
import urllib.parse
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


def resolve_server_pid(url: str, explicit_pid: int = 0) -> int:
    """Phase 0.2 — find the inference server's PID so we can sample its RSS.

    Order of resolution:
      1. --server-pid if the user passed one explicitly.
      2. Parse the port from `url` and ask `lsof` who's listening.

    Returns 0 if we can't determine a pid; the caller treats 0 as
    "RSS measurement unavailable" (not an error — bench still runs).
    """
    if explicit_pid > 0:
        return explicit_pid
    try:
        port = urllib.parse.urlparse(url).port
        if not port:
            return 0
        out = subprocess.check_output(
            ["lsof", "-nP", "-iTCP:%d" % port, "-sTCP:LISTEN", "-t"],
            stderr=subprocess.DEVNULL, timeout=2,
        ).decode().strip()
        if not out:
            return 0
        return int(out.splitlines()[0])
    except (subprocess.SubprocessError, ValueError, OSError):
        return 0


def sample_rss_bytes(pid: int) -> int:
    """Return resident set size of `pid` in bytes, or 0 on failure.

    Portable across Linux and macOS via `ps -o rss=`. ps reports RSS in
    kilobytes on both platforms; we convert to bytes for the JSON.
    """
    if pid <= 0:
        return 0
    try:
        out = subprocess.check_output(
            ["ps", "-o", "rss=", "-p", str(pid)],
            stderr=subprocess.DEVNULL, timeout=2,
        ).decode().strip()
        kib = int(out.split()[0]) if out else 0
        return kib * 1024
    except (subprocess.SubprocessError, ValueError, OSError):
        return 0


def _fmt_mib(b: int) -> str:
    return "n/a" if b <= 0 else f"{b / (1024 * 1024):.1f} MiB"


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

    # Phase 0.2 — RSS sampling. Resolve the server pid up front so we
    # fail-fast with a clear message if --measure-rss was requested but
    # we can't find the server process.
    server_pid = 0
    rss_baseline = 0
    if args.measure_rss:
        server_pid = resolve_server_pid(args.url, args.server_pid)
        if server_pid <= 0:
            print("  warning: --measure-rss set but server pid could not be "
                  "resolved (no lsof match for the URL port). RSS will be 0.",
                  file=sys.stderr)
        else:
            rss_baseline = sample_rss_bytes(server_pid)
            print(f"  server_pid: {server_pid}  rss_baseline: {_fmt_mib(rss_baseline)}")

    print()
    print("warmup (1 call)...")
    try:
        call_once(args.url, "hi", stream=False, max_tokens=20)
    except Exception as e:
        print(f"warmup failed: {e}", file=sys.stderr)

    rss_after_warmup = sample_rss_bytes(server_pid) if args.measure_rss else 0

    results = {"tag": args.tag, "url": args.url, "iterations": args.n,
               "started": int(time.time()), "health": h, "conditions": {},
               "rss": {
                   "server_pid": server_pid,
                   "baseline_bytes": rss_baseline,
                   "after_warmup_bytes": rss_after_warmup,
                   "after_run_bytes": 0,
                   "measured": bool(args.measure_rss and server_pid > 0),
               }}

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

    if args.measure_rss and server_pid > 0:
        results["rss"]["after_run_bytes"] = sample_rss_bytes(server_pid)
        rss = results["rss"]
        print(f"\nRSS: baseline {_fmt_mib(rss['baseline_bytes'])} → "
              f"warmup {_fmt_mib(rss['after_warmup_bytes'])} → "
              f"end {_fmt_mib(rss['after_run_bytes'])}")

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
    a_rss = a.get("rss", {})
    b_rss = b.get("rss", {})
    if a_rss.get("measured") and b_rss.get("measured"):
        before_kv = a_rss.get("after_warmup_bytes", 0)
        after_kv = b_rss.get("after_warmup_bytes", 0)
        delta = _delta(before_kv, after_kv) if before_kv else "n/a"
        print(f"RSS @ warmup: {_fmt_mib(before_kv)} → {_fmt_mib(after_kv)}  ({delta})")
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
    p.add_argument("--measure-rss", action="store_true",
                   help="sample server RSS at baseline, after warmup, and "
                        "after the full run. Requires the server to be "
                        "reachable via lsof on the URL's port, or "
                        "--server-pid to be set explicitly.")
    p.add_argument("--server-pid", type=int, default=0,
                   help="explicit PID of the inference server (overrides "
                        "lsof-based auto-detection for --measure-rss).")
    args = p.parse_args()
    if args.compare:
        compare(args)
        return
    run(args)


if __name__ == "__main__":
    main()
