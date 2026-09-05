"""Tests for scripts/blind_ab/authorship_promotion_gate.py — synthetic JSON
fixtures only, no chat.db, no model weights, no LUAR/torch import.

    python3 -m pytest scripts/blind_ab/test_authorship_promotion_gate.py -v

Per .claude/rules/reports-success-does-nothing.md's "prove a guard
discriminates": the BLOCK/PASS/boundary tests below are deliberately paired
so that a predicate hardcoded to always return one verdict fails at least
one test in every group.
"""
import json
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import authorship_promotion_gate as apg  # noqa: E402


# --------------------------------------------------------------------------
# decide_promotion — the pure predicate
# --------------------------------------------------------------------------


def test_block_known_regression():
    """AC-2.4's exact fixture: the 2026-09 v6 regression shape (prior twin
    0.70, new twin 0.625, floor 0.62). Asserts verdict == BLOCK AND
    reason == "regression_vs_prior" — not just "not PASS", which would also
    pass if the code always returned BLOCK."""
    v = apg.decide_promotion(candidate_twin=0.625, serving_twin=0.70, floor=0.62)
    assert v["verdict"] == "BLOCK"
    assert v["reason"] == "regression_vs_prior"
    assert v["delta"] == pytest.approx(-0.075)


def test_pass_genuine_improvement():
    """Proves the predicate can say yes — a predicate with no PASS-path test
    is exactly as unverified as one with no BLOCK-path test."""
    v = apg.decide_promotion(candidate_twin=0.71, serving_twin=0.625, floor=0.62)
    assert v["verdict"] == "PASS"
    assert v["reason"] == "twin_improved_over_prior_above_floor"


def test_block_below_floor_even_if_improved():
    """Candidate improved over serving but is still below the measured
    floor — the case a naive `candidate > serving` gate would wrongly PASS.
    Tests AC-2.2's second OR-clause independently of the first."""
    v = apg.decide_promotion(candidate_twin=0.60, serving_twin=0.55, floor=0.62)
    assert v["verdict"] == "BLOCK"
    assert v["reason"] == "below_floor"


def test_boundary_equal_is_block():
    """AC-2.2 says `<=`, not `<`; an off-by-one here is invisible in every
    other test."""
    v = apg.decide_promotion(candidate_twin=0.65, serving_twin=0.65, floor=0.62)
    assert v["verdict"] == "BLOCK"
    assert v["reason"] == "regression_vs_prior"


def test_min_gain_requires_a_real_step():
    """min_gain is a keyword, not hardcoded — a candidate that beats serving
    by less than min_gain still BLOCKs."""
    v = apg.decide_promotion(candidate_twin=0.66, serving_twin=0.65, floor=0.62, min_gain=0.05)
    assert v["verdict"] == "BLOCK"
    assert v["reason"] == "regression_vs_prior"
    v2 = apg.decide_promotion(candidate_twin=0.71, serving_twin=0.65, floor=0.62, min_gain=0.05)
    assert v2["verdict"] == "PASS"


def test_decide_promotion_never_returns_inconclusive():
    """INCONCLUSIVE is a property of the loaders raising before this
    function is ever called — decide_promotion itself has exactly two
    possible verdicts."""
    for cand, serv, floor in [(0.9, 0.1, 0.05), (0.0, 0.0, 0.0), (-1.0, -2.0, -3.0)]:
        v = apg.decide_promotion(cand, serv, floor)
        assert v["verdict"] in ("PASS", "BLOCK")


# --------------------------------------------------------------------------
# load_gate_inputs_from_score_json — the primary loader (AC-2.1)
# --------------------------------------------------------------------------


def _score_json(candidate_twin=0.625, serving_twin=0.70, floor=0.62, **overrides):
    payload = {
        "date": "2026-09-05",
        "candidate_adapter": "/adapters/candidate",
        "serving_adapter": "/adapters/serving",
        "candidate": {
            "ceiling_seth_vs_seth": {"mean": 0.701, "ci95": [0.6, 0.8], "n": 200},
            "twin_seth_vs_adapter": {"mean": candidate_twin, "ci95": [0.5, 0.7], "n": 200},
            "floor_seth_vs_other_humans": {"mean": floor, "ci95": [0.5, 0.7], "n": 200},
            "gap_closed_fraction": 0.5,
        },
        "serving": {
            "twin_seth_vs_adapter": {"mean": serving_twin, "ci95": [0.5, 0.8], "n": 200},
            "gap_closed_fraction": 0.9,
        },
        "comparison": {
            "twin_candidate": candidate_twin,
            "twin_serving": serving_twin,
            "delta_candidate_minus_serving": round(candidate_twin - serving_twin, 4),
            "candidate_closer_to_seth": candidate_twin > serving_twin,
        },
    }
    payload.update(overrides)
    return payload


def test_load_gate_inputs_from_score_json_reads_the_right_fields(tmp_path):
    p = tmp_path / "candidate-authorship-2026-09-05.json"
    p.write_text(json.dumps(_score_json(candidate_twin=0.625, serving_twin=0.70, floor=0.62)))
    inputs = apg.load_gate_inputs_from_score_json(str(p))
    assert inputs["candidate_twin"] == pytest.approx(0.625)
    assert inputs["serving_twin"] == pytest.approx(0.70)
    assert inputs["floor"] == pytest.approx(0.62)
    assert inputs["candidate_adapter"] == "/adapters/candidate"
    # never hardcodes 0.625/0.70/0.62 -- these came from the file, not a constant
    assert inputs["candidate_gap"]["twin_seth_vs_adapter"]["mean"] == pytest.approx(0.625)


def test_inconclusive_missing_file():
    with pytest.raises(SystemExit, match="INCONCLUSIVE"):
        apg.load_gate_inputs_from_score_json("/nonexistent/path/candidate-authorship-x.json")


def test_inconclusive_malformed_json(tmp_path):
    """Missing twin_serving and no `candidate` key at all -- a shape
    authorship_gap.py's own refusal-before-write contract guarantees can
    never actually reach disk, but the loader must not KeyError on a
    hand-edited or truncated file."""
    p = tmp_path / "malformed.json"
    p.write_text(json.dumps({"comparison": {"twin_candidate": 0.7}}))
    with pytest.raises(SystemExit, match="INCONCLUSIVE"):
        apg.load_gate_inputs_from_score_json(str(p))


def test_inconclusive_non_finite_twin(tmp_path):
    """twin_candidate is present but not a finite number (null, or a string
    a future producer bug could write) -- proves the loader checks TYPE,
    not just presence."""
    for bad in (None, "NaN", "not-a-number", float("nan")):
        p = tmp_path / "bad.json"
        payload = _score_json()
        payload["comparison"]["twin_candidate"] = bad
        p.write_text(json.dumps(payload, default=lambda x: None))
        with pytest.raises(SystemExit, match="INCONCLUSIVE"):
            apg.load_gate_inputs_from_score_json(str(p))


def test_inconclusive_missing_floor_block(tmp_path):
    p = tmp_path / "no-floor.json"
    payload = _score_json()
    del payload["candidate"]["floor_seth_vs_other_humans"]
    p.write_text(json.dumps(payload))
    with pytest.raises(SystemExit, match="INCONCLUSIVE"):
        apg.load_gate_inputs_from_score_json(str(p))


def test_inconclusive_unparseable_file(tmp_path):
    p = tmp_path / "not-json.json"
    p.write_text("{not valid json")
    with pytest.raises(SystemExit, match="INCONCLUSIVE"):
        apg.load_gate_inputs_from_score_json(str(p))


# --------------------------------------------------------------------------
# load_gate_inputs_from_gap_jsons — the secondary loader
# --------------------------------------------------------------------------


def _gap_json(twin, floor=0.62, ceiling=0.701):
    return {
        "date": "2026-09-05",
        "ceiling_seth_vs_seth": {"mean": ceiling, "ci95": [0.6, 0.8], "n": 200},
        "twin_seth_vs_adapter": {"mean": twin, "ci95": [0.5, 0.7], "n": 200},
        "floor_seth_vs_other_humans": {"mean": floor, "ci95": [0.5, 0.7], "n": 200},
        "gap_closed_fraction": 0.5,
    }


def test_load_gate_inputs_from_gap_jsons(tmp_path):
    cand = tmp_path / "candidate-gap.json"
    serv = tmp_path / "serving-gap.json"
    cand.write_text(json.dumps(_gap_json(0.625, floor=0.62)))
    serv.write_text(json.dumps(_gap_json(0.70)))
    inputs = apg.load_gate_inputs_from_gap_jsons(str(cand), str(serv))
    assert inputs["candidate_twin"] == pytest.approx(0.625)
    assert inputs["serving_twin"] == pytest.approx(0.70)
    assert inputs["floor"] == pytest.approx(0.62)


def test_load_gate_inputs_from_gap_jsons_missing_serving(tmp_path):
    cand = tmp_path / "candidate-gap.json"
    cand.write_text(json.dumps(_gap_json(0.625)))
    with pytest.raises(SystemExit, match="INCONCLUSIVE"):
        apg.load_gate_inputs_from_gap_jsons(str(cand), str(tmp_path / "nope.json"))


def test_load_gate_inputs_from_gap_jsons_malformed_candidate(tmp_path):
    cand = tmp_path / "candidate-gap.json"
    serv = tmp_path / "serving-gap.json"
    cand.write_text(json.dumps({"date": "2026-09-05"}))  # missing twin/floor entirely
    serv.write_text(json.dumps(_gap_json(0.70)))
    with pytest.raises(SystemExit, match="INCONCLUSIVE"):
        apg.load_gate_inputs_from_gap_jsons(str(cand), str(serv))


# --------------------------------------------------------------------------
# _find_latest_score_json — never a bare "newest file"
# --------------------------------------------------------------------------


def test_find_latest_score_json_matches_by_candidate_adapter(tmp_path):
    (tmp_path / "candidate-authorship-2026-09-01.json").write_text(
        json.dumps({"candidate_adapter": "/adapters/other-adapter"}))
    wanted = tmp_path / "candidate-authorship-2026-09-05.json"
    wanted.write_text(json.dumps({"candidate_adapter": "/adapters/my-adapter"}))
    found = apg._find_latest_score_json(
        "/adapters/my-adapter", pattern=str(tmp_path / "candidate-authorship-*.json"))
    assert found == str(wanted)


def test_find_latest_score_json_returns_none_when_no_match(tmp_path):
    (tmp_path / "candidate-authorship-2026-09-01.json").write_text(
        json.dumps({"candidate_adapter": "/adapters/other-adapter"}))
    found = apg._find_latest_score_json(
        "/adapters/my-adapter", pattern=str(tmp_path / "candidate-authorship-*.json"))
    assert found is None


def test_find_latest_score_json_picks_newest_of_multiple_matches(tmp_path):
    import time as _time
    older = tmp_path / "candidate-authorship-2026-09-01.json"
    older.write_text(json.dumps({"candidate_adapter": "/adapters/my-adapter", "which": "older"}))
    _time.sleep(0.01)
    newer = tmp_path / "candidate-authorship-2026-09-05.json"
    newer.write_text(json.dumps({"candidate_adapter": "/adapters/my-adapter", "which": "newer"}))
    found = apg._find_latest_score_json(
        "/adapters/my-adapter", pattern=str(tmp_path / "candidate-authorship-*.json"))
    assert found == str(newer)


def test_find_latest_score_json_ignores_unparseable_files(tmp_path):
    (tmp_path / "candidate-authorship-broken.json").write_text("{not valid json")
    wanted = tmp_path / "candidate-authorship-2026-09-05.json"
    wanted.write_text(json.dumps({"candidate_adapter": "/adapters/my-adapter"}))
    found = apg._find_latest_score_json(
        "/adapters/my-adapter", pattern=str(tmp_path / "candidate-authorship-*.json"))
    assert found == str(wanted)


# --------------------------------------------------------------------------
# thin CLI: exit 0=PASS / 1=BLOCK / 2=INCONCLUSIVE
# --------------------------------------------------------------------------


def test_cli_exit_0_on_pass(tmp_path, capsys):
    p = tmp_path / "score.json"
    p.write_text(json.dumps(_score_json(candidate_twin=0.71, serving_twin=0.625, floor=0.62)))
    rc = apg.main(["--score-json", str(p)])
    assert rc == 0
    out = json.loads(capsys.readouterr().out)
    assert out["verdict"] == "PASS"


def test_cli_exit_1_on_block(tmp_path, capsys):
    p = tmp_path / "score.json"
    p.write_text(json.dumps(_score_json(candidate_twin=0.625, serving_twin=0.70, floor=0.62)))
    rc = apg.main(["--score-json", str(p)])
    assert rc == 1
    out = json.loads(capsys.readouterr().out)
    assert out["verdict"] == "BLOCK"
    assert out["reason"] == "regression_vs_prior"


def test_cli_exit_2_on_inconclusive(capsys):
    rc = apg.main(["--score-json", "/nonexistent/score.json"])
    assert rc == 2


def test_cli_requires_an_input_mode(capsys):
    rc = apg.main([])
    assert rc == 2
    assert "FATAL" in capsys.readouterr().err


def test_cli_secondary_path(tmp_path, capsys):
    cand = tmp_path / "candidate-gap.json"
    serv = tmp_path / "serving-gap.json"
    cand.write_text(json.dumps(_gap_json(0.625, floor=0.62)))
    serv.write_text(json.dumps(_gap_json(0.70)))
    rc = apg.main(["--candidate-json", str(cand), "--serving-json", str(serv)])
    assert rc == 1
    out = json.loads(capsys.readouterr().out)
    assert out["verdict"] == "BLOCK"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
