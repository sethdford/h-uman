#!/usr/bin/env python3
"""
Layer 4 / Month 5 — Multimodal policy: tapback-vs-text, voice-vs-text, GIF.

A real iMessage pro picks the modality that fits the moment:
  - "k" → tapback 👍 (don't waste a message bubble on acknowledgment)
  - emotional moment → maybe a voice memo (humans send those when typing
    feels insufficient)
  - hyped-up reaction → GIF (cultural fluency)
  - everything else → text

Today h-uman always picks text. That's a tell — humans don't.

This module provides a deterministic, testable POLICY CLASSIFIER that
takes the incoming message + conversation context and produces:

  {modality: "tapback" | "text" | "voice" | "gif",
   tapback_kind: "love" | "like" | "laugh" | "emphasize" | "question",
   confidence: float in [0, 1],
   reason: str}

Per security-predicate-extraction.md (~/.claude/rules/): the decision
is a pure predicate, not coupled to the send path, so it can be unit-
tested without spawning a channel.

Per audit-verify-before-allege.md: we ship the predicate + its tests
BEFORE wiring it into iMessage send path. Layer 5 (verifier TTT) can
then learn over modality choice as part of best-of-N.

Wiring (M5+, not in this commit):
  In src/channels/imessage.c, before render-and-send, call:
    hu_multimodal_policy_decide(incoming, history) → decision
  If decision.modality == "tapback" with confidence > 0.7:
    send tapback via existing react() vtable; suppress text
  If decision.modality == "voice" with confidence > 0.8:
    queue voice synthesis (future); for now, fall through to text
    + log policy decision to dpo_pairs as "would-have-been-voice"
  If decision.modality == "gif" with confidence > 0.7:
    select GIF from curated bank; send via attachment path
  Else: send text as today.

Usage:
  python3 scripts/multimodal_policy.py --eval eval_suites/imessage_humanness.json
  python3 scripts/multimodal_policy.py --decide "lol"
"""

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Optional


# --- Feature extractors ---

LAUGH_RE = re.compile(r"\b(lol|lmao|rofl|lmfao|haha+|hehe+|😂|🤣|💀)\b", re.IGNORECASE)
LOVE_RE = re.compile(r"(\blove\b|❤️|💕|🥰|\bproud of (you|u)\b|\bappreciate (you|u|that|this)\b)",
                     re.IGNORECASE)
ACK_TOKENS = (r"(?:k|kk|ok|okay|cool|got it|sounds good|sg|gotcha|thx|thanks|"
              r"ty|yep|yup|yes|no|nope|same|word|bet|fr)")
# Accepts a single ack token OR two ack tokens separated by space
# ("cool got it", "ok cool", "thx bet"). All trailing punctuation tolerated.
ACK_RE = re.compile(rf"^{ACK_TOKENS}(?:\s+{ACK_TOKENS})?[\.\!]?$",
                    re.IGNORECASE)
QUESTION_TRAIL_RE = re.compile(r"\?\s*$")
EMPHASIS_RE = re.compile(r"\b(huge|massive|incredible|wild|insane|crazy|"
                         r"unbelievable|amazing|incredible)\b", re.IGNORECASE)
EMOTIONAL_HEAVY = re.compile(r"\b(sorry|grief|loss|miss(ing)?|love you|"
                             r"missing you|hurt|pain|sad|crying|tears)\b",
                             re.IGNORECASE)
HYPED_RE = re.compile(r"\b(let'?s go|lfg|yesss+|fire|goat|🔥|🚀|💪|🙌)\b",
                      re.IGNORECASE)


def _strip(s: str) -> str:
    return s.strip() if s else ""


def decide(incoming: str, history: Optional[list] = None) -> dict:
    """Return modality decision for one incoming message.

    Args:
      incoming: the message we'd respond TO.
      history: list of prior turns (optional).

    Returns:
      {modality, tapback_kind, confidence, reason}
    """
    inc = _strip(incoming)
    if not inc:
        return {"modality": "text", "tapback_kind": None,
                "confidence": 1.0, "reason": "empty-incoming"}

    history = history or []

    # 1. Acknowledgment-class incoming — Seth often replies with "k" or
    #    tapback. Use tapback for very short ack-shaped if it's the END
    #    of a thread (last message we'd say to wrap).
    if ACK_RE.match(inc) and len(inc) <= 16:
        # Probably "got it"/"ok" — best response is tapback 👍, NOT text
        return {"modality": "tapback", "tapback_kind": "like",
                "confidence": 0.85,
                "reason": "incoming-is-ack-tapback-suffices"}

    # 2. Loaded question — text only. Never tapback ?.
    if QUESTION_TRAIL_RE.search(inc):
        # Was it short? Then short text response.
        return {"modality": "text", "tapback_kind": None,
                "confidence": 0.95, "reason": "explicit-question"}

    # 3. Laugh-trigger — tapback 😂 fits the moment IF the response
    #    would otherwise be one of "lol", "hahaha", "💀". When the
    #    incoming itself is funny content (not a "you're funny" reaction),
    #    sending a haha-tapback is the move.
    if LAUGH_RE.search(inc):
        # If the LAUGH appears in the incoming, that's a setup line —
        # we should respond with text (build on the joke) not tapback.
        # Only tapback if WE are the one being told something funny.
        return {"modality": "text", "tapback_kind": None,
                "confidence": 0.6,
                "reason": "incoming-contains-laughter-build-don-t-react"}

    # 4. Emotional content — never tapback. Voice memo if very intense,
    #    otherwise text.
    if EMOTIONAL_HEAVY.search(inc):
        if any(w in inc.lower() for w in ["grief", "loss", "miss you", "love you"]):
            return {"modality": "voice", "tapback_kind": None,
                    "confidence": 0.65,
                    "reason": "deep-emotional-content-voice-conveys-more"}
        return {"modality": "text", "tapback_kind": None,
                "confidence": 0.9,
                "reason": "emotional-content-text-not-reaction"}

    # 5. Hyped/celebratory — GIF fits the cultural moment.
    if HYPED_RE.search(inc):
        return {"modality": "gif", "tapback_kind": None,
                "confidence": 0.7,
                "reason": "hyped-celebratory-gif-conveys-energy"}

    # 6. "I'm proud of you" / love / appreciation incoming — Seth
    #    typically reacts with tapback ❤️.
    if LOVE_RE.search(inc) and len(inc) < 50:
        return {"modality": "tapback", "tapback_kind": "love",
                "confidence": 0.75,
                "reason": "short-appreciative-incoming-love-tapback"}

    # 7. Emphasis triggers — exclamation-heavy claim. tapback ‼️ if
    #    short, text if longer.
    if EMPHASIS_RE.search(inc) and len(inc) < 40:
        return {"modality": "tapback", "tapback_kind": "emphasize",
                "confidence": 0.6,
                "reason": "short-emphatic-incoming-double-emphasis"}

    # 8. Default — text.
    return {"modality": "text", "tapback_kind": None,
            "confidence": 0.95, "reason": "default-text-fits"}


# --- Test cases for the predicate ---
GOLDEN = [
    ("k", "tapback", "like"),
    ("ok", "tapback", "like"),
    ("cool got it", "tapback", "like"),
    ("you free saturday?", "text", None),
    ("haha that's wild", "text", None),  # build on joke, don't react
    ("love you so much", "voice", None),
    ("I'm so proud of you", "tapback", "love"),  # short + appreciative
    ("lfg let's go!!!", "gif", None),
    ("that's insane", "tapback", "emphasize"),
    ("hey what's up", "text", None),
    ("", "text", None),
]


def run_self_test() -> int:
    passed = 0
    failed = 0
    for inc, expect_mod, expect_kind in GOLDEN:
        d = decide(inc)
        ok = d["modality"] == expect_mod
        if expect_kind:
            ok = ok and d["tapback_kind"] == expect_kind
        if ok:
            passed += 1
        else:
            failed += 1
            print(f"  FAIL: {inc!r} -> {d} (expected modality={expect_mod}, "
                  f"kind={expect_kind})")
    print(f"\nself-test: {passed}/{passed + failed} pass")
    return 0 if failed == 0 else 1


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--decide", help="Single incoming message to classify")
    p.add_argument("--eval", help="Eval suite JSON; classify each prompt")
    p.add_argument("--self-test", action="store_true", help="Run golden cases")
    args = p.parse_args()
    if args.self_test:
        sys.exit(run_self_test())
    if args.decide:
        d = decide(args.decide)
        print(json.dumps(d, indent=2))
        return
    if args.eval:
        suite = json.loads(Path(args.eval).read_text())
        for t in suite.get("tasks", []):
            inc = t.get("prompt", "")
            d = decide(inc)
            print(f"[{t.get('id')}] {inc[:60]!r:64} → {d['modality']:<8} "
                  f"({d['tapback_kind'] or '-':<10}) conf={d['confidence']:.2f}")
        return
    p.print_help()


if __name__ == "__main__":
    main()
