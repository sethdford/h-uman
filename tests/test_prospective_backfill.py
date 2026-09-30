"""Hermetic tests for scripts/prospective_backfill.py (spec 2026-09-30 §4.1, rollout step 2).

A fake `human` binary in tmp_path plays `human prospective backfill`; every
path (db, backups, manifests) is under tmp_path. Nothing reads ~/.human: HOME
and HU_STATE_DIR point at tmp_path for every test (ruling F3), including the
one end-to-end test that drives the real build/human binary.
"""
import json
import os
import sqlite3
import stat
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))

import prospective_backfill as pb  # noqa: E402

NOW = 1_790_000_000

FAKE = """#!/usr/bin/env python3
import json, os, sys
args = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as f:
    f.write(" ".join(args) + "\\n")
if os.environ.get("FAKE_FAIL"):
    sys.exit(3)
if "--write" in args and not os.listdir(os.environ["FAKE_BACKUP_DIR"]):
    sys.exit(4)  # a write before the backup exists is exactly what must never happen
print(json.dumps({"commitments_seen": 3, "followups_seen": 1, "imported_pending": 2,
                  "imported_expired": 1, "reanchored": 1, "skipped_existing": 1,
                  "written": "--write" in args, "skipped_unsafe": 0,
                  "ledger_retired": 1, "ledger_unretired": 0}))
"""


@pytest.fixture(autouse=True)
def private_home(tmp_path, monkeypatch):
    home = tmp_path / "home"
    home.mkdir()
    monkeypatch.setenv("HOME", str(home))
    monkeypatch.setenv("HU_STATE_DIR", str(home / ".human"))
    return home


def make_db(tmp_path, migrated=True):
    p = tmp_path / "memory.db"
    cols = ("id INTEGER PRIMARY KEY, trigger_type TEXT, trigger_value TEXT, action TEXT, "
            "contact_id TEXT, expires_at INTEGER, fired INTEGER DEFAULT 0, created_at INTEGER")
    if migrated:
        cols += (", cue_kind TEXT DEFAULT 'keyword', due_at INTEGER, status TEXT DEFAULT "
                 "'pending', surfaced_at INTEGER, attempts INTEGER DEFAULT 0, outcome TEXT, "
                 "source TEXT DEFAULT 'extractor'")
    con = sqlite3.connect(p)
    con.execute(f"CREATE TABLE prospective_memories({cols})")
    con.commit()
    con.close()
    return str(p)


def setup(tmp_path, monkeypatch, migrated=True):
    b = tmp_path / "human"
    b.write_text(FAKE)
    b.chmod(0o755)
    backups = tmp_path / "backups"
    monkeypatch.setenv("FAKE_LOG", str(tmp_path / "calls.log"))
    monkeypatch.setenv("FAKE_BACKUP_DIR", str(backups))
    logs = tmp_path / "logs"
    argv = ["--db", make_db(tmp_path, migrated), "--human-bin", str(b), "--now", str(NOW),
            "--backup-dir", str(backups), "--manifest-dir", str(logs)]
    return argv, backups, logs


def no_manifest(logs):
    # The manifest dir is created (empty, 0700) before the backup/backfill.
    return not logs.exists() or os.listdir(logs) == []


def calls(tmp_path):
    p = tmp_path / "calls.log"
    return p.read_text().splitlines() if p.exists() else []


def test_dry_run_counts_without_a_backup(tmp_path, monkeypatch):
    argv, backups, logs = setup(tmp_path, monkeypatch)
    assert pb.main(argv) == 0
    assert calls(tmp_path) and "--write" not in calls(tmp_path)[0]
    assert not backups.exists() or not os.listdir(backups)
    [manifest] = list(logs.iterdir())
    assert stat.S_IMODE(manifest.stat().st_mode) == 0o600
    body = json.loads(manifest.read_text())
    assert body["mode"] == "dry_run" and body["backup_written"] is False
    assert body["counts"]["imported_pending"] == 2 and body["counts"]["written"] is False


def test_write_backs_up_before_the_backfill(tmp_path, monkeypatch):
    argv, backups, logs = setup(tmp_path, monkeypatch)
    assert pb.main(argv + ["--write"]) == 0
    [bak] = list(backups.iterdir())
    assert stat.S_IMODE(bak.stat().st_mode) == 0o600
    assert stat.S_IMODE(backups.stat().st_mode) == 0o700
    con = sqlite3.connect(bak)
    assert con.execute("SELECT COUNT(*) FROM sqlite_master WHERE "
                       "name='prospective_memories'").fetchone()[0] == 1
    con.close()
    assert "--write" in calls(tmp_path)[0]
    body = json.loads(next(logs.iterdir()).read_text())
    assert body["mode"] == "write" and body["backup_written"] is True


def test_refuses_an_unmigrated_database_and_writes_nothing(tmp_path, monkeypatch):
    argv, backups, logs = setup(tmp_path, monkeypatch, migrated=False)
    assert pb.main(argv + ["--write"]) == 2
    assert calls(tmp_path) == []
    assert not logs.exists() and not backups.exists()


def test_refuses_a_missing_database_and_writes_nothing(tmp_path, monkeypatch):
    argv, backups, logs = setup(tmp_path, monkeypatch)
    argv[argv.index("--db") + 1] = str(tmp_path / "nope.db")
    assert pb.main(argv + ["--write"]) == 2
    assert calls(tmp_path) == []
    assert not logs.exists() and not backups.exists()
    assert not (tmp_path / "nope.db").exists()


def test_refuses_when_the_backup_cannot_be_made(tmp_path, monkeypatch):
    argv, backups, logs = setup(tmp_path, monkeypatch)
    real = tmp_path / "elsewhere"
    real.mkdir()
    backups.symlink_to(real)  # a planted symlink is never followed
    assert pb.main(argv + ["--write"]) == 2
    assert calls(tmp_path) == []  # the backfill never ran
    assert no_manifest(logs) and os.listdir(real) == []


def test_refuses_when_the_backfill_fails(tmp_path, monkeypatch):
    argv, _, logs = setup(tmp_path, monkeypatch)
    monkeypatch.setenv("FAKE_FAIL", "1")
    assert pb.main(argv) == 2
    assert no_manifest(logs)


def test_refuses_without_a_binary(tmp_path, monkeypatch):
    argv, _, logs = setup(tmp_path, monkeypatch)
    argv[argv.index("--human-bin") + 1] = str(tmp_path / "missing")
    assert pb.main(argv) == 2
    assert not logs.exists()


def test_refuses_an_unwritable_manifest_dir_before_touching_anything(tmp_path, monkeypatch):
    """Fix round 1, M1: the manifest dir is checked BEFORE the backup and the
    backfill, so an unwritable one is a clean refusal (exit 2), not a
    traceback after the database was already written."""
    argv, backups, logs = setup(tmp_path, monkeypatch)
    logs.mkdir(mode=0o500)
    try:
        assert pb.main(argv + ["--write"]) == 2
    finally:
        logs.chmod(0o700)
    assert calls(tmp_path) == []  # the backfill never ran
    assert not backups.exists()  # nor did the backup
    assert os.listdir(logs) == []


def test_refuses_a_symlinked_manifest_dir(tmp_path, monkeypatch):
    argv, backups, logs = setup(tmp_path, monkeypatch)
    real = tmp_path / "elsewhere"
    real.mkdir()
    logs.symlink_to(real)
    assert pb.main(argv + ["--write"]) == 2
    assert calls(tmp_path) == [] and not backups.exists() and os.listdir(real) == []


def test_creates_a_missing_manifest_dir_private(tmp_path, monkeypatch):
    argv, _, logs = setup(tmp_path, monkeypatch)
    assert pb.main(argv) == 0
    assert stat.S_IMODE(logs.stat().st_mode) == 0o700


def test_manifest_failure_after_a_write_prints_counts_and_exits_3(tmp_path, monkeypatch,
                                                                  capsys):
    """Fix round 1, M1 second line of defence: the database WAS written, so
    the counts must not be lost -- they go to stdout and the exit code says
    'done, but no manifest' (3), never a traceback."""
    argv, backups, logs = setup(tmp_path, monkeypatch)

    def boom(path, lines):
        raise PermissionError(13, "denied", path)

    monkeypatch.setattr(pb.cn, "write_jsonl_private", boom)
    assert pb.main(argv + ["--write"]) == pb.EXIT_NO_MANIFEST == 3
    out, err = capsys.readouterr()
    counts = json.loads(out.strip().splitlines()[0])
    assert counts["written"] is True and counts["imported_pending"] == 2
    assert "manifest" in err and str(next(backups.iterdir())) in err


def test_refusal_after_a_write_attempt_names_the_backup_state_unknown(tmp_path, monkeypatch,
                                                                       capsys):
    """Fix round 2, M4: when the backfill subprocess was invoked WITH --write
    (its backup already taken) and then failed, the refusal must NOT claim
    the database is "untouched" -- a contract-mismatch error, a timeout, or a
    kill between the subprocess's COMMIT and this wrapper noticing cannot be
    told apart from a partial write. It must say the state is unknown and
    name the backup to restore from. "untouched" wording is reserved for
    refusals that happen before the backfill subprocess ever runs (or a dry
    run, which never commits)."""
    argv, backups, logs = setup(tmp_path, monkeypatch)
    monkeypatch.setenv("FAKE_FAIL", "1")
    assert pb.main(argv + ["--write"]) == 2
    [bak] = list(backups.iterdir())
    err = capsys.readouterr().err
    assert "UNKNOWN" in err and str(bak) in err
    assert "database untouched" not in err
    assert "nothing written" not in err
    assert no_manifest(logs)


def test_refusal_before_the_backfill_runs_still_says_untouched(tmp_path, monkeypatch, capsys):
    """The dual of the test above: a refusal that happens BEFORE the backfill
    subprocess is ever invoked (a backup failure, here) still gets the plain
    "untouched" wording -- the database genuinely was never touched, and
    write_attempted must be False for this call site."""
    argv, backups, logs = setup(tmp_path, monkeypatch)
    real = tmp_path / "elsewhere"
    real.mkdir()
    backups.symlink_to(real)  # backup() raises before run_backfill is ever called
    assert pb.main(argv + ["--write"]) == 2
    err = capsys.readouterr().err
    assert "nothing written" in err
    assert "UNKNOWN" not in err and "database untouched" not in err
    assert calls(tmp_path) == []  # the backfill subprocess never ran
    assert no_manifest(logs)


def test_manifest_carries_counts_only(tmp_path, monkeypatch):
    argv, _, logs = setup(tmp_path, monkeypatch)
    assert pb.main(argv) == 0
    body = json.loads(next(logs.iterdir()).read_text())
    assert set(body) == {"schema_version", "measured_at", "mode", "backup_written", "counts"}
    assert set(body["counts"]) == set(pb.KEYS)


HUMAN = ROOT / "build" / "human"


@pytest.mark.skipif(not HUMAN.exists(), reason="build/human not built")
def test_end_to_end_with_the_real_binary(tmp_path, private_home):
    """The real `human prospective backfill` on a temp DB: dry run changes
    nothing, --write imports once, a second --write imports nothing."""
    db = tmp_path / "memory.db"
    env = dict(os.environ)
    subprocess.run([str(HUMAN), "prospective", "init", "--db", str(db)], check=True, env=env,
                   capture_output=True)
    con = sqlite3.connect(db)
    con.execute("INSERT INTO commitments(contact_id,description,who,deadline,status,created_at) "
                "VALUES('+15550000002','text you when I land','them',?,'pending',1),"
                "('+15550000002','to ','them',?,'pending',1)", (NOW - 86400, NOW + 86400))
    con.commit()
    con.close()
    argv = ["--db", str(db), "--human-bin", str(HUMAN), "--now", str(NOW),
            "--backup-dir", str(tmp_path / "backups"), "--manifest-dir", str(tmp_path / "logs")]

    def time_rows():
        c = sqlite3.connect(db)
        try:
            return c.execute("SELECT trigger_value, action, due_at FROM prospective_memories "
                             "WHERE cue_kind='time'").fetchall()
        finally:
            c.close()

    assert pb.main(argv) == 0
    assert time_rows() == []
    assert pb.main(argv + ["--write"]) == 0
    assert time_rows() == [("commitment:1", "ask if they still need to text you when they land",
                            NOW)]
    manifests = sorted((tmp_path / "logs").iterdir())
    counts = json.loads(manifests[-1].read_text())["counts"]
    assert counts["imported_pending"] == 1 and counts["reanchored"] == 1
    assert counts["skipped_unsafe"] == 1 and counts["written"] is True
    # Same second -> the stamp repeats; a later --now keeps the backup name unique.
    argv[argv.index("--now") + 1] = str(NOW + 60)
    assert pb.main(argv + ["--write"]) == 0
    assert len(time_rows()) == 1
    assert not (private_home / ".human").exists()
