#!/usr/bin/env python3
"""One-time backfill of dated commitments and delayed follow-ups into
prospective_memories as cue_kind='time' rows (spec
docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.1,
rollout step 2).

The mirroring rules (the live writers' text via hu_prospective_mirror_action,
14-day expiry, re-anchoring, dedupe of a commitment and its paired follow-up)
live in C, in hu_prospective_v2_backfill, which runs through
`human prospective backfill`. This wrapper owns the operational safety:

  * refuses (exit 2, writes nothing) when memory.db is missing, the table is
    not migrated (deploy a build with the v2 migration and let the daemon open
    the database once), or the human binary is missing;
  * --write first backs memory.db up with the SQLite online backup API to
    ~/.human/backups/memory.db.bak-prospective-<ts> (0600, dir 0700), and
    refuses if the backup fails;
  * writes a counts-only manifest (0600, via curator_names.write_jsonl_private,
    the scripts' shared private writer) to
    ~/.human/logs/prospective-backfill-<ts>.json.

The default is a dry run: the C side rolls its transaction back, so the counts
are exact and the database is unchanged.
"""
import argparse
import datetime as dt
import json
import os
import sqlite3
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import curator_names as cn  # noqa: E402

HOME = os.path.expanduser("~")
# The CLI's JSON line; skipped_unsafe (contact promises that cannot be
# rephrased safely, controller ruling F4) is appended after "written".
KEYS = ("commitments_seen", "followups_seen", "imported_pending", "imported_expired",
        "reanchored", "skipped_existing", "written", "skipped_unsafe")
REQUIRED_COLUMNS = {"cue_kind", "due_at", "status", "surfaced_at", "attempts", "outcome",
                    "source"}


def refuse(msg):
    print(f"refusing: {msg}; nothing written", file=sys.stderr)
    return 2


def migrated(db_path):
    con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    try:
        cols = {r[1] for r in con.execute("PRAGMA table_info(prospective_memories)")}
    finally:
        con.close()
    return REQUIRED_COLUMNS <= cols


def backup(db_path, backup_dir, stamp):
    """Consistent 0600 copy through the online backup API from a read-only
    connection (WAL content included; the source is never written). Raises
    OSError / sqlite3.Error on any failure, after removing a partial copy."""
    if os.path.islink(backup_dir):
        raise OSError(f"backup dir is a symlink ({backup_dir})")
    os.makedirs(backup_dir, mode=0o700, exist_ok=True)
    os.chmod(backup_dir, 0o700)
    dst = os.path.join(backup_dir, f"memory.db.bak-prospective-{stamp}")
    os.close(os.open(dst, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600))
    try:
        src = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
        try:
            d = sqlite3.connect(dst)
            try:
                src.backup(d)
                ok = d.execute("SELECT COUNT(*) FROM sqlite_master WHERE "
                               "name='prospective_memories'").fetchone()[0] == 1
            finally:
                d.close()
        finally:
            src.close()
        if not ok:
            raise sqlite3.DatabaseError("backup has no prospective_memories table")
        os.chmod(dst, 0o600)
    except BaseException:
        try:
            os.unlink(dst)
        except OSError:
            pass
        raise
    return dst


def run_backfill(human_bin, db_path, write, now, timeout=300):
    cmd = [human_bin, "prospective", "backfill", "--db", db_path, "--now", str(now)]
    if write:
        cmd.append("--write")
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError(f"backfill exited {r.returncode}")
    lines = [ln for ln in r.stdout.splitlines() if ln.strip()]
    if not lines:
        raise RuntimeError("backfill printed nothing")
    counts = json.loads(lines[-1])
    if set(counts) != set(KEYS) or counts["written"] is not write:
        raise RuntimeError("backfill output does not match the contract")
    return counts


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", default=os.path.join(HOME, ".human/memory.db"))
    ap.add_argument("--human-bin", default=os.path.join(HOME, ".local/bin/human-daemon"))
    ap.add_argument("--write", action="store_true", help="apply (default: dry run)")
    ap.add_argument("--now", type=int, default=None, help="epoch seconds (tests)")
    ap.add_argument("--backup-dir", default=os.path.join(HOME, ".human/backups"))
    ap.add_argument("--manifest-dir", default=os.path.join(HOME, ".human/logs"))
    a = ap.parse_args(argv)
    now = a.now or int(time.time())
    stamp = dt.datetime.fromtimestamp(now, dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    if not (os.path.isfile(a.human_bin) and os.access(a.human_bin, os.X_OK)):
        return refuse(f"no executable human binary at {a.human_bin}")
    if not os.path.isfile(a.db):
        return refuse(f"no database at {a.db}")
    try:
        if not migrated(a.db):
            return refuse("prospective_memories is not migrated; deploy a build with the v2 "
                          "migration and let the daemon open the database once")
    except sqlite3.Error as e:
        return refuse(f"cannot read the database ({e.__class__.__name__})")
    backup_path = None
    if a.write:
        try:
            backup_path = backup(a.db, a.backup_dir, stamp)
        except (OSError, sqlite3.Error) as e:
            return refuse(f"backup failed ({e.__class__.__name__})")
    try:
        counts = run_backfill(a.human_bin, a.db, a.write, now)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as e:
        return refuse(f"backfill failed ({e})")
    payload = {"schema_version": 1, "measured_at": stamp,
               "mode": "write" if a.write else "dry_run",
               "backup_written": backup_path is not None, "counts": counts}
    path = cn.write_jsonl_private(
        os.path.join(a.manifest_dir, f"prospective-backfill-{stamp}.json"), [payload])
    print(json.dumps(counts))
    print(f"wrote {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
