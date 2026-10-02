"""Tests for better_score.py -- the "is h-uman's reply BETTER" measurement.

Hermetic: no real sheet, no network, no gate-file writes outside a tmpdir.
Mirrors test_score.py / score_preference.py's own test patterns.
"""
import csv
import json
import os
import tempfile

import better_score as bs
from score import wilson


FIELDS = ["id", "context", "option_A", "option_B", "choice", "confidence", "better_choice"]


def _mk_sheet(rows, fields=FIELDS):
    f = tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False, newline="")
    w = csv.DictWriter(f, fieldnames=fields)
    w.writeheader()
    w.writerows(rows)
    f.close()
    return f.name


# ── load_better_key ─────────────────────────────────────────────────────

def test_load_better_key_reads_plain_object():
    f = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
    json.dump({"r1": "A", "r2": "B"}, f)
    f.close()
    try:
        assert bs.load_better_key(f.name) == {"r1": "A", "r2": "B"}
    finally:
        os.unlink(f.name)


def test_load_better_key_refuses_non_object():
    f = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
    json.dump(["not", "a", "dict"], f)
    f.close()
    try:
        try:
            bs.load_better_key(f.name)
            assert False, "must raise on a non-dict key file"
        except ValueError:
            pass
    finally:
        os.unlink(f.name)


# ── load_better_rows: remaps better_choice -> choice ────────────────────

def test_load_better_rows_remaps_the_column():
    path = _mk_sheet([
        {"id": "r1", "context": "c", "option_A": "a", "option_B": "b",
         "choice": "A", "confidence": "3", "better_choice": "B"},
    ])
    try:
        rows = bs.load_better_rows([path])
        assert rows[0]["id"] == "r1"
        assert rows[0]["choice"] == "B"  # from better_choice, not choice
    finally:
        os.unlink(path)


def test_load_better_rows_blank_on_unanswered_row():
    path = _mk_sheet([
        {"id": "r1", "context": "c", "option_A": "a", "option_B": "b",
         "choice": "A", "confidence": "3", "better_choice": ""},
    ])
    try:
        rows = bs.load_better_rows([path])
        assert rows[0]["choice"] == ""
    finally:
        os.unlink(path)


def test_load_better_rows_backward_compat_no_better_choice_column():
    # A sheet built before this feature existed has no better_choice column
    # at all -- DictReader simply omits the key; must degrade to "", not KeyError.
    path = _mk_sheet(
        [{"id": "r1", "context": "c", "option_A": "a", "option_B": "b",
          "choice": "A", "confidence": "3"}],
        fields=["id", "context", "option_A", "option_B", "choice", "confidence"],
    )
    try:
        rows = bs.load_better_rows([path])
        assert rows[0]["choice"] == ""
    finally:
        os.unlink(path)


# ── score_rows reused unmodified: n=0 must never be a measurement ──────

def test_better_scoring_is_the_same_wilson_math():
    key = {f"t{i}": ("A" if i % 2 else "B") for i in range(10)}
    rows = [{"id": f"t{i}", "choice": key[f"t{i}"], "confidence": 3, "_rater": "x"}
            for i in range(7)]
    from score import score_rows
    agg = score_rows(rows, key)
    assert agg["n"] == 7
    expected = wilson(7, 7)
    assert abs(agg["detect"] - expected[0]) < 1e-12


def test_n_zero_never_produces_a_rate():
    from score import score_rows
    assert score_rows([], {"t0": "A"})["n"] == 0
    assert wilson(0, 0) == (0.0, 0.0, 0.0)


# ── CLI: better_rate written only when n > 0; never touches the gate ───

def test_main_writes_better_rate_to_the_given_out_path(tmp_path, monkeypatch=None):
    sheet = _mk_sheet([
        {"id": "t0", "context": "c", "option_A": "a", "option_B": "b",
         "choice": "", "confidence": "", "better_choice": "A"},
        {"id": "t1", "context": "c", "option_A": "a", "option_B": "b",
         "choice": "", "confidence": "", "better_choice": "B"},
    ])
    keyf = tmp_path / "better_key.json"
    keyf.write_text(json.dumps({"t0": "A", "t1": "A"}))
    out = tmp_path / "better.json"
    import sys as _sys
    argv = _sys.argv
    _sys.argv = ["better_score.py", sheet, "--better-key", str(keyf), "--out", str(out)]
    try:
        try:
            bs.main()
        except SystemExit as e:
            assert e.code in (0, None)
    finally:
        _sys.argv = argv
        os.unlink(sheet)
    data = json.loads(out.read_text())
    assert data["n"] == 2
    assert 0.0 <= data["better_rate"] <= 1.0
    assert "arm" not in data


def test_main_refuses_to_write_on_zero_evidence(tmp_path):
    sheet = _mk_sheet([
        {"id": "t0", "context": "c", "option_A": "a", "option_B": "b",
         "choice": "", "confidence": "", "better_choice": ""},  # unanswered
    ])
    keyf = tmp_path / "better_key.json"
    keyf.write_text(json.dumps({"t0": "A"}))
    out = tmp_path / "better.json"
    import sys as _sys
    argv = _sys.argv
    _sys.argv = ["better_score.py", sheet, "--better-key", str(keyf), "--out", str(out)]
    try:
        try:
            bs.main()
            assert False, "must exit non-zero on n=0"
        except SystemExit as e:
            assert e.code not in (0, None)
    finally:
        _sys.argv = argv
        os.unlink(sheet)
    assert not out.exists(), "n=0 must never write a measurement file"


def test_main_never_writes_to_a_gate_path_by_default():
    # The default --out must not be either gate-file name, under any
    # spelling -- this measurement must never be mistaken for a promotion
    # verdict.
    assert "blind_ab_gate" not in bs.DEFAULT_OUT
    assert bs.DEFAULT_OUT.endswith("blind_ab_better.json")


def test_selftest_runs_clean():
    bs.selftest()  # must not raise
