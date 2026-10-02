"""End-to-end dry run of scripts/cutover/run_cutover.sh: fixture chat.db and
state, one fake loopback server (director, replies, judge), and the stub
`human replay` (scripts/cutover/dryrun_fake_human.py) -- or, with
HU_CUTOVER_TEST_BIN, a real build that has `human replay`.

Needs the replay harness (PR #594, scripts/blind_ab/replay_*.py) and the
memory-probe scorer (PR #593, scripts/datasets/memory_probe_score.py): from
this repo once merged, or from HU_CUTOVER_HARNESS_DIR / HU_CUTOVER_MEMORY_DIR.
Skipped when either is absent. HOME and TMPDIR point into tmp_path, so the
run cannot touch ~/.human, ~/blind_ab_run or the launchd plists.
"""
import os
import stat
import subprocess

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))
KIT = os.path.join(REPO, "scripts", "cutover")
HARNESS = os.environ.get("HU_CUTOVER_HARNESS_DIR") or os.path.join(REPO, "scripts", "blind_ab")
MEMORY = os.environ.get("HU_CUTOVER_MEMORY_DIR") or os.path.join(REPO, "scripts", "datasets")

pytestmark = pytest.mark.skipif(
    not (os.path.isfile(os.path.join(HARNESS, "replay_driver.py"))
         and os.path.isfile(os.path.join(MEMORY, "memory_probe_score.py"))),
    reason="replay harness (#594) or memory-probe scorer (#593) not present")


def run_dry(tmp_path, extra=()):
    home = tmp_path / "home"
    home.mkdir()
    env = dict(os.environ, HOME=str(home), TMPDIR=str(tmp_path),
               HU_CUTOVER_HARNESS_DIR=HARNESS, HU_CUTOVER_MEMORY_DIR=MEMORY)
    env = {k: v for k, v in env.items() if not k.startswith("HU_") or k.startswith("HU_CUTOVER")}
    p = subprocess.run(["bash", os.path.join(KIT, "run_cutover.sh"), "--dry-run", *extra],
                       env=env, capture_output=True, text=True, timeout=900)
    return p, home


def _run_dir(tmp_path):
    fix = [d for d in tmp_path.iterdir() if d.name.startswith("cutover-dryrun.")]
    assert len(fix) == 1
    return fix[0] / "runs" / "cutover-dryrun"


def test_dry_run_with_stub_produces_a_complete_report(tmp_path):
    p, home = run_dry(tmp_path)
    assert p.returncode == 0, p.stdout[-3000:] + p.stderr[-3000:]
    out = p.stdout
    assert "gates in the binary: HU_THREAD_CONTEXT,HU_LENGTH_POLICY,HU_DIRECTOR_V2" in out
    assert "runtime estimate:" in out.split("1/5 export")[0]  # printed before any work
    assert "arm B-no-director_v2: rows 12/12" in out and "INCOMPLETE" not in out
    run = _run_dir(tmp_path)
    report = (run / "report.md").read_text()
    for g in ("HU_THREAD_CONTEXT", "HU_LENGTH_POLICY", "HU_DIRECTOR_V2"):
        assert f"| {g} | **" in report
    assert "Complete measurement (R5): yes" in report
    assert "Memory probes: measured" in report
    assert "THRESHOLDS OVERRIDDEN" in report  # the fixture is tiny; flagged, not hidden
    # every arm was judged
    for arm in ("A", "B", "B-no-thread_context", "B-no-length_policy", "B-no-director_v2"):
        assert (run / "feed" / arm / "judged.csv").is_file()
    # private files, and nothing written under HOME
    for f in (run / "report.md", run / "turns.jsonl", run / "feed" / "A" / "judged.csv"):
        assert stat.S_IMODE(os.stat(f).st_mode) == 0o600, f
    assert list(home.iterdir()) == []


def test_estimate_only_stops_before_any_work(tmp_path):
    p, _ = run_dry(tmp_path, ["--estimate-only"])
    assert p.returncode == 0, p.stderr
    assert "runtime estimate:" in p.stdout and "1/5 export" not in p.stdout
    assert not (_run_dir(tmp_path)).exists()


@pytest.mark.skipif(not os.access(os.environ.get("HU_CUTOVER_TEST_BIN", "/nonexistent"),
                                  os.X_OK), reason="HU_CUTOVER_TEST_BIN not set")
def test_dry_run_with_the_real_replay_binary(tmp_path):
    p, home = run_dry(tmp_path, ["--human", os.environ["HU_CUTOVER_TEST_BIN"]])
    assert p.returncode == 0, p.stdout[-3000:] + p.stderr[-3000:]
    report = (_run_dir(tmp_path) / "report.md").read_text()
    assert "Complete measurement (R5): yes" in report
    assert list(home.iterdir()) == []
