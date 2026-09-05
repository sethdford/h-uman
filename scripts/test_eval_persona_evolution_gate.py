#!/usr/bin/env python3
"""Hermetic tests for scripts/eval_persona_evolution_gate.py — synthetic
generations and a synthetic results.json only; nothing under ~ is read."""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import eval_persona_evolution_gate as g  # noqa: E402


def _ctx(i):
    return [{"from": "them", "text": "prompt %d" % i}]


def _triples(path, replies):
    path.write_text(json.dumps([{"id": i, "context": _ctx(i), "huuman_reply": r, "seth_reply": "x"}
                                for i, r in enumerate(replies)]))
    return str(path)


def _trials(path, replies, offset=0):
    path.write_text(json.dumps({"trials": [{"i": i, "context": _ctx(i + offset), "ai_response": r, "real_seth": "x"}
                                           for i, r in enumerate(replies)]}))
    return str(path)


def _results(path, axes):
    """axes: {label: (delta, moved)} -> a results.json with one OK event."""
    ev = {"status": "OK", "axes": {lab: {"delta": d, "moved_beyond_ci": m,
                                         "pre": {"mean": 0, "ci_lo": 0, "ci_hi": 0, "n": 1},
                                         "post": {"mean": 0, "ci_lo": 0, "ci_hi": 0, "n": 1}}
                                   for lab, (d, m) in axes.items()}}
    path.write_text(json.dumps({"events": {"job": ev}}))
    return str(path)


def test_load_generations_triples_shape(tmp_path):
    p = _triples(tmp_path / "t.json", ["a", "b"])
    gens = g.load_generations(p)
    assert len(gens) == 2 and set(gens.values()) == {"a", "b"}


def test_load_generations_trials_shape(tmp_path):
    p = _trials(tmp_path / "c.json", ["a", "b", "c"])
    gens = g.load_generations(p)
    assert len(gens) == 3 and set(gens.values()) == {"a", "b", "c"}


def test_load_generations_rejects_unknown_shape(tmp_path):
    p = tmp_path / "x.json"
    p.write_text(json.dumps([{"prompt": "a", "chosen": "b"}]))
    try:
        g.load_generations(str(p))
    except ValueError as e:
        assert "shape" in str(e)
    else:
        raise AssertionError("expected ValueError")


def test_match_generations_pairs_by_context_only(tmp_path):
    pre = g.load_generations(_triples(tmp_path / "pre.json", ["p0", "p1", "p2"]))
    post = g.load_generations(_trials(tmp_path / "post.json", ["q1", "q2", "q9"], offset=1))
    pairs = g.match_generations(pre, post)
    assert sorted(pairs) == [("p1", "q1"), ("p2", "q2")]


def test_gate_pass_when_every_moved_axis_sign_matches(tmp_path):
    res = _results(tmp_path / "r.json", {
        "length_chars": (-3.0, True), "emoji_rate": (+0.03, True), "question_rate": (+0.5, False)})
    pre = ["a much longer reply here", "another long one here"] * 20
    post = ["short 🙂", "tiny 🙂"] * 20
    out = g.run_gate(res, "job", list(zip(pre, post)), min_matched=30, exclude=())
    assert out["status"] == "PASS"
    assert out["axes"]["length_chars"]["sign_match"] is True
    assert out["axes"]["emoji_rate"]["sign_match"] is True
    assert "question_rate" not in out["axes"], "axes that did not move are not gated"


def test_gate_fail_when_one_moved_axis_sign_mismatches(tmp_path):
    res = _results(tmp_path / "r.json", {"length_chars": (-3.0, True), "emoji_rate": (+0.03, True)})
    pre = ["a much longer reply here 🙂"] * 40
    post = ["short"] * 40  # length shrinks (match), emoji vanishes (mismatch)
    out = g.run_gate(res, "job", list(zip(pre, post)), min_matched=30, exclude=())
    assert out["status"] == "FAIL"
    assert out["axes"]["emoji_rate"]["sign_match"] is False


def test_gate_refuses_below_min_matched(tmp_path):
    res = _results(tmp_path / "r.json", {"length_chars": (-3.0, True)})
    out = g.run_gate(res, "job", [("long reply", "s")] * 10, min_matched=30, exclude=())
    assert out["status"] == "INSUFFICIENT_DATA" and "axes" not in out


def test_gate_refuses_when_event_not_ok(tmp_path):
    p = tmp_path / "r.json"
    p.write_text(json.dumps({"events": {"move": {"status": "INSUFFICIENT_DATA", "reason": "n"}}}))
    out = g.run_gate(str(p), "move", [("a b", "c d")] * 40, min_matched=30, exclude=())
    assert out["status"] == "INSUFFICIENT_DATA"


def test_gate_excluded_axis_is_reported_not_gated(tmp_path):
    res = _results(tmp_path / "r.json", {"lowercase_start_rate": (-0.09, True), "length_chars": (-3.0, True)})
    pre = ["Long reply text here"] * 40
    post = ["short"] * 40
    out = g.run_gate(res, "job", list(zip(pre, post)), min_matched=30, exclude=("lowercase_start_rate",))
    assert out["status"] == "PASS"
    assert "lowercase_start_rate" not in out["axes"]
    assert out["excluded_axes"] == ["lowercase_start_rate"]


def test_gate_zero_human_delta_never_counts_as_match(tmp_path):
    res = _results(tmp_path / "r.json", {"length_chars": (0.0, True)})
    out = g.run_gate(res, "job", [("aa", "a")] * 40, min_matched=30, exclude=())
    assert out["axes"]["length_chars"]["sign_match"] is False


if __name__ == "__main__":
    import pytest
    sys.exit(pytest.main([__file__, "-v"]))
