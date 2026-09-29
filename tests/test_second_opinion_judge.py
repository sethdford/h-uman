"""Judge calibration (spec §4.2): synthetic key only, κ needs ≥ 20 shared items."""
import csv
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
from second_opinion import judge, stats  # noqa: E402

COLS = ["id", "context", "option_A", "option_B", "choice", "confidence"]


def sheet(path, choices, judged=False):
    cols = COLS + (["judge_model"] if judged else [])
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for i, ch in enumerate(choices):
            row = {"id": f"x{i}", "context": "c", "option_A": "a", "option_B": "b",
                   "choice": ch, "confidence": "4"}
            if judged:
                row["judge_model"] = "gemma"
            w.writerow(row)


def test_human_choices_ignore_blank_and_judged_sheets(tmp_path):
    sheet(tmp_path / "rating_sheet.csv", ["", "", ""])            # nobody rated yet
    sheet(tmp_path / "rating_sheet_seth.csv", ["A", "", "b"])
    sheet(tmp_path / "judged.csv", ["B", "B", "B"], judged=True)
    assert judge.human_choices(str(tmp_path)) == {"x0": "A", "x2": "B"}


def test_calibration_needs_twenty_shared_items():
    few = judge.calibration({"a": "A"}, {"a": "A"})
    assert few["shared"] == 1 and few["kappa"] == stats.NOT_MEASURED
    assert few["agreement"] == stats.NOT_MEASURED
    h = {f"i{n}": ("A" if n % 2 else "B") for n in range(20)}
    r = judge.calibration(h, dict(h))
    assert r["shared"] == 20 and r["agreement"] == 1.0 and r["kappa"] == 1.0
    assert isinstance(r["agreement"], float)


def test_latest_run_dir_requires_sheet_and_key(tmp_path):
    (tmp_path / "old").mkdir()
    (tmp_path / "new").mkdir()
    for d in ("old", "new"):
        (tmp_path / d / "rating_sheet.csv").write_text("id\n")
    (tmp_path / "old" / "answer_key.json").write_text("{}")
    assert judge.latest_run_dir(str(tmp_path)) == str(tmp_path / "old")
    assert judge.latest_run_dir(str(tmp_path / "missing")) is None


def test_latest_run_dir_skips_preference_mode_and_invalid_json(tmp_path):
    # "detect" is the OLDEST candidate; "badjson" and "pref" are newer but must
    # still be skipped, proving the filter (not mtime luck) picks "detect".
    def make(name, key_text, mtime_offset):
        d = tmp_path / name
        d.mkdir()
        (d / "rating_sheet.csv").write_text("id\n")
        (d / "answer_key.json").write_text(key_text)
        t = os.path.getmtime(d) + mtime_offset
        os.utime(d, (t, t))

    make("detect", json.dumps({"x0": "A"}), 0)
    make("badjson", "{not valid json", 10)
    make("pref", json.dumps({"_mode": "preference", "x0": "A"}), 20)

    assert judge.latest_run_dir(str(tmp_path)) == str(tmp_path / "detect")


def test_human_choices_judged_check_matches_detect_rater_kind(tmp_path):
    # synthetic_judge.py stamps BOTH judge_api and judge_model; a sheet must
    # count as judged (and be excluded from human_choices) if EITHER is set,
    # matching score.py's detect_rater_kind.
    cols = COLS + ["judge_api"]
    with open(tmp_path / "judged_api_only.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for i, ch in enumerate(["A", "B", "A"]):
            w.writerow({"id": f"x{i}", "context": "c", "option_A": "a", "option_B": "b",
                       "choice": ch, "confidence": "4", "judge_api": "openai"})
    assert judge.human_choices(str(tmp_path)) == {}


class G:
    base_url = "http://127.0.0.1:8743"
    model = "mlx-community/gemma-4-31b-it-4bit"
    name = "gemma-4-31b-it-4bit@local"


def test_judge_pass_runs_synthetic_rater_and_calibrates(tmp_path):
    run_dir, out = tmp_path / "run", tmp_path / "out"
    run_dir.mkdir()
    sheet(run_dir / "rating_sheet.csv", [""] * 3)
    (run_dir / "answer_key.json").write_text(json.dumps({"x0": "A", "x1": "B", "x2": "A"}))
    sheet(run_dir / "rating_sheet_seth.csv", ["A", "B", "A"])
    calls = []

    class R:
        returncode = 0

    def run(cmd, **kw):
        calls.append(cmd)
        if cmd[1].endswith("synthetic_judge.py"):
            sheet(cmd[cmd.index("--out") + 1], ["A", "B", "B"], judged=True)
        else:
            Path(cmd[cmd.index("--json-out") + 1]).write_text(json.dumps({"detection": 0.33}))
        return R()

    r = judge.judge_pass(G(), str(run_dir), str(out), run=run)
    assert "--rater" in calls[1] and calls[1][calls[1].index("--rater") + 1] == "synthetic"
    assert calls[0][calls[0].index("--endpoint") + 1] == G.base_url + "/v1/chat/completions"
    assert r["calibration"]["shared"] == 3 and r["calibration"]["kappa"] == stats.NOT_MEASURED
    assert r["calibration"]["agreement"] == stats.NOT_MEASURED


def test_judge_pass_skips_non_local_backends(tmp_path):
    class V:
        name = "gemini-3.8-flash@vertex"

    assert judge.judge_pass(V(), str(tmp_path), str(tmp_path)) == {"skipped": "backend"}


# ---------------------------------------------------------------------------
# Final-review fix round: C1 — score.py exits 1 on any verdict other than PASS
# (after writing results); that night must still produce the calibration.
# ---------------------------------------------------------------------------

def _scoring_run(score_rc, write_results=True):
    class R:
        def __init__(self, rc):
            self.returncode = rc

    def run(cmd, **kw):
        if cmd[1].endswith("synthetic_judge.py"):
            sheet(cmd[cmd.index("--out") + 1], ["A"] * 20 + ["B"] * 5, judged=True)
            return R(0)
        if write_results:
            Path(cmd[cmd.index("--json-out") + 1]).write_text(
                json.dumps({"verdict": "FAIL", "detection": 0.88}))
        return R(score_rc)
    return run


def _run_dir_25(tmp_path):
    run_dir = tmp_path / "run"
    run_dir.mkdir()
    sheet(run_dir / "rating_sheet.csv", [""] * 25)
    (run_dir / "answer_key.json").write_text(json.dumps({f"x{i}": "A" for i in range(25)}))
    sheet(run_dir / "rating_sheet_seth.csv", ["A"] * 20 + ["B"] * 5)
    return run_dir


def test_judge_pass_calibrates_when_score_verdict_is_not_pass(tmp_path):
    run_dir = _run_dir_25(tmp_path)
    r = judge.judge_pass(G(), str(run_dir), str(tmp_path / "out"), run=_scoring_run(1))
    assert r["score_exit"] == 1 and r["results"]["verdict"] == "FAIL"
    assert r["calibration"]["shared"] == 25 and r["calibration"]["agreement"] == 1.0
    assert r["calibration"]["kappa"] == 1.0


def test_judge_pass_treats_score_refusal_as_failure(tmp_path):
    import pytest
    run_dir = _run_dir_25(tmp_path)
    for rc in (2, 3):
        with pytest.raises(RuntimeError):
            judge.judge_pass(G(), str(run_dir), str(tmp_path / f"out{rc}"),
                             run=_scoring_run(rc))


def test_judge_pass_needs_the_results_file_even_on_exit_1(tmp_path):
    import pytest
    run_dir = _run_dir_25(tmp_path)
    with pytest.raises(RuntimeError):
        judge.judge_pass(G(), str(run_dir), str(tmp_path / "out"),
                         run=_scoring_run(1, write_results=False))


def test_judge_pass_stamps_the_run_dir_it_judged(tmp_path):
    import hashlib
    run_dir = _run_dir_25(tmp_path)
    out = tmp_path / "out"
    judge.judge_pass(G(), str(run_dir), str(out), run=_scoring_run(0))
    src = out / "source.json"
    assert (src.stat().st_mode & 0o777) == 0o600
    want = hashlib.sha256((run_dir / "answer_key.json").read_bytes()).hexdigest()[:16]
    assert json.loads(src.read_text()) == {"run_dir": "run", "answer_key_sha256": want}
    assert judge.lane_sheet_matches(str(out / "judged.csv"), str(run_dir))


def test_judge_pass_never_reads_a_previous_runs_results(tmp_path):
    # A same-day re-run reuses out_dir. If score.py exits 1 without writing
    # results this time, the old judge-results.json must not be read as this
    # run's: the pass fails instead.
    import pytest
    run_dir = _run_dir_25(tmp_path)
    out = tmp_path / "out"
    judge.judge_pass(G(), str(run_dir), str(out), run=_scoring_run(1))
    assert (out / "judge-results.json").exists()
    with pytest.raises(RuntimeError):
        judge.judge_pass(G(), str(run_dir), str(out), run=_scoring_run(1, write_results=False))
    assert not (out / "judge-results.json").exists()
