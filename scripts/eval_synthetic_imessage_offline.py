#!/usr/bin/env python3
"""
Offline synthetic iMessage-readiness evaluation (no daemon, no API, no hu).

Scores *assistant draft strings* the same way CI can: structural AI-tells,
forbidden channel artifacts, runaway repetition, and single-bubble length.
Use this to prove gates across conversation *types* without waiting for a
real human or a live LLM round-trip.

  python3 scripts/eval_synthetic_imessage_offline.py
  python3 scripts/eval_synthetic_imessage_offline.py --json /tmp/imessage_synth.json

Exit 0 iff every case with expect_pass=True passes and every expect_pass=False fails.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from typing import Any

# ── structural tells (aligned with scripts/eval_humanness.py detect_structural_tells) ──


def detect_structural_tells(response: str) -> list[str]:
    tells: list[str] = []
    if re.search(r"^\d+[.)]\s", response, re.MULTILINE):
        tells.append("numbered list")
    if re.search(r"^[-*]\s", response, re.MULTILINE):
        tells.append("bullet list")
    if "\u2014" in response:
        tells.append("em-dash")
    if re.search(r"^[A-Z][a-z]+:\s", response, re.MULTILINE):
        tells.append("topic-colon pattern")
    if any(p in response.lower() for p in ["first,", "second,", "third,"]):
        tells.append("first/second/third enumeration")
    if any(
        p in response.lower()
        for p in ["in summary", "to summarize", "in conclusion"]
    ):
        tells.append("concluding summary")
    if any(
        p in response.lower()
        for p in ["let me know", "hope this helps", "feel free", "don't hesitate"]
    ):
        tells.append("offer of further help")
    if re.search(
        r"(?i)^(love that|great point|so true|haha yeah)[,.!]?\s+.{20,}",
        response,
    ):
        tells.append("hollow opener then pivot")
    if len(response) > 360:
        tells.append(f"overly long ({len(response)} chars)")
    paragraphs = [p for p in response.split("\n\n") if p.strip()]
    if len(paragraphs) >= 3:
        tells.append(f"addresses {len(paragraphs)}+ topics")
    if "```" in response:
        tells.append("code block in casual message")
    if "**" in response:
        tells.append("markdown bold")
    return tells


FORBIDDEN_IMESSAGE_ARTIFACTS = [
    ("<tool_call", "tool_call markup"),
    ("</tool_call>", "tool_call close"),
    ("<|im_start", "chatml im_start"),
    ("<|im_end", "chatml im_end"),
    ("<|channel", "harmony channel token"),
    ("<|eot", "harmony eot token"),
    ("</s>", "eos sentinel"),
    ("[INST]", "llama inst token"),
    ("<thinking>", "thinking block"),
]


def detect_runaway_repetition(text: str, min_unit: int = 8, min_repeats: int = 5) -> str | None:
    """Return reason string if a substring repeats many times (quote-loop class)."""
    n = len(text)
    if n < min_unit * min_repeats:
        return None
    # Sliding window: cheap O(n * widths) for small offline harness.
    for width in range(min_unit, min(80, n // min_repeats) + 1):
        i = 0
        while i + width * min_repeats <= n:
            chunk = text[i : i + width]
            if len(chunk.strip()) < 4:
                i += 1
                continue
            reps = 1
            j = i + width
            while j + width <= n and text[j : j + width] == chunk:
                reps += 1
                j += width
            if reps >= min_repeats:
                return f"runaway repetition ({reps}x {width!r} chars)"
            i += 1
    return None


def imessage_gates(
    assistant_text: str,
    *,
    strict_tells: bool = True,
    max_chars: int = 4095,
) -> dict[str, Any]:
    """
    strict_tells: when True (default for iMessage-shaped eval), any structural tell fails.
    """
    reasons: list[str] = []
    ok = True

    if len(assistant_text) > max_chars:
        ok = False
        reasons.append(f"over imessage single-message cap ({len(assistant_text)} > {max_chars})")

    for needle, label in FORBIDDEN_IMESSAGE_ARTIFACTS:
        if needle.lower() in assistant_text.lower():
            ok = False
            reasons.append(f"forbidden artifact: {label}")

    rep = detect_runaway_repetition(assistant_text)
    if rep:
        ok = False
        reasons.append(rep)

    tells = detect_structural_tells(assistant_text)
    if strict_tells and tells:
        ok = False
        reasons.extend([f"structural tell: {t}" for t in tells])

    return {
        "pass": ok,
        "reasons": reasons,
        "structural_tells": tells,
        "char_len": len(assistant_text),
    }


# Synthetic cases: conversation *type* + last user context + assistant draft.
# expect_pass: whether the harness should accept the draft for send-pipeline readiness.

SYNTHETIC_CASES: list[dict[str, Any]] = [
    {
        "id": "casual_ok",
        "conversation_type": "casual_friend",
        "user_style": "lowercase_short",
        "user_last": "hey you free tonight",
        "assistant": "yeah probably - text me around 7?",
        "expect_pass": True,
    },
    {
        "id": "support_ok",
        "conversation_type": "emotional_support",
        "user_style": "vulnerable_paragraph",
        "user_last": "rough week, barely sleeping",
        "assistant": "ugh that sounds brutal. want to vent or want distraction?",
        "expect_pass": True,
    },
    {
        "id": "emoji_burst_ok",
        "conversation_type": "celebration",
        "user_style": "emoji_heavy",
        "user_last": "WE CLOSED THE DEAL",
        "assistant": "LET'S GOOO that rules",
        "expect_pass": True,
    },
    {
        "id": "unicode_ok",
        "conversation_type": "multilingual_mix",
        "user_style": "mixed_scripts",
        "user_last": "café réservé — 19h00 ok?",
        "assistant": "oui parfait à tout à l'heure",
        "expect_pass": True,
    },
    {
        "id": "logistics_brief_ok",
        "conversation_type": "mundane_logistics",
        "user_style": "direct",
        "user_last": "pickup 6:15 or 6:30?",
        "assistant": "6:30 easier thx",
        "expect_pass": True,
    },
    {
        "id": "bad_tool_call",
        "conversation_type": "casual_friend",
        "user_style": "lowercase_short",
        "user_last": "remind me tomorrow",
        "assistant": 'Sure.<tool_call>{"name":"alarm"}</tool_call>',
        "expect_pass": False,
    },
    {
        "id": "bad_chatml",
        "conversation_type": "technical_question",
        "user_style": "formal",
        "user_last": "what is a websocket",
        "assistant": "<|im_start|>assistant\nA websocket is…",
        "expect_pass": False,
    },
    {
        "id": "bad_runaway_quotes",
        "conversation_type": "casual_friend",
        "user_style": "lowercase_short",
        "user_last": "lol",
        "assistant": '"' * 400,
        "expect_pass": False,
    },
    {
        "id": "bad_markdown_rubric",
        "conversation_type": "casual_friend",
        "user_style": "lowercase_short",
        "user_last": "thoughts?",
        "assistant": "Here is my answer:\n\n1. First point\n2. Second\n3. Third\n\n**Summary**: ok",
        "expect_pass": False,
    },
    {
        "id": "lettered_steps_ok",
        "conversation_type": "technical_question",
        "user_style": "formal",
        "user_last": "quick steps?",
        "assistant": "a) open settings b) tap wifi c) forget network d) reconnect",
        "expect_pass": True,
    },
]


def run_all(strict_tells: bool) -> tuple[list[dict[str, Any]], bool]:
    rows: list[dict[str, Any]] = []
    all_ok = True
    for case in SYNTHETIC_CASES:
        gid = case["id"]
        text = case["assistant"]
        expect = bool(case["expect_pass"])
        gates = imessage_gates(text, strict_tells=strict_tells)
        passed = bool(gates["pass"])
        match = passed == expect
        if not match:
            all_ok = False
        rows.append(
            {
                "id": gid,
                "conversation_type": case.get("conversation_type"),
                "user_style": case.get("user_style"),
                "expect_pass": expect,
                "observed_pass": passed,
                "self_check_ok": match,
                "gates": gates,
                "note": case.get("note"),
            }
        )
    return rows, all_ok


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--no-strict-tells",
        action="store_true",
        help="do not fail on structural tells alone (only artifacts/length/repetition)",
    )
    ap.add_argument("--json", metavar="PATH", help="write full report JSON")
    args = ap.parse_args()

    strict = not args.no_strict_tells
    rows, all_ok = run_all(strict_tells=strict)

    fail = sum(1 for r in rows if not r["self_check_ok"])
    print("Synthetic iMessage offline eval")
    print(f"  cases: {len(rows)}  strict_tells: {strict}")
    print(f"  self-check failures: {fail}")
    for r in rows:
        mark = "OK" if r["self_check_ok"] else "MISMATCH"
        print(f"  [{mark}] {r['id']}: expect_pass={r['expect_pass']} observed={r['observed_pass']}")
        if not r["self_check_ok"] or not r["observed_pass"]:
            for reason in r["gates"]["reasons"]:
                print(f"       - {reason}")
        if r["gates"]["structural_tells"]:
            print(f"       tells: {', '.join(r['gates']['structural_tells'][:6])}")

    report = {
        "harness": "eval_synthetic_imessage_offline",
        "strict_tells": strict,
        "all_self_checks_passed": all_ok,
        "cases": rows,
    }
    if args.json:
        os.makedirs(os.path.dirname(os.path.abspath(args.json)) or ".", exist_ok=True)
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)
        print(f"  wrote {args.json}")

    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
