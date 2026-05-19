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

# Per-channel length and markdown thresholds. Real human texts on
# iMessage are almost always under 250 chars; Slack tolerates markdown
# and longer threads; email needs greetings and paragraphs.
# 2026-05-18 audit (M5): added per-channel mode so the classifier
# applies the right shape rules for each medium.
CHANNEL_RULES = {
    "imessage": {
        "too_long": 250,
        "way_too_long": 500,
        "markdown_allowed": False,
        "ai_openers_allowed": False,
    },
    "telegram": {
        "too_long": 350,
        "way_too_long": 700,
        "markdown_allowed": False,
        "ai_openers_allowed": False,
    },
    "discord": {
        "too_long": 500,
        "way_too_long": 1200,
        "markdown_allowed": True,   # Discord supports markdown natively
        "ai_openers_allowed": False,
    },
    "slack": {
        "too_long": 800,
        "way_too_long": 2000,
        "markdown_allowed": True,   # Slack workflows use markdown
        "ai_openers_allowed": False,  # still no "Depending on" garbage
    },
    "email": {
        "too_long": 2000,
        "way_too_long": 5000,
        "markdown_allowed": True,
        "ai_openers_allowed": True,  # emails sometimes do open formally
    },
}
# Default = strictest (iMessage). Most eval suites are iMessage-style.
DEFAULT_RULES = CHANNEL_RULES["imessage"]
TOO_LONG_CHARS = DEFAULT_RULES["too_long"]
WAY_TOO_LONG_CHARS = DEFAULT_RULES["way_too_long"]


def classify(response: str, channel: str = "imessage") -> dict:
    """Return {pass, score, fails} for a response under channel-specific shape rules.

    score is in [0, 1] where 1 = perfect in-voice, 0 = canonical AI-assistant.
    fails is a list of named violations for explainability.
    channel: one of CHANNEL_RULES keys (imessage, telegram, discord, slack, email).
    """
    rules = CHANNEL_RULES.get(channel, DEFAULT_RULES)
    too_long = rules["too_long"]
    way_too_long = rules["way_too_long"]
    markdown_allowed = rules["markdown_allowed"]
    ai_openers_allowed = rules["ai_openers_allowed"]
    if response is None:
        return {"pass": False, "score": 0.0, "fails": ["null-response"], "len": 0}
    response = response.strip()
    if not response:
        return {"pass": False, "score": 0.0, "fails": ["empty-response"], "len": 0}

    fails = []

    # Length checks (channel-specific)
    n = len(response)
    if n > way_too_long:
        fails.append(f"way-too-long ({n} chars, channel={channel})")
    elif n > too_long:
        fails.append(f"too-long ({n} chars, channel={channel})")

    # AI-assistant tells (skip for email which sometimes legitimately opens formally)
    if not ai_openers_allowed:
        for pat, name in AI_ASSISTANT_PATTERNS:
            if pat.search(response):
                fails.append(name)

    # Markdown tells (skip for channels that natively support markdown)
    if not markdown_allowed:
        for pat, name in MARKDOWN_PATTERNS:
            if pat.search(response):
                fails.append(name)

    # M5: excessive emoji — count F0 9F xx xx UTF-8 sequences (most
    # emoji code points). Threshold mirrors src/eval/shape.c: flag when
    # emoji count * 30 > response length.
    try:
        utf8 = response.encode("utf-8")
        emoji_count = 0
        i = 0
        while i + 3 < len(utf8):
            if utf8[i] == 0xF0 and utf8[i + 1] == 0x9F:
                emoji_count += 1
                i += 4
            else:
                i += 1
        if len(response) > 0 and emoji_count * 30 > len(response):
            fails.append("excessive-emoji")
    except Exception:
        pass

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
