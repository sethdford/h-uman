#!/usr/bin/env python3
"""Tests for conversation_arena.py pure helpers.

Pins: DPO-row extraction (only flagged turns with a judge rewrite and a real
margin become training pairs — synthetic data entering the production learning
loop must clear a quality bar), scoreboard trend math, and scenario validation.

Run: python3 scripts/test_conversation_arena.py
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import conversation_arena as ca  # noqa: E402


def _judgment(turns):
    return {"turns": turns, "overall_humanness": 0.8, "summary": "s"}


def test_dpo_rows_only_from_flagged_turns_with_rewrite():
    transcript = [
        {"role": "them", "text": "yo what's up"},
        {"role": "seth", "text": "Certainly! I can help with that."},
        {"role": "them", "text": "lol ok"},
        {"role": "seth", "text": "haha all good"},
    ]
    turns = [
        {"turn_index": 1, "score": 0.2, "ai_tells": ["assistant-opener"],
         "better_reply": "not much, chillin", "confidence": 0.9},
        {"turn_index": 3, "score": 0.95, "ai_tells": [], "better_reply": "",
         "confidence": 0.9},
    ]
    rows = ca.build_dpo_rows(_judgment(turns), transcript, min_margin=0.3)
    assert len(rows) == 1
    r = rows[0]
    assert r["rejected"] == "Certainly! I can help with that."
    assert r["chosen"] == "not much, chillin"
    assert "yo what's up" in r["prompt"]
    assert r["margin"] > 0
    assert r["source"] == "arena"


def test_dpo_rows_skip_low_margin_and_low_confidence():
    transcript = [{"role": "them", "text": "hey"}, {"role": "seth", "text": "hey"}]
    # margin below threshold (score 0.75 -> margin 0.25 < 0.3)
    t1 = [{"turn_index": 1, "score": 0.75, "ai_tells": ["x"],
           "better_reply": "yo", "confidence": 0.9}]
    assert ca.build_dpo_rows(_judgment(t1), transcript, min_margin=0.3) == []
    # low judge confidence
    t2 = [{"turn_index": 1, "score": 0.1, "ai_tells": ["x"],
           "better_reply": "yo", "confidence": 0.4}]
    assert ca.build_dpo_rows(_judgment(t2), transcript, min_margin=0.3) == []
    # no rewrite offered
    t3 = [{"turn_index": 1, "score": 0.1, "ai_tells": ["x"],
           "better_reply": "", "confidence": 0.9}]
    assert ca.build_dpo_rows(_judgment(t3), transcript, min_margin=0.3) == []


def test_dpo_rows_ignore_out_of_range_or_them_turns():
    transcript = [{"role": "them", "text": "hey"}, {"role": "seth", "text": "hey"}]
    turns = [{"turn_index": 0, "score": 0.1, "ai_tells": ["x"],
              "better_reply": "yo", "confidence": 0.9},   # index 0 is THEM
             {"turn_index": 9, "score": 0.1, "ai_tells": ["x"],
              "better_reply": "yo", "confidence": 0.9}]   # out of range
    assert ca.build_dpo_rows(_judgment(turns), transcript, min_margin=0.3) == []


def test_scoreboard_trend():
    rows = [{"overall_humanness": 0.6}, {"overall_humanness": 0.7},
            {"overall_humanness": 0.9}]
    t = ca.scoreboard_trend(rows, window=2)
    assert t["n"] == 3
    assert abs(t["recent_mean"] - 0.8) < 1e-9
    assert abs(t["all_mean"] - (0.6 + 0.7 + 0.9) / 3) < 1e-9
    assert ca.scoreboard_trend([], window=5)["n"] == 0


def test_scenario_validation():
    ok = {"id": "a", "persona": "college buddy", "goal": "banter about game",
          "opener": "yo did you watch"}
    assert ca.validate_scenario(ok) == []
    assert ca.validate_scenario({"id": "b"})  # missing fields


def test_all_scenarios_pass_precheck():
    # Acceptance (a): every shipped scenario clears validate_scenario, so the
    # arena precheck (which aborts on any error) never trips on the library.
    errs = [e for sc in ca.SCENARIOS for e in ca.validate_scenario(sc)]
    assert errs == [], errs


def test_humor_scenarios_present_and_valid():
    # The teasing/joking register must be represented and well-formed.
    assert len(ca.HUMOR_SCENARIO_IDS) >= 4
    for sc in ca.SCENARIOS:
        if sc.get("register") == "humor":
            assert ca.validate_scenario(sc) == []
    # ids are unique across the whole library
    ids = [sc["id"] for sc in ca.SCENARIOS]
    assert len(ids) == len(set(ids))


def test_humor_axis_in_judge_schema():
    # The measurement contract: 'humor' is both a property and REQUIRED, so the
    # structured-output judge cannot silently omit it (which would collapse
    # every scoreboard row's humor to the 0.0 fallback).
    assert "humor" in ca.JUDGE_SCHEMA["properties"]
    assert "humor" in ca.JUDGE_SCHEMA["required"]


def test_axis_spread():
    rows = [{"humor": 0.9}, {"humor": 0.3}, {"humor": 0.6}]
    s = ca.axis_spread(rows, "humor")
    assert s["n"] == 3
    assert abs(s["mean"] - 0.6) < 1e-9
    assert s["min"] == 0.3 and s["max"] == 0.9
    assert abs(s["spread"] - 0.6) < 1e-9


def test_axis_spread_skips_missing_key_and_handles_empty():
    # rows without the key are skipped (old scoreboard rows predate the axis)
    rows = [{"humor": 0.5}, {"overall_humanness": 0.8}, {"humor": 0.5}]
    s = ca.axis_spread(rows, "humor")
    assert s["n"] == 2 and s["spread"] == 0.0
    empty = ca.axis_spread([], "humor")
    assert empty["n"] == 0 and empty["spread"] == 0.0


def main():
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in fns:
        fn()
        print(f"PASS {fn.__name__}")
    print(f"--- {len(fns)}/{len(fns)} passed ---")
    return 0


if __name__ == "__main__":
    sys.exit(main())
