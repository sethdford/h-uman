#!/usr/bin/env python3
"""Hermetic tests for ab_agent_turns.py / ab_agent_turns_judge.py: no chat.db,
no model, no network.  Run: python3 -m pytest -q scripts/test_ab_agent_turns.py"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ab_agent_turns as ab  # noqa: E402
import ab_agent_turns_judge as jd  # noqa: E402


def test_contexts_match_what_each_ab_is_about():
    long_msg = "x " * 120
    assert ab.keep_context("tot", long_msg, False)
    assert not ab.keep_context("tot", "short one", True)
    planner_msg = "so the landlord wants me to sign another full year and i do not know"
    assert ab.keep_context("planner", planner_msg, True)
    assert not ab.keep_context("planner", planner_msg, False)  # needs Seth's own reply
    assert not ab.keep_context("planner", "hey whats up", True)  # 12 words or fewer
    assert ab.keep_context("beat", "how was the game?", True)
    for kind in ("tot", "planner", "beat"):
        assert not ab.keep_context(kind, "look ￼", True)
        assert not ab.keep_context(kind, "see https://example.com " + long_msg, True)


def test_reset_state_restores_the_snapshot_every_time(tmp_path):
    pristine = tmp_path / "pristine"
    (pristine / "personas").mkdir(parents=True)
    (pristine / "config.json").write_text("{}")
    (pristine / "personas" / "seth.json").write_text('{"a": 1}')
    state = ab.reset_state(str(tmp_path))
    (tmp_path / "state" / "sessions").mkdir()  # what a turn leaves behind
    (tmp_path / "state" / "config.json").write_text("changed")
    state = ab.reset_state(str(tmp_path))
    assert sorted(os.listdir(state)) == ["config.json", "personas"]
    assert open(os.path.join(state, "config.json")).read() == "{}"


def test_failed_rows_are_retried_and_the_latest_row_counts(tmp_path):
    path = tmp_path / "ab.jsonl"
    rows = [{"id": "c1", "arm": "off", "reply": "", "rc": 1, "secs": 3},
            {"id": "c1", "arm": "on", "reply": "yo", "rc": 0, "secs": 30}]
    path.write_text("".join(json.dumps(r) + "\n" for r in rows))
    assert ab.settled(str(path)) == {("c1", "on")}
    with open(path, "a") as f:
        f.write(json.dumps({"id": "c1", "arm": "off", "reply": "hey there. you good?", "rc": 0,
                            "secs": 20}) + "\n")
    by = jd.latest(json.loads(l) for l in open(path))
    assert by["c1"]["off"]["reply"] == "hey there. you good?"
    m = jd.metrics([by["c1"]["off"]])
    assert m["empty_or_failed"] == 0 and m["question"] == 1.0 and m["second_beat"] == 1.0


def test_metrics_count_failures_and_verdicts_parse():
    m = jd.metrics([{"reply": "", "rc": 0, "secs": 4}, {"reply": "ok", "rc": 0, "secs": 6}])
    assert m["empty_or_failed"] == 1 and m["secs_median"] == 5
    assert jd.parse_verdict(" b\n") == "B"
    assert jd.parse_verdict("TIE.") == "TIE"
    assert jd.parse_verdict("I prefer neither") is None
