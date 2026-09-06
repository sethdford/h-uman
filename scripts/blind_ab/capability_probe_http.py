#!/usr/bin/env python3
"""Base-capability smoke against an ALREADY-SERVED endpoint.

Why HTTP and not adapter_smoke_test.py: that script loads the model itself, so
using it here would mean a second 56 GB load and a second production-dark window.
Probing the endpoint we already stand up costs one load total.

It also tests what production actually serves: the server applies GLM <think>
scrubbing, so a leak that mlx_lm-in-process would hide is visible here.

The point of this probe is to catch the ORPO failure mode named in the plan --
over-correction into blank, degenerate, or truncated output (the 2026-05 run
collapsed exactly that way). It is a REGRESSION check, not a quality score.

Exit 0 = all probes pass. Exit 1 = at least one failed.
"""
import argparse
import json
import re
import sys
import urllib.error
import urllib.request

PROBES = [
    {"id": "persona_short", "cat": "persona",
     "msgs": [{"role": "user", "content": "hey what's up"}]},
    {"id": "persona_warm", "cat": "persona",
     "msgs": [{"role": "user", "content": "Proud of u!"}]},
    {"id": "instruct_translate", "cat": "instruction",
     "msgs": [{"role": "user", "content": "Translate to French, reply with only the translation: The book is on the table."}],
     "expect": r"(livre|table)"},
    {"id": "instruct_extract", "cat": "instruction",
     "msgs": [{"role": "user", "content": "List exactly three primary colors, comma separated, nothing else."}],
     "expect": r"(red|blue|yellow)"},
    {"id": "reason_arith", "cat": "reasoning",
     "msgs": [{"role": "user", "content": "If I have 12 apples and give away 5, then buy 3 more, how many do I have? Answer with the number."}],
     "expect": r"10"},
    {"id": "multiturn", "cat": "reasoning",
     "msgs": [{"role": "user", "content": "My sister's name is Annette."},
              {"role": "assistant", "content": "got it"},
              {"role": "user", "content": "What is my sister's name?"}],
     "expect": r"annette"},
]

THINK_LEAK = re.compile(r"</?think>|<\|assistant\|>|^\s*<\|", re.I)


def degenerate(t, max_cycle=12):
    """A short phrase repeated to fill the window -- the collapse signature."""
    words = t.split()
    if len(words) < 8:
        return False
    for n in range(1, max_cycle):
        if len(words) < 3 * n:
            continue
        blocks = [" ".join(words[i:i + n]) for i in range(0, len(words) - n, n)]
        if len(blocks) >= 4 and len(set(blocks[:6])) == 1:
            return True
    return False


def call(endpoint, model, msgs, max_tokens, temp, timeout):
    body = json.dumps({"model": model, "messages": msgs,
                       "max_tokens": max_tokens, "temperature": temp}).encode()
    req = urllib.request.Request(endpoint, data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        d = json.loads(r.read())
    return (d["choices"][0]["message"]["content"] or "").strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--endpoint", required=True)
    ap.add_argument("--model", default="mlx-community/GLM-4.5-Air-4bit")
    ap.add_argument("--max-tokens", type=int, default=96)
    ap.add_argument("--temp", type=float, default=0.0)
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--out")
    a = ap.parse_args()

    rows, failed = [], 0
    for p in PROBES:
        try:
            text = call(a.endpoint, a.model, p["msgs"], a.max_tokens, a.temp, a.timeout)
        except (urllib.error.URLError, OSError, KeyError, ValueError) as e:
            text = f"[ERROR {type(e).__name__}: {e}]"

        fails = []
        if not text or text.startswith("[ERROR"):
            fails.append("empty_or_error")
        else:
            if degenerate(text):
                fails.append("degenerate")
            if THINK_LEAK.search(text):
                fails.append("think_leak")
            exp = p.get("expect")
            if exp and not re.search(exp, text, re.I):
                fails.append("expectation_missed")
        if fails:
            failed += 1
        rows.append({"id": p["id"], "cat": p["cat"], "text": text,
                     "chars": len(text), "pass": not fails, "fails": fails})
        mark = "ok " if not fails else "FAIL"
        print(f"  [{mark}] {p['id']:20} {len(text):>4}c  {text[:70]!r}"
              + (f"  <- {','.join(fails)}" if fails else ""))

    total = len(rows)
    summary = {"total": total, "passed": total - failed, "failed": failed,
               "by_cat": {}}
    for r in rows:
        c = summary["by_cat"].setdefault(r["cat"], {"n": 0, "pass": 0})
        c["n"] += 1
        c["pass"] += int(r["pass"])
    print(f"\n[capability] {summary['passed']}/{total} passed  {summary['by_cat']}")

    if a.out:
        json.dump({"summary": summary, "rows": rows}, open(a.out, "w"), indent=2)
        print(f"[capability] wrote {a.out}")

    # Any instruction/reasoning failure means base capability regressed -- that is
    # the thing that must block, not persona taste.
    hard = [r for r in rows if not r["pass"] and r["cat"] in ("instruction", "reasoning")]
    if hard:
        print(f"[capability] FAIL: {len(hard)} base-capability probe(s) regressed",
              file=sys.stderr)
        return 1
    if failed:
        print("[capability] persona probes flagged but base capability intact")
    return 0


if __name__ == "__main__":
    sys.exit(main())
