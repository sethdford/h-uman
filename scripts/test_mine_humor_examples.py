#!/usr/bin/env python3
"""Tests for mine_humor_examples.py pure classifiers.

Step 1 of the persona quirk/teasing effort (2026-07-21). Pins the teasing/
humor detector and the PII scrub BEFORE any real chat.db text is mined into
the persona — a miner that mislabels or leaks is worse than none, because its
output goes straight into what the daemon texts real people.

Run: python3 scripts/test_mine_humor_examples.py
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mine_humor_examples as m  # noqa: E402


def test_teasing_and_humor_signals_fire():
    # Playful roast / sarcasm — the register the user asked for.
    assert m.humor_signal("shut up it was iconic and you know it")
    assert m.humor_signal("does walking to the fridge count lol")
    assert m.humor_signal("oh you're SO productive huh")
    assert m.humor_signal("lmaooo no way you actually did that")
    assert m.humor_signal("nerd")
    assert m.humor_signal("wow ok calm down there champ")


def test_ai_leak_apologies_are_rejected():
    # The daemon's OWN leak-apologies pollute the corpus. They must NEVER be
    # mined as humor — feeding them back teaches the persona to say them.
    assert m.scrub("lol sorry that was my AI responding") is None
    assert m.scrub("my AI sorry") is None
    assert m.scrub("sorry my AI is still on") is None
    assert m.scrub("haha the AI jumping in again") is None


def test_acronyms_do_not_read_as_sarcasm():
    # USA / AI / CI / DTCC are not mock-emphatic caps.
    assert not m.humor_signal("USA USA")
    assert not m.humor_signal("any noise over there about the DTCC")
    assert not m.humor_signal("working on the AI")


def test_flat_or_logistical_replies_do_not_fire():
    # These are real Seth texts but carry no teasing/humor — must NOT be mined
    # as humor exemplars or the bank fills with noise.
    assert not m.humor_signal("yeah sounds good")
    assert not m.humor_signal("i'll get it scheduled")
    assert not m.humor_signal("got the measuring tape")
    assert not m.humor_signal("how did it go?")
    assert not m.humor_signal("")


def test_scrub_drops_pii_bearing_text():
    # Digits (phone/address/amounts) → reject the whole exemplar.
    assert m.scrub("call me at 8015551234 lol") is None
    assert m.scrub("meet at 123 main st haha") is None
    # A clean teasing line passes through unchanged.
    assert m.scrub("shut up nerd") == "shut up nerd"
    # Over-long lines (not a quick text) are rejected.
    assert m.scrub("x " * 60) is None


def test_build_example_shape_matches_persona_bank():
    ex = m.build_example("did you work out today", "does walking to the fridge count")
    assert ex["incoming"] == "did you work out today"
    assert ex["response"] == "does walking to the fridge count"
    assert "context" in ex  # persona example_banks require the key


def test_dedup_keeps_first_drops_repeat_response():
    rows = [
        m.build_example("a", "shut up nerd"),
        m.build_example("b", "shut up nerd"),   # same response → drop
        m.build_example("c", "does walking to the fridge count"),
    ]
    out = m.dedup(rows)
    assert len(out) == 2
    assert out[0]["response"] == "shut up nerd"


def main():
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in fns:
        fn()
        print(f"PASS {fn.__name__}")
    print(f"--- {len(fns)}/{len(fns)} passed ---")


if __name__ == "__main__":
    main()
