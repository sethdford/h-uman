"""Tests for scripts/blind_ab/casing_probe.py — synthetic trials only, no
chat.db, no model weights.

    python3 -m pytest scripts/blind_ab/test_casing_probe.py -v
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "casing_probe.py")
sys.path.insert(0, HERE)

import casing_probe as cp  # noqa: E402


def _run(args):
    return subprocess.run(
        [sys.executable, SCRIPT] + args,
        capture_output=True, text=True, timeout=30,
    )


def _trial(i, real, ai):
    return {"i": f"item_{i:02d}", "context": f"ctx {i}", "real_seth": real, "ai_response": ai}


def seth_like_trials(n=40):
    """~6% lowercase-start, ~25% terminal punct, on both sides -- a
    Seth-shaped distribution that the gate should PASS."""
    trials = []
    for i in range(n):
        lc = i < round(n * 0.06)
        pt = i < round(n * 0.25)
        real = ("yeah that works" if lc else "Yeah that works") + ("." if pt else "")
        ai = ("yeah sounds good" if (i + 1) < round(n * 0.06) + 1 and i < round(n * 0.06)
              else "Sounds good") + ("." if pt else "")
        trials.append(_trial(i, real, ai))
    return trials


def eighty_six_percent_lowercase_trials(n=40):
    """The production incident shape: adapter ~86% lowercase-start / 0%
    terminal punct, human side close to Seth's measured numbers."""
    trials = []
    for i in range(n):
        human_lc = i < round(n * 0.06)
        human_pt = i < round(n * 0.25)
        real = ("yeah ok" if human_lc else "Yeah ok") + ("." if human_pt else "")
        ai_lc = i < round(n * 0.86)
        ai = "yeah for sure" if ai_lc else "Yeah for sure"  # 0% terminal punct
        trials.append(_trial(i, real, ai))
    return trials


# --------------------------------------------------------------------------
# pure function tests
# --------------------------------------------------------------------------


def test_lowercase_rate_and_punct_rate_basic():
    lc, n = cp.lowercase_rate(["yeah sure", "Yeah sure", "123"])
    assert n == 2  # "123" has no alpha char, excluded
    assert abs(lc - 0.5) < 1e-9
    pt, n2 = cp.punct_rate(["hi.", "hi", "hi!"])
    assert n2 == 3
    assert abs(pt - 2 / 3) < 1e-9


def test_compute_casing_gate_passes_on_seth_like_distribution():
    trials = seth_like_trials(40)
    report = cp.compute_casing_gate(trials)
    assert report["pass"] is True, report["reasons"]
    assert report["reasons"] == []
    assert report["adapter"]["lowercase_start_rate"] <= 0.10 + 1e-9


def test_compute_casing_gate_fails_on_86_percent_lowercase_distribution():
    trials = eighty_six_percent_lowercase_trials(40)
    report = cp.compute_casing_gate(trials)
    assert report["pass"] is False
    assert any("lowercase-start rate" in r for r in report["reasons"])
    assert report["adapter"]["lowercase_start_rate"] > 0.10


def test_compute_casing_gate_discriminates_by_threshold():
    """A distribution that JUST clears adapter_lowercase_max but still gaps
    widely from human must fail on the gap check, not silently pass."""
    trials = []
    for i in range(40):
        # adapter at 8% lowercase (under the 0.10 cap) but human at 40% ->
        # a >0.15 gap should still fail the gate.
        ai_lc = i < 3
        real_lc = i < 16
        trials.append(_trial(i, "yeah" if real_lc else "Yeah", "yeah" if ai_lc else "Yeah"))
    report = cp.compute_casing_gate(trials)
    assert report["adapter"]["lowercase_start_rate"] <= 0.10 + 1e-9
    assert report["pass"] is False
    assert any("gap" in r for r in report["reasons"])


def test_compute_casing_gate_never_raises_and_reports_ns():
    report = cp.compute_casing_gate([])
    assert report["n_trials"] == 0
    assert report["adapter"]["n"] == 0
    assert report["human"]["n"] == 0
    # zero-population rates are defined as 0.0, not NaN/crash
    assert report["adapter"]["lowercase_start_rate"] == 0.0


def test_custom_field_names():
    trials = [{"prompt": "x", "candidate": "yeah sure", "gold": "Yeah sure"}] * 25
    report = cp.compute_casing_gate(trials, adapter_field="candidate", human_field="gold")
    assert report["adapter"]["n"] == 25
    assert report["human"]["n"] == 25


# --------------------------------------------------------------------------
# file-loading / refusal contract
# --------------------------------------------------------------------------


def test_compute_casing_gate_from_file_refuses_on_missing_file():
    try:
        cp.compute_casing_gate_from_file("/nonexistent/trials.json")
        assert False, "expected SystemExit"
    except SystemExit as e:
        assert "REFUSING" in str(e)


def test_compute_casing_gate_from_file_refuses_on_too_few_trials():
    d = tempfile.mkdtemp()
    path = os.path.join(d, "trials.json")
    json.dump({"trials": seth_like_trials(5)}, open(path, "w"))
    try:
        cp.compute_casing_gate_from_file(path, min_trials=20)
        assert False, "expected SystemExit"
    except SystemExit as e:
        assert "REFUSING" in str(e)


def test_compute_casing_gate_from_file_accepts_bare_list():
    d = tempfile.mkdtemp()
    path = os.path.join(d, "trials.json")
    json.dump(seth_like_trials(30), open(path, "w"))
    report = cp.compute_casing_gate_from_file(path, min_trials=20)
    assert report["n_trials"] == 30


# --------------------------------------------------------------------------
# CLI / subprocess
# --------------------------------------------------------------------------


def test_cli_exit_zero_on_pass():
    d = tempfile.mkdtemp()
    path = os.path.join(d, "trials.json")
    json.dump({"trials": seth_like_trials(40)}, open(path, "w"))
    r = _run(["--trials", path])
    assert r.returncode == 0, r.stdout + r.stderr
    assert "PASS" in r.stderr


def test_cli_exit_nonzero_on_fail_with_clear_line():
    d = tempfile.mkdtemp()
    path = os.path.join(d, "trials.json")
    json.dump({"trials": eighty_six_percent_lowercase_trials(40)}, open(path, "w"))
    r = _run(["--trials", path])
    assert r.returncode != 0
    assert "FAIL" in r.stderr
    assert "lowercase-start" in r.stderr


def test_cli_writes_out_report():
    d = tempfile.mkdtemp()
    path = os.path.join(d, "trials.json")
    out = os.path.join(d, "report.json")
    json.dump({"trials": seth_like_trials(40)}, open(path, "w"))
    r = _run(["--trials", path, "--out", out])
    assert r.returncode == 0, r.stdout + r.stderr
    assert os.path.isfile(out)
    report = json.load(open(out))
    assert report["pass"] is True


def test_cli_refuses_on_missing_trials_file():
    r = _run(["--trials", "/nonexistent/trials.json"])
    assert r.returncode != 0
    assert "REFUSING" in r.stderr or "REFUSING" in r.stdout


if __name__ == "__main__":
    import pytest
    raise SystemExit(pytest.main([__file__, "-v"]))
