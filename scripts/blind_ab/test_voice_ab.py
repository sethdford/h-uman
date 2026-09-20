"""Tests for voice_ab.py pure helpers. Run: python3 -m pytest scripts/blind_ab/test_voice_ab.py"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import voice_ab as va  # noqa: E402


def _lines(texts):
    return [json.dumps({"text": t}) for t in texts]


def test_usable_texts_filters_length_punctuation_links_and_dupes():
    good = "ok so i thought about it more. honestly that trip sounds amazing, we should do it!"
    lines = _lines([
        good, good,                                   # dupe collapses
        "short.",                                     # too short
        "no terminal punctuation at all in this fairly long sentence about nothing much",
        "check https://example.com for the thing we talked about earlier today ok.",
        "call me at 555-1234 when you land tonight, it should be fine either way.",
    ]) + ["not json", ""]
    out = va.usable_texts(lines)
    assert out == [good]


def test_plan_pairs_round_robins_axes_and_is_deterministic():
    texts = [f"text number {i} is long enough to be spoken aloud nicely, right." for i in range(6)]
    p1, k1 = va.plan_pairs(texts, 6, seed=3)
    p2, k2 = va.plan_pairs(texts, 6, seed=3)
    assert p1 == p2 and k1 == k2
    assert [p["axis"] for p in p1] == ["model", "speed", "prep", "model", "speed", "prep"]
    for p in p1:
        a, b = k1[p["id"]]["A"], k1[p["id"]]["B"]
        assert {a, b} == set(va.AXES[p["axis"]])
        assert p["A"].endswith("_A.caf") and p["B"].endswith("_B.caf")
    # seeded side assignment: not every pair puts variant 1 on A
    sides = [k1[p["id"]]["A"] == va.AXES[p["axis"]][0] for p in p1]
    p3, k3 = va.plan_pairs(texts, 30, seed=3)
    sides3 = [k3[p["id"]]["A"] == va.AXES[p["axis"]][0] for p in p3]
    assert any(sides3) and not all(sides3)
    assert len(sides) == 6


def test_preview_argv_changes_only_the_axis_under_test():
    base = va.preview_argv("human", "seth", "hi there.", "model", "sonic-3", "/tmp/x.caf")
    assert base[:3] == ["human", "voice", "preview"]
    assert "--model" in base and base[base.index("--model") + 1] == "sonic-3"
    assert "--speed" not in base and "--raw" not in base
    sp = va.preview_argv("human", "seth", "hi there.", "speed", "0.95", "/tmp/x.caf")
    assert sp[sp.index("--speed") + 1] == "0.95" and "--model" not in sp
    on = va.preview_argv("human", "seth", "hi there.", "prep", "on", "/tmp/x.caf")
    off = va.preview_argv("human", "seth", "hi there.", "prep", "off", "/tmp/x.caf")
    assert "--raw" not in on and "--raw" in off


def test_wilson_bounds():
    lo, hi = va.wilson(0, 0)
    assert (lo, hi) == (0.0, 1.0)
    lo, hi = va.wilson(10, 10)
    assert lo > 0.7 and hi == 1.0
    lo, hi = va.wilson(5, 10)
    assert lo < 0.5 < hi
    lo, hi = va.wilson(2, 10)
    assert hi < 0.5 or lo < 0.5


def test_score_sheet_reports_preference_per_axis_with_verdict():
    texts = ["a sentence that is long enough to be read aloud by the clone, ok."] * 3
    pairs, key = va.plan_pairs(texts, 9, seed=1)
    # Rater always picks sonic-3.6 on model pairs, always variant 2 on speed, nothing on prep
    for p in pairs:
        if p["axis"] == "model":
            p["choice"] = "A" if key[p["id"]]["A"] == "sonic-3.6" else "B"
        elif p["axis"] == "speed":
            p["choice"] = "A" if key[p["id"]]["A"] == "0.95" else "B"
    res = va.score_sheet(pairs, key)
    assert res["model"]["n"] == 3 and res["model"]["v1_wins"] == 3
    assert res["speed"]["n"] == 3 and res["speed"]["v1_wins"] == 0
    assert res["prep"]["n"] == 0 and res["prep"]["verdict"] == "unmeasured"
    # n=3 is not enough for a CI to clear 0.5 — verdict must stay honest
    assert res["model"]["verdict"] == "no preference yet"
    pairs, key = va.plan_pairs(texts, 45, seed=1)
    for p in pairs:
        if p["axis"] == "model":
            p["choice"] = "A" if key[p["id"]]["A"] == "sonic-3.6" else "B"
    res = va.score_sheet(pairs, key)
    assert res["model"]["n"] == 15 and res["model"]["verdict"] == "sonic-3.6 preferred"


def test_next_unanswered_skips_rated_and_skipped():
    pairs = [{"id": "p01", "choice": "A"}, {"id": "p02"}, {"id": "p03"}]
    assert va.next_unanswered(pairs, ["p02"])["id"] == "p03"
    assert va.next_unanswered(pairs)["id"] == "p02"
    assert va.next_unanswered([{"id": "p01", "choice": "B"}]) is None


def test_compose_question_is_short_and_explains_order():
    q = va.compose_question({"id": "p01"}, 2, 12)
    assert "3/12" in q and "A then B" in q and "reply A or B" in q
    assert len(q) < 300
