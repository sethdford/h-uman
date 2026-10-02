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


# ── tie / can't-tell answers (better question offers A, B, or T) ────────
# Ties are excluded from the rate's denominator, reported separately, and the
# Wilson CI is over the non-tie answers only.

def test_ties_excluded_from_denominator_and_reported():
    key = {"t0": "A", "t1": "A", "t2": "B", "t3": "A"}
    rows = [{"id": "t0", "choice": "A", "confidence": 3, "_rater": "x"},   # model better
            {"id": "t1", "choice": "T", "confidence": 3, "_rater": "x"},   # tie
            {"id": "t2", "choice": "A", "confidence": 3, "_rater": "x"},   # Seth better
            {"id": "t3", "choice": "T", "confidence": 3, "_rater": "x"}]   # tie
    agg = bs.score_better(rows, key)
    assert agg["n"] == 2 and agg["ties"] == 2 and agg["n_answered"] == 4
    p, lo, hi = wilson(1, 2)
    assert abs(agg["detect"] - p) < 1e-12 and abs(agg["ci_lo"] - lo) < 1e-12
    assert abs(agg["ci_hi"] - hi) < 1e-12
    assert agg["tie_rate"] == 0.5


def test_tie_with_unknown_id_is_not_counted():
    agg = bs.score_better([{"id": "ghost", "choice": "T", "confidence": 3}], {"t0": "A"})
    assert agg["ties"] == 0 and agg["n"] == 0


def test_existing_ab_only_sheet_scores_exactly_as_before():
    # Rows already in an in-progress sheet (A/B/blank, no T anywhere) must
    # parse and score unchanged: same n, rate and CI as score.py's
    # score_rows() over the plain remap, and zero ties.
    from score import score_rows
    sheet = _mk_sheet([
        {"id": "t0", "context": "c", "option_A": "a", "option_B": "b",
         "choice": "A", "confidence": "4", "better_choice": "A"},
        {"id": "t1", "context": "c", "option_A": "a", "option_B": "b",
         "choice": "B", "confidence": "2", "better_choice": "B"},
        {"id": "t2", "context": "c", "option_A": "a", "option_B": "b",
         "choice": "A", "confidence": "3", "better_choice": ""},
    ])
    try:
        rows = bs.load_better_rows([sheet])
        assert [r["choice"] for r in rows] == ["A", "B", ""]
        key = {"t0": "A", "t1": "A", "t2": "B"}
        old = score_rows(rows, key)
        new = bs.score_better(rows, key)
        for k in ("n", "detect", "ci_lo", "ci_hi", "weighted_detect"):
            assert new[k] == old[k], k
        assert new["ties"] == 0 and new["n_answered"] == old["n"]
    finally:
        os.unlink(sheet)


def test_main_writes_ties_separately(tmp_path):
    sheet = _mk_sheet([
        {"id": "t0", "context": "c", "option_A": "a", "option_B": "b",
         "choice": "", "confidence": "", "better_choice": "A"},
        {"id": "t1", "context": "c", "option_A": "a", "option_B": "b",
         "choice": "", "confidence": "", "better_choice": "T"},
        {"id": "t2", "context": "c", "option_A": "a", "option_B": "b",
         "choice": "", "confidence": "", "better_choice": "b"},
    ])
    keyf = tmp_path / "better_key.json"
    keyf.write_text(json.dumps({"t0": "A", "t1": "A", "t2": "A"}))
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
    assert data["n"] == 2 and data["ties"] == 1 and data["n_answered"] == 3
    assert data["better_rate"] == round(wilson(1, 2)[0], 4)


def test_main_all_ties_is_no_rate(tmp_path):
    sheet = _mk_sheet([
        {"id": "t0", "context": "c", "option_A": "a", "option_B": "b",
         "choice": "", "confidence": "", "better_choice": "T"},
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
            assert False, "all-tie sheet has no non-tie answers: must not emit a rate"
        except SystemExit as e:
            assert e.code not in (0, None)
    finally:
        _sys.argv = argv
        os.unlink(sheet)
    assert not out.exists()


def test_main_refuses_llm_judged_sheet(tmp_path):
    # synthetic_judge.py stamps judge_api/judge_model on every row. The check
    # must see the RAW rows: load_better_rows drops those columns.
    sheet = _mk_sheet([
        {"id": "t0", "context": "c", "option_A": "a", "option_B": "b",
         "choice": "A", "confidence": "3", "better_choice": "A",
         "judge_api": "openai", "judge_model": "gemma4-26b"},
    ], fields=FIELDS + ["judge_api", "judge_model"])
    keyf = tmp_path / "better_key.json"
    keyf.write_text(json.dumps({"t0": "A"}))
    out = tmp_path / "better.json"
    import sys as _sys
    argv = _sys.argv
    _sys.argv = ["better_score.py", sheet, "--better-key", str(keyf), "--out", str(out)]
    try:
        try:
            bs.main()
            assert False, "an LLM-judged sheet must be refused"
        except SystemExit as e:
            assert e.code == 2
    finally:
        _sys.argv = argv
        os.unlink(sheet)
    assert not out.exists(), "a refused sheet must never write a measurement"
