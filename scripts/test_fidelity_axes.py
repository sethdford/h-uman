#!/usr/bin/env python3
"""Tests for fidelity_axes.py (W7-2)."""

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import fidelity_axes as fx

CASUAL = [
    "yeah for a bit, whats up",
    "nah not yet, gonna grab something soon tbh",
    "lol fair, lemme know",
    "idk maybe later, kinda busy rn",
    "haha yeah thats the plan",
]
FORMAL = [
    "Yes, I am available this evening. How may I assist you?",
    "I have not yet eaten; I intend to do so shortly.",
    "That is a reasonable point. Please advise me of your preference.",
    "I am currently occupied but can revisit this matter later today.",
    "Certainly. I will proceed accordingly and confirm once complete.",
]


def test_message_features_casual_vs_formal():
    c = fx.message_features("idk lol gonna grab food rn")
    f = fx.message_features("I am currently unavailable.")
    assert c["abbrev_ratio"] > f["abbrev_ratio"]
    assert c["lower_ratio"] > f["lower_ratio"]


def test_identical_corpora_score_high():
    r = fx.decompose(CASUAL, CASUAL)
    assert r["aggregate"] > 0.9, r
    for axis, score in r["axes"].items():
        assert score > 0.8, (axis, score)


def test_formal_model_against_casual_ref_flags_axes():
    r = fx.decompose(CASUAL, FORMAL)
    assert r["aggregate"] < 0.75, r
    weak = {w["axis"] for w in r["weakest_axes"]}
    # casing, abbreviation, or length should surface as a gap
    assert weak & {"casing", "abbreviation", "length_rhythm", "vocabulary"}, weak


def test_vocabulary_axis_rewards_shared_content_words():
    ref = ["lunch plans tomorrow at the office downtown"]
    same = ["lunch plans tomorrow at the office downtown"]
    diff = ["quantum entanglement violates classical locality assumptions"]
    assert fx.decompose(ref, same)["axes"]["vocabulary"] > fx.decompose(ref, diff)["axes"]["vocabulary"]


def test_load_messages_jsonl_and_plain():
    d = Path(tempfile.mkdtemp())
    j = d / "m.jsonl"
    j.write_text('{"text":"hello there"}\n{"response":"second one"}\n')
    p = d / "m.txt"
    p.write_text("plain line one\nplain line two\n")
    assert fx.load_messages(j) == ["hello there", "second one"]
    assert fx.load_messages(p) == ["plain line one", "plain line two"]


def test_contrastive_uniqueness_rewards_target_match():
    # model echoes the casual target → distinctive vs the formal "other" baseline
    u = fx.contrastive_uniqueness(CASUAL, FORMAL, CASUAL)
    assert u["distinctive"] is True, u
    assert u["uniqueness"] > 0.5, u


def test_contrastive_uniqueness_penalizes_generic():
    # model talks like the FORMAL "other" baseline → not distinctive to the casual target
    u = fx.contrastive_uniqueness(CASUAL, FORMAL, FORMAL)
    assert u["distinctive"] is False, u
    assert u["uniqueness"] < 0.5, u


def test_multiturn_consistency_stable_vs_drift():
    # Stable: uniform lowercase, no emoji/abbrev variance → near-zero drift.
    stable = [
        "that sounds great thanks so much",
        "really sounds great thanks",
        "great that sounds really nice",
        "thanks that sounds great",
    ]
    # Drift: voice swings across turns (casing, abbreviation, emoji all vary wildly).
    drift = [
        "sounds great",
        "WOW THAT IS TRULY AMAZING",
        "lol 😂😂😂 idk tbh ngl fr",
        "I remain quite formal in this matter.",
    ]
    s = fx.multiturn_consistency(stable)
    d = fx.multiturn_consistency(drift)
    assert s["consistency"] > d["consistency"], (s, d)
    assert fx.multiturn_consistency(["one turn"])["consistency"] == 1.0


def main():
    tests = [
        test_message_features_casual_vs_formal,
        test_identical_corpora_score_high,
        test_formal_model_against_casual_ref_flags_axes,
        test_vocabulary_axis_rewards_shared_content_words,
        test_load_messages_jsonl_and_plain,
        test_contrastive_uniqueness_rewards_target_match,
        test_contrastive_uniqueness_penalizes_generic,
        test_multiturn_consistency_stable_vs_drift,
    ]
    print("Testing fidelity_axes.py")
    print("=" * 60)
    p = f = 0
    for t in tests:
        try:
            t()
            print(f"✓ {t.__name__}")
            p += 1
        except AssertionError as e:
            print(f"✗ {t.__name__}: {e}")
            f += 1
        except Exception as e:  # noqa: BLE001
            print(f"✗ {t.__name__}: {type(e).__name__}: {e}")
            f += 1
    print("=" * 60)
    print(f"Results: {p} passed, {f} failed")
    return 0 if f == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
