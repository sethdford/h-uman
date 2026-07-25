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


def test_humor_probe_scenarios_exist_and_validate():
    probes = [sc for sc in ca.SCENARIOS if sc.get("humor_probe")]
    assert len(probes) >= 4
    for sc in probes:
        assert ca.validate_scenario(sc) == []
    # The anti-forced-humor probe must be present: without a scenario where
    # joking is WRONG, the humor axis only ever rewards more jokes.
    assert any(sc["id"] == "humor_wrong_moment" for sc in probes)


def test_judge_schema_requires_both_humor_signals():
    # A single 'humor' score is gameable (more jokes = higher). The forced
    # counter-signal must be required of the judge, not optional.
    assert "humor" in ca.JUDGE_SCHEMA["required"]
    assert "humor_forced" in ca.JUDGE_SCHEMA["required"]


def test_scoreboard_trend_accepts_axis_key():
    rows = [{"humor": 0.2}, {"humor": 0.8}]
    t = ca.scoreboard_trend(rows, window=2, key="humor")
    assert abs(t["all_mean"] - 0.5) < 1e-9
    # default key unchanged (back-compat with existing callers)
    assert ca.scoreboard_trend([{"overall_humanness": 0.4}])["all_mean"] == 0.4


def _arm(tag, humor, voice, forced=0.1, humanness=0.8):
    return {"tag": tag, "humor": humor, "voice_consistency": voice,
            "humor_forced": forced, "overall_humanness": humanness,
            "engagement": 0.7}


def test_arm_summary_filters_by_tag_and_means():
    rows = [_arm("off", 0.4, 0.8), _arm("off", 0.6, 0.6), _arm("live", 0.9, 0.7)]
    off = ca.arm_summary(rows, "off")
    assert off["n"] == 2
    assert abs(off["humor"] - 0.5) < 1e-9
    assert abs(off["voice_consistency"] - 0.7) < 1e-9
    assert ca.arm_summary(rows, "nope")["n"] == 0


def test_compare_arms_promotes_on_real_gain():
    rows = [_arm("off", 0.40, 0.80), _arm("live", 0.70, 0.80)]
    d = ca.compare_arms(rows, "off", "live")
    assert d["promote"] is True
    assert d["humor_gain"] > 0.05


def test_compare_arms_vetoes_when_voice_regresses():
    """Forced humor's signature: humor climbs, voice pays for it. Must NOT promote."""
    rows = [_arm("off", 0.40, 0.85), _arm("live", 0.90, 0.60)]
    d = ca.compare_arms(rows, "off", "live")
    assert d["promote"] is False
    assert any("voice regressed" in r for r in d["reasons"])


def test_compare_arms_vetoes_on_forced_humor_even_when_voice_holds():
    rows = [_arm("off", 0.40, 0.80, forced=0.05),
            _arm("live", 0.80, 0.80, forced=0.60)]
    d = ca.compare_arms(rows, "off", "live")
    assert d["promote"] is False
    assert any("humor_forced" in r for r in d["reasons"])


def test_compare_arms_vetoes_on_insufficient_gain_and_missing_arm():
    rows = [_arm("off", 0.70, 0.80), _arm("live", 0.71, 0.80)]
    assert ca.compare_arms(rows, "off", "live")["promote"] is False
    # no live rows at all -> cannot decide, must not promote
    d = ca.compare_arms([_arm("off", 0.4, 0.8)], "off", "live")
    assert d["promote"] is False
    assert any("missing arm data" in r for r in d["reasons"])


def main():
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in fns:
        fn()
        print(f"PASS {fn.__name__}")
    print(f"--- {len(fns)}/{len(fns)} passed ---")
    return 0


if __name__ == "__main__":
    sys.exit(main())
