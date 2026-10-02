#!/usr/bin/env python3
# scripts/eval_empty_reply_rate.py
#
# Offline empty-reply rate for a candidate persona adapter, measured against a
# SPARE mlx-server instance -- never production. Sends a fixed prompt set
# (classifier-style + chat, scripts/eval_data/empty_reply_prompts.jsonl) N times
# each to http://127.0.0.1:<port>/v1/chat/completions and counts replies whose
# visible text is empty after stripping.
#
# Why: the served seth-glm-air-mlxtune-orpo-20260905-* adapter, trained without
# the production chat template or an end-of-turn token, returned an empty reply
# on ~13% of classifier-style requests, which pushed traffic to cloud failover.
# A retrained adapter must show a lower rate than the serving adapter, on the
# same prompts, on the same spare server config, before it goes to the blind
# A/B gate. See docs/guides/persona-adapter-retrain-runbook.md.
#
# Refuses ports 8741 (production) and 8743 (arena). Never starts or stops a
# server. Exit codes: 0 measured (and under --max-empty-rate if given),
# 1 measured but over --max-empty-rate, 2 NOT measured (server unreachable,
# any request error, or the server's loaded adapter differs from --expect-adapter).
# Request errors are never counted as empty replies, and a run with errors
# writes no result file -- see .claude/rules/no-number-without-a-measurement.md.
"""Empty-reply rate of an adapter served on a spare mlx-server port."""

import argparse
import json
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

FORBIDDEN_PORTS = {8741: "production", 8743: "conversation-arena"}
DEFAULT_PROMPTS = Path(__file__).parent / "eval_data" / "empty_reply_prompts.jsonl"
# Scaffold that can survive server-side stripping; a reply consisting only of
# these is empty for every caller downstream.
_SCAFFOLD_RE = re.compile(r"</?think>|<\|(?:user|assistant|system|observation|endoftext)\|>|/nothink")


def check_port(port: int) -> None:
    if port in FORBIDDEN_PORTS:
        raise SystemExit(f"REFUSING port {port} ({FORBIDDEN_PORTS[port]}): evaluate on a spare "
                         "server instance, never a live one")


def visible_text(content) -> str:
    if not isinstance(content, str):
        return ""
    return _SCAFFOLD_RE.sub("", content).strip()


def is_empty_reply(content) -> bool:
    return visible_text(content) == ""


def load_prompts(path):
    rows = []
    for line in Path(path).read_text().splitlines():
        if line.strip():
            r = json.loads(line)
            if r.get("category") not in ("classifier", "chat") or not r.get("messages"):
                raise SystemExit(f"bad prompt row in {path}: {line[:120]}")
            rows.append(r)
    if not rows:
        raise SystemExit(f"no prompts in {path}")
    return rows


def _http(url, body=None, timeout=180):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, headers={
        "Content-Type": "application/json", "X-HU-Priority": "batch"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def summarize(results):
    """{category: {n, empty, rate}} plus 'all'. n counts completed requests only."""
    out = {}
    for cat in ("classifier", "chat", "all"):
        sel = [r for r in results if cat == "all" or r["category"] == cat]
        n = len(sel)
        empty = sum(1 for r in sel if r["empty"])
        out[cat] = {"n": n, "empty": empty, "rate": (empty / n) if n else None}
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", type=int, required=True, help="spare server port (not 8741/8743)")
    ap.add_argument("--prompts", default=str(DEFAULT_PROMPTS))
    ap.add_argument("--samples", type=int, default=3, help="requests per prompt")
    ap.add_argument("--max-tokens", type=int, default=200)
    ap.add_argument("--temperature", type=float, default=0.7)
    ap.add_argument("--label", required=True, help="e.g. serving-20260905 or candidate-<stamp>")
    ap.add_argument("--expect-adapter", default=None,
                    help="refuse unless /health reports this adapter path as applied")
    ap.add_argument("--out", required=True, help="result JSON path")
    ap.add_argument("--max-empty-rate", type=float, default=None,
                    help="exit 1 if the classifier empty rate exceeds this")
    args = ap.parse_args(argv)

    check_port(args.port)
    base = f"http://127.0.0.1:{args.port}"
    try:
        health = _http(f"{base}/health", timeout=10)
    except (urllib.error.URLError, OSError, ValueError) as e:
        print(f"NOT MEASURED: {base}/health unreachable: {e}", file=sys.stderr)
        return 2
    adapter = health.get("active_adapter") or health.get("adapter")
    if args.expect_adapter is not None:
        want = str(Path(args.expect_adapter).expanduser())
        if adapter != want or not health.get("adapter_applied"):
            print(f"NOT MEASURED: server adapter {adapter!r} (applied="
                  f"{health.get('adapter_applied')}) != expected {want!r}", file=sys.stderr)
            return 2

    prompts = load_prompts(args.prompts)
    results, errors = [], []
    for p in prompts:
        for k in range(args.samples):
            body = {"messages": p["messages"], "max_tokens": args.max_tokens,
                    "temperature": args.temperature}
            t0 = time.time()
            try:
                resp = _http(f"{base}/v1/chat/completions", body)
                choice = (resp.get("choices") or [{}])[0]
                content = (choice.get("message") or {}).get("content")
            except (urllib.error.URLError, OSError, ValueError, KeyError) as e:
                errors.append({"id": p["id"], "sample": k, "error": str(e)})
                continue
            results.append({"id": p["id"], "category": p["category"], "sample": k,
                            "empty": is_empty_reply(content),
                            "finish_reason": choice.get("finish_reason"),
                            "reply": visible_text(content)[:200],
                            "latency_s": round(time.time() - t0, 2)})

    if errors:
        print(f"NOT MEASURED: {len(errors)} request error(s), e.g. {errors[0]} -- "
              "no result written", file=sys.stderr)
        return 2

    summary = summarize(results)
    report = {"label": args.label, "port": args.port, "model": health.get("model"),
              "adapter": adapter, "adapter_applied": health.get("adapter_applied"),
              "tensors_loaded": health.get("tensors_loaded"),
              "prompts_file": str(args.prompts), "samples": args.samples,
              "max_tokens": args.max_tokens, "temperature": args.temperature,
              "summary": summary, "results": results}
    Path(args.out).write_text(json.dumps(report, indent=2) + "\n")
    for cat in ("classifier", "chat", "all"):
        s = summary[cat]
        print(f"{args.label} {cat:10s} empty {s['empty']}/{s['n']} = {s['rate']:.1%}")
    print(f"wrote {args.out}")
    rate = summary["classifier"]["rate"]
    if args.max_empty_rate is not None and rate is not None and rate > args.max_empty_rate:
        print(f"FAIL: classifier empty rate {rate:.1%} > {args.max_empty_rate:.1%}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
