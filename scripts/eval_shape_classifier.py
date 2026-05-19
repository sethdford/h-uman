#!/usr/bin/env python3
"""
Deterministic response-shape classifier for eval responses.

The LLM-judge has two noise sources:
  (1) judge non-determinism — flips pass/fail on the same response
  (2) MLX provider dropouts — returns NULL/empty when overloaded

Both contaminate pass-rate as a metric. This script provides a
deterministic shape-classifier that ignores both noise sources:

  PASS if response is in-voice for iMessage (short, no markdown,
       no AI-assistant openers, no "Depending on" / "Options" tropes)
  FAIL if response has any of the canonical AI-assistant tells

The classifier is the operational form of seth.json's anti_patterns,
turned into a regex-based gate. It correlates with the LLM-judge on
unambiguous responses and provides signal where the judge gives noise.

Usage:
  python3 scripts/eval_shape_classifier.py                  # re-score all suites
  python3 scripts/eval_shape_classifier.py --suite imessage-humanness
  python3 scripts/eval_shape_classifier.py --compare-runs 4 5

The shape classifier is for iMessage / texting-style channels. Other
channels (Slack, email) have different shape expectations and need a
separate classifier — out of scope for v1.
"""

import argparse
import json
import re
import sqlite3
import sys
from collections import defaultdict
from pathlib import Path

DB_PATH = Path.home() / ".human" / "memory.db"

# Anti-patterns lifted from ~/.human/personas/seth.json::anti_patterns and
# the canonical "AI assistant offering options" failure mode from the
# 2026-05-18 audit. Each is a (regex, name) pair for explainable
# attribution when a response fails.
AI_ASSISTANT_PATTERNS = [
    (re.compile(r"^\s*Depending on", re.IGNORECASE), "depending-on opener"),
    (re.compile(r"^\s*Here are (a few|some|several|the)", re.IGNORECASE), "here-are-options opener"),
    (re.compile(r"^\s*Certainly[\s,.!]", re.IGNORECASE), "certainly opener"),
    (re.compile(r"^\s*Absolutely[\s,.!]", re.IGNORECASE), "absolutely opener"),
    (re.compile(r"^\s*Of course[\s,.!]", re.IGNORECASE), "of-course opener"),
    (re.compile(r"^\s*I (appreciate|understand|hear you)\b", re.IGNORECASE), "i-understand opener"),
    (re.compile(r"\bI am here (to|for)\b", re.IGNORECASE), "i-am-here-to-support"),
    (re.compile(r"\b(That|This) sounds like\b", re.IGNORECASE), "that-sounds-like"),
    (re.compile(r"\bgreat question\b", re.IGNORECASE), "great-question"),
]

# Markdown that real iMessage texts never contain.
MARKDOWN_PATTERNS = [
    (re.compile(r"^\s*[\*\-]\s+", re.MULTILINE), "bullet-list"),
    (re.compile(r"^\s*\d+\.\s+", re.MULTILINE), "numbered-list"),
    (re.compile(r"^#{1,6}\s+", re.MULTILINE), "header"),
    (re.compile(r"\*\*[^\*\n]{2,}\*\*"), "bold-markdown"),
    (re.compile(r"^-{3,}\s*$", re.MULTILINE), "horizontal-rule"),
    (re.compile(r"```"), "code-fence"),
]

# Length thresholds for iMessage. Real human texts on iMessage are
# almost always under 250 chars (per persona overlay: "Default 5-15
# words. When deep: 20-60 words across 1-2 sentences").
TOO_LONG_CHARS = 250
WAY_TOO_LONG_CHARS = 500


def classify(response: str) -> dict:
    """Return {pass, score, fails} for a response under iMessage shape rules.

    score is in [0, 1] where 1 = perfect in-voice, 0 = canonical AI-assistant.
    fails is a list of named violations for explainability.
    """
    if response is None:
        return {"pass": False, "score": 0.0, "fails": ["null-response"], "len": 0}
    response = response.strip()
    if not response:
        return {"pass": False, "score": 0.0, "fails": ["empty-response"], "len": 0}

    fails = []

    # Length checks
    n = len(response)
    if n > WAY_TOO_LONG_CHARS:
        fails.append(f"way-too-long ({n} chars)")
    elif n > TOO_LONG_CHARS:
        fails.append(f"too-long ({n} chars)")

    # AI-assistant tells
    for pat, name in AI_ASSISTANT_PATTERNS:
        if pat.search(response):
            fails.append(name)

    # Markdown tells
    for pat, name in MARKDOWN_PATTERNS:
        if pat.search(response):
            fails.append(name)

    # Score: start at 1.0, subtract per-fail penalty, clamp to [0, 1]
    # Heavy violations (way-too-long, markdown lists) get 0.3 each.
    # Light violations (mild openers) get 0.15 each.
    heavy = {"way-too-long", "bullet-list", "numbered-list", "header", "code-fence"}
    score = 1.0
    for f in fails:
        if any(f.startswith(h) for h in heavy):
            score -= 0.3
        else:
            score -= 0.15
    score = max(0.0, score)

    return {
        "pass": score >= 0.7 and not any(f.startswith(("bullet-list", "numbered-list", "way-too-long"))
                                          for f in fails),
        "score": round(score, 3),
        "fails": fails,
        "len": n,
    }


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--suite", default=None,
                   help="filter to a single suite name (default: all)")
    p.add_argument("--compare-runs", nargs=2, type=int, metavar=("OLD", "NEW"),
                   help="show per-task before/after comparison between two run_ids")
    p.add_argument("--verbose", "-v", action="store_true",
                   help="show every failure with attribution")
    args = p.parse_args()

    con = sqlite3.connect(DB_PATH)
    con.row_factory = sqlite3.Row

    if args.compare_runs:
        run_a, run_b = args.compare_runs
        print(f"\n=== Comparing run {run_a} (BEFORE) vs run {run_b} (AFTER) ===\n")
        rows_a = {r["task_id"]: r for r in con.execute(
            "SELECT task_id, passed, actual_output FROM eval_results WHERE run_id=?", (run_a,))}
        rows_b = {r["task_id"]: r for r in con.execute(
            "SELECT task_id, passed, actual_output FROM eval_results WHERE run_id=?", (run_b,))}
        for tid in sorted(set(rows_a) | set(rows_b)):
            ra = rows_a.get(tid)
            rb = rows_b.get(tid)
            ca = classify(ra["actual_output"]) if ra else {"pass": None, "score": None, "fails": [], "len": 0}
            cb = classify(rb["actual_output"]) if rb else {"pass": None, "score": None, "fails": [], "len": 0}
            print(f"{tid}:")
            print(f"  BEFORE: shape_pass={ca['pass']} score={ca['score']} len={ca['len']} judge_pass={ra['passed'] if ra else '-'}")
            if args.verbose and ca['fails']:
                print(f"    fails: {ca['fails']}")
            print(f"  AFTER:  shape_pass={cb['pass']} score={cb['score']} len={cb['len']} judge_pass={rb['passed'] if rb else '-'}")
            if args.verbose and cb['fails']:
                print(f"    fails: {cb['fails']}")
        return

    # Aggregate per-run shape vs judge
    q = "SELECT er.id, er.suite_name FROM eval_runs er"
    where = []
    params = []
    if args.suite:
        where.append("er.suite_name = ?")
        params.append(args.suite)
    if where:
        q += " WHERE " + " AND ".join(where)
    q += " ORDER BY er.id"
    runs = con.execute(q, params).fetchall()

    print(f"{'run':>4} {'suite':<22} {'judge%':>6} {'shape%':>6} {'mean_score':>10} {'non_null':>9}")
    print("-" * 70)
    for r in runs:
        run_id = r["id"]
        suite = r["suite_name"]
        results = con.execute(
            "SELECT task_id, passed, actual_output FROM eval_results WHERE run_id=?",
            (run_id,)).fetchall()
        if not results:
            continue
        total = len(results)
        judge_pass = sum(1 for rr in results if rr["passed"])
        shape_classifications = [classify(rr["actual_output"]) for rr in results]
        shape_pass = sum(1 for c in shape_classifications if c["pass"])
        mean_score = sum(c["score"] for c in shape_classifications) / total
        non_null = sum(1 for rr in results if rr["actual_output"])
        print(f"{run_id:>4} {suite:<22} {100*judge_pass/total:>5.1f}% {100*shape_pass/total:>5.1f}% "
              f"{mean_score:>10.3f} {non_null:>5}/{total}")


if __name__ == "__main__":
    main()
