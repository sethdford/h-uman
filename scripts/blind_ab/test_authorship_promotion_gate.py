"""Tests for scripts/blind_ab/authorship_promotion_gate.py — synthetic JSON
fixtures only, no chat.db, no model weights, no LUAR/torch import.

    python3 -m pytest scripts/blind_ab/test_authorship_promotion_gate.py -v

Per .claude/rules/reports-success-does-nothing.md's "prove a guard
discriminates": the BLOCK/PASS/HOLD/boundary tests below are deliberately
paired so that a predicate hardcoded to always return one verdict fails at
least one test in every group.

F1 fix (2026-09-05): decide_promotion() is now a noise-aware, three-way
verdict (PASS/BLOCK/HOLD) that consults the candidate's own twin CI
(candidate_twin_ci95), not just the two point means. The original
point-mean-only gate BLOCKed the real 2026-09-02 -> 2026-09-04 cycle
(twin 0.633 -> 0.625, delta -0.008) as a "regression," even though the
measurement's own ~0.1 CI half-width cannot distinguish that delta from
zero -- see module docstring. Several tests below were updated from the
pre-fix file to reflect the new three-way behavior; where a test's fixture
numbers now land on a different verdict than before, the docstring says so
explicitly.
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


def test_block_known_regression_ci_distinguishable():
    """A clear, CI-distinguishable regression (candidate's CI upper bound
    stays below the serving point estimate) still BLOCKs. Same shape as
    the original AC-2.4 fixture (candidate below prior, above floor) but
    with a tight CI so the data can actually rule out 'just noise' --
    the case the pre-fix test called 'known regression' without ever
    checking whether the CI supported that claim."""
    v = apg.decide_promotion(candidate_twin=0.625, serving_twin=0.70, floor=0.55,
                             candidate_twin_ci95=(0.60, 0.65))
    assert v["verdict"] == "BLOCK"
    assert v["reason"] == "regression_ci_distinguishable"
    assert v["delta"] == pytest.approx(-0.075)
    assert v["candidate_twin_ci95"] == [0.60, 0.65]


def test_hold_within_noise_real_09_02_vs_09_04_cycle():
    """The exact real-world case F1 exists to fix: twin 0.633 (serving,
    2026-09-02) -> 0.625 (candidate, 2026-09-04), delta -0.008, with the
    realistic CI documented in designs/US-2.md §4 (0.506-0.725, a span of
    0.22 at n~=36). A point-mean-only gate BLOCKs this every single night
    forever; the noise-aware gate must HOLD instead: the CI overlaps
    serving's point estimate on both sides, so the data cannot distinguish
    'slightly worse' from 'no change,' and the correct answer is to
    accumulate another cycle, not to declare a regression."""
    v = apg.decide_promotion(candidate_twin=0.625, serving_twin=0.633, floor=0.50,
                             candidate_twin_ci95=(0.506, 0.725))
    assert v["verdict"] == "HOLD"
    assert v["reason"] == "within_noise"
    assert v["delta"] == pytest.approx(-0.008)


def test_block_ci_distinguishable_regression_low_candidate():
    """AC-2's follow-up fixture: candidate 0.60 with CI upper bound 0.62
    against serving 0.633. 0.62 < 0.633, so the data CAN tell this apart
    from serving -- BLOCK, not HOLD, even though the raw point delta
    (-0.033) is smaller than the AC-2.4 fixture above."""
    v = apg.decide_promotion(candidate_twin=0.60, serving_twin=0.633, floor=0.50,
                             candidate_twin_ci95=(0.55, 0.62))
    assert v["verdict"] == "BLOCK"
    assert v["reason"] == "regression_ci_distinguishable"


def test_pass_ci_distinguishable_improvement():
    """Candidate 0.70 with CI lower bound 0.64 against serving 0.633: 0.64
    > 0.633, so the data CAN tell this apart from serving -- PASS via CI
    separation, not merely the min_gain fallback."""
    v = apg.decide_promotion(candidate_twin=0.70, serving_twin=0.633, floor=0.50,
                             candidate_twin_ci95=(0.64, 0.76))
    assert v["verdict"] == "PASS"
    assert v["reason"] == "twin_improved_ci_distinguishable"


def test_pass_genuine_improvement_via_min_gain():
    """A real step forward whose CI still overlaps serving (so CI
    separation alone doesn't decide it) but whose point-estimate gain
    clears --min-gain (default 0.05) still PASSes -- the min_gain
    fallback exists precisely so a genuine improvement doesn't get stuck
    at HOLD forever just because n~=36 keeps every CI wide."""
    v = apg.decide_promotion(candidate_twin=0.71, serving_twin=0.625, floor=0.55,
                             candidate_twin_ci95=(0.60, 0.82))
    assert v["verdict"] == "PASS"
    assert v["reason"] == "twin_improved_min_gain"
    assert v["delta"] == pytest.approx(0.085)


def test_block_below_floor_even_if_improved_and_ci_wide():
    """Candidate improved over serving and its CI is wide enough to say
    nothing about serving either way, but the candidate is still below
    the measured floor -- the case a naive `candidate > serving` gate
    would wrongly PASS. Floor comparison stays a plain point check
    regardless of CI width (AC-2.2's second OR-clause, tested
    independently of the CI logic)."""
    v = apg.decide_promotion(candidate_twin=0.60, serving_twin=0.55, floor=0.62,
                             candidate_twin_ci95=(0.50, 0.70))
    assert v["verdict"] == "BLOCK"
    assert v["reason"] == "below_floor"


def test_boundary_equal_twins_is_hold_not_block():
    """Equal point estimates carry zero directional information -- that is
    exactly what HOLD means. (The pre-fix gate asserted BLOCK here via a
    literal `<=`, which is the same category of bug F1 fixes: a tie is not
    evidence of regression, it's an absence of evidence either way.)"""
    v = apg.decide_promotion(candidate_twin=0.65, serving_twin=0.65, floor=0.55,
                             candidate_twin_ci95=(0.55, 0.75))
    assert v["verdict"] == "HOLD"
    assert v["reason"] == "within_noise"
    assert v["delta"] == 0.0


def test_min_gain_requires_a_real_step():
    """min_gain is a keyword, not hardcoded. A candidate that beats serving
    by less than min_gain, with a CI too wide to decide it either way,
    HOLDs (not BLOCK -- see test_hold_within_noise, this is the same
    'can't tell' case, not a positive regression finding). A candidate
    that clears min_gain PASSes."""
    wide_ci = (0.55, 0.85)  # brackets both twins below; doesn't itself decide
    v = apg.decide_promotion(candidate_twin=0.66, serving_twin=0.65, floor=0.55,
                             candidate_twin_ci95=wide_ci, min_gain=0.05)
    assert v["verdict"] == "HOLD"
    assert v["reason"] == "within_noise"
    v2 = apg.decide_promotion(candidate_twin=0.71, serving_twin=0.65, floor=0.55,
                              candidate_twin_ci95=wide_ci, min_gain=0.05)
    assert v2["verdict"] == "PASS"
    assert v2["reason"] == "twin_improved_min_gain"


def test_decide_promotion_never_returns_inconclusive():
    """INCONCLUSIVE is a property of the loaders raising before this
    function is ever called -- decide_promotion itself has exactly three
    possible verdicts."""
    cases = [
        (0.9, 0.1, 0.05, (0.85, 0.95)),
        (0.0, 0.0, 0.0, (-0.05, 0.05)),
        (-1.0, -2.0, -3.0, (-1.05, -0.95)),
    ]
    for cand, serv, floor, ci95 in cases:
        v = apg.decide_promotion(cand, serv, floor, ci95)
        assert v["verdict"] in ("PASS", "BLOCK", "HOLD")


# --------------------------------------------------------------------------
# load_gate_inputs_from_score_json — the primary loader (AC-2.1)
# --------------------------------------------------------------------------


def _score_json(candidate_twin=0.625, serving_twin=0.70, floor=0.62,
                candidate_twin_ci95=None, **overrides):
    if candidate_twin_ci95 is None:
        candidate_twin_ci95 = [round(candidate_twin - 0.1, 3), round(candidate_twin + 0.1, 3)]
    payload = {
        "date": "2026-09-05",
        "candidate_adapter": "/adapters/candidate",
        "serving_adapter": "/adapters/serving",
        "candidate": {
            "ceiling_seth_vs_seth": {"mean": 0.701, "ci95": [0.6, 0.8], "n": 200},
            "twin_seth_vs_adapter": {"mean": candidate_twin, "ci95": candidate_twin_ci95, "n": 200},
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
    p.write_text(json.dumps(_score_json(candidate_twin=0.625, serving_twin=0.70, floor=0.62,
                                        candidate_twin_ci95=[0.5, 0.7])))
    inputs = apg.load_gate_inputs_from_score_json(str(p))
    assert inputs["candidate_twin"] == pytest.approx(0.625)
    assert inputs["serving_twin"] == pytest.approx(0.70)
    assert inputs["floor"] == pytest.approx(0.62)
    assert inputs["candidate_adapter"] == "/adapters/candidate"
    # never hardcodes 0.625/0.70/0.62 -- these came from the file, not a constant
    assert inputs["candidate_gap"]["twin_seth_vs_adapter"]["mean"] == pytest.approx(0.625)
    # candidate_twin_ci95 is read from candidate.twin_seth_vs_adapter.ci95,
    # not fabricated by the loader.
    assert inputs["candidate_twin_ci95"] == (0.5, 0.7)


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


def test_inconclusive_missing_ci95_field(tmp_path):
    """candidate.twin_seth_vs_adapter has a mean but no ci95 at all -- the
    F1 fix's new required input. Must INCONCLUDE, not silently proceed
    with a fabricated CI (that would be exactly the class of bug
    .claude/rules/no-number-without-a-measurement.md forbids)."""
    p = tmp_path / "no-ci.json"
    payload = _score_json()
    del payload["candidate"]["twin_seth_vs_adapter"]["ci95"]
    p.write_text(json.dumps(payload))
    with pytest.raises(SystemExit, match="INCONCLUSIVE"):
        apg.load_gate_inputs_from_score_json(str(p))


def test_inconclusive_malformed_ci95_field(tmp_path):
    """ci95 present but malformed: wrong length, non-numeric, or
    lo > hi (an impossible/inverted interval)."""
    for bad_ci in ([0.5], [0.5, 0.6, 0.7], ["lo", "hi"], [0.7, 0.5]):
        p = tmp_path / "bad-ci.json"
        payload = _score_json()
        payload["candidate"]["twin_seth_vs_adapter"]["ci95"] = bad_ci
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


def _gap_json(twin, floor=0.62, ceiling=0.701, ci95=None):
    if ci95 is None:
        ci95 = [round(twin - 0.1, 3), round(twin + 0.1, 3)]
    return {
        "date": "2026-09-05",
        "ceiling_seth_vs_seth": {"mean": ceiling, "ci95": [0.6, 0.8], "n": 200},
        "twin_seth_vs_adapter": {"mean": twin, "ci95": ci95, "n": 200},
        "floor_seth_vs_other_humans": {"mean": floor, "ci95": [0.5, 0.7], "n": 200},
        "gap_closed_fraction": 0.5,
    }


def test_load_gate_inputs_from_gap_jsons(tmp_path):
    cand = tmp_path / "candidate-gap.json"
    serv = tmp_path / "serving-gap.json"
    cand.write_text(json.dumps(_gap_json(0.625, floor=0.62, ci95=[0.5, 0.7])))
    serv.write_text(json.dumps(_gap_json(0.70)))
    inputs = apg.load_gate_inputs_from_gap_jsons(str(cand), str(serv))
    assert inputs["candidate_twin"] == pytest.approx(0.625)
    assert inputs["serving_twin"] == pytest.approx(0.70)
    assert inputs["floor"] == pytest.approx(0.62)
    assert inputs["candidate_twin_ci95"] == (0.5, 0.7)


def test_load_gate_inputs_from_gap_jsons_missing_serving(tmp_path):
    cand = tmp_path / "candidate-gap.json"
    cand.write_text(json.dumps(_gap_json(0.625)))
    with pytest.raises(SystemExit, match="INCONCLUSIVE"):
        apg.load_gate_inputs_from_gap_jsons(str(cand), str(tmp_path / "nope.json"))


def test_load_gate_inputs_from_gap_jsons_malformed_candidate(tmp_path):
    cand = tmp_path / "candidate-gap.json"
    serv = tmp_path / "serving-gap.json"
    cand.write_text(json.dumps({"date": "2026-09-05"}))  # missing twin/floor/ci95 entirely
    serv.write_text(json.dumps(_gap_json(0.70)))
    with pytest.raises(SystemExit, match="INCONCLUSIVE"):
        apg.load_gate_inputs_from_gap_jsons(str(cand), str(serv))


def test_load_gate_inputs_from_gap_jsons_missing_ci95(tmp_path):
    cand_payload = _gap_json(0.625)
    del cand_payload["twin_seth_vs_adapter"]["ci95"]
    cand = tmp_path / "candidate-gap.json"
    serv = tmp_path / "serving-gap.json"
    cand.write_text(json.dumps(cand_payload))
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
# thin CLI: exit 0=PASS / 1=BLOCK / 2=INCONCLUSIVE / 3=HOLD
# --------------------------------------------------------------------------


def test_cli_exit_0_on_pass(tmp_path, capsys):
    p = tmp_path / "score.json"
    p.write_text(json.dumps(_score_json(candidate_twin=0.71, serving_twin=0.625, floor=0.55,
                                        candidate_twin_ci95=[0.66, 0.76])))
    rc = apg.main(["--score-json", str(p)])
    assert rc == 0
    out = json.loads(capsys.readouterr().out)
    assert out["verdict"] == "PASS"


def test_cli_exit_1_on_block(tmp_path, capsys):
    p = tmp_path / "score.json"
    p.write_text(json.dumps(_score_json(candidate_twin=0.55, serving_twin=0.70, floor=0.50,
                                        candidate_twin_ci95=[0.45, 0.65])))
    rc = apg.main(["--score-json", str(p)])
    assert rc == 1
    out = json.loads(capsys.readouterr().out)
    assert out["verdict"] == "BLOCK"
    assert out["reason"] == "regression_ci_distinguishable"


def test_cli_exit_3_on_hold(tmp_path, capsys):
    """The CLI-level twin of test_hold_within_noise_real_09_02_vs_09_04_cycle
    -- exit code 3 is a distinct, documented signal from BLOCK's 1, so a
    caller script can special-case 'accumulate another cycle' without
    string-matching the reason field."""
    p = tmp_path / "score.json"
    p.write_text(json.dumps(_score_json(candidate_twin=0.625, serving_twin=0.633, floor=0.50,
                                        candidate_twin_ci95=[0.506, 0.725])))
    rc = apg.main(["--score-json", str(p)])
    assert rc == 3
    out = json.loads(capsys.readouterr().out)
    assert out["verdict"] == "HOLD"
    assert out["reason"] == "within_noise"


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
    cand.write_text(json.dumps(_gap_json(0.55, floor=0.50, ci95=[0.45, 0.65])))
    serv.write_text(json.dumps(_gap_json(0.70)))
    rc = apg.main(["--candidate-json", str(cand), "--serving-json", str(serv)])
    assert rc == 1
    out = json.loads(capsys.readouterr().out)
    assert out["verdict"] == "BLOCK"


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
