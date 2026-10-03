#!/usr/bin/env python3
"""One-time backfill of dated commitments and delayed follow-ups into
prospective_memories as cue_kind='time' rows (spec
docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.1,
rollout step 2).

The mirroring rules (the live writers' text via hu_prospective_mirror_action,
14-day expiry, re-anchoring, dedupe of a commitment and its paired follow-up)
live in C, in hu_prospective_v2_backfill, which runs through
`human prospective backfill`. This wrapper owns the operational safety:

  * refuses (exit 2, database untouched) when memory.db is missing, the table
    is not migrated (deploy a build with the v2 migration and let the daemon
    open the database once), the human binary is missing, or the manifest dir
    is not a writable, non-symlink directory -- all checked before anything is
    written;
  * --write first backs memory.db up with the SQLite online backup API to
    ~/.human/backups/memory.db.bak-prospective-<ts> (0600, dir 0700), and
    refuses if the backup fails;
  * writes a counts-only manifest (0600, via curator_names.write_jsonl_private,
    the scripts' shared private writer) to
    ~/.human/logs/prospective-backfill-<ts>.json.

The default is a dry run: the C side rolls its transaction back, so the counts
are exact and the database is unchanged.

Exit codes:
  0  done; manifest written
  2  refused. Every refusal BEFORE the backfill subprocess is invoked (missing
     binary/database, unmigrated schema, unusable manifest dir, a failed
     --write backup) leaves the database untouched. A refusal AFTER a
     --write attempt's backfill subprocess was invoked and then failed
     (contract mismatch, timeout, killed between its COMMIT and this
     wrapper noticing) is different: that failure cannot be told apart from
     a partial commit, so the message says the database state is UNKNOWN
     and names the backup to restore from -- it never claims "untouched".
     A dry-run backfill failure is still "nothing written": the C side
     never commits without --write.
  3  the backfill ran (with --write, the database WAS written) but the
     manifest could not be written; the counts JSON line is on stdout
"""
import argparse
import datetime as dt
import json
import os
import sqlite3
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import curator_names as cn  # noqa: E402

HOME = os.path.expanduser("~")
# The CLI's JSON line; skipped_unsafe (contact promises that cannot be
# rephrased safely, controller ruling F4), ledger_retired (ledger rows of
# expired imports retired in the same transaction, known gap 5) and
# ledger_unretired (expired imports whose contact was too long to retire) are
# appended after "written".
KEYS = ("commitments_seen", "followups_seen", "imported_pending", "imported_expired",
        "reanchored", "skipped_existing", "written", "skipped_unsafe", "ledger_retired",
        "ledger_unretired")
EXIT_REFUSED = 2
EXIT_NO_MANIFEST = 3
REQUIRED_COLUMNS = {"cue_kind", "due_at", "status", "surfaced_at", "attempts", "outcome",
                    "source"}


def refuse(msg, backup_path=None, write_attempted=False):
    """write_attempted=True means the backfill subprocess was already invoked
    WITH --write (its backup, if any, is in backup_path) when it failed. At
    that point a contract-mismatch error, a timeout, or the process being
    killed between its COMMIT and this wrapper noticing are indistinguishable
    from each other -- the database may already carry the write. Never call
    that "untouched"; say the state is unknown and point at the backup to
    restore from. Every other refusal happens before the backfill subprocess
    runs at all (or, for a dry run, the C side never commits without
    --write), so "untouched" / "nothing written" is exact."""
    if write_attempted:
        tail = (f"database state is UNKNOWN after the --write attempt; restore from the "
                 f"backup at {backup_path} if you need a known-good database" if backup_path
                 else "database state is UNKNOWN after the --write attempt (no backup exists "
                      "to restore from)")
    else:
        tail = (f"database untouched; backup kept at {backup_path}" if backup_path
                else "nothing written")
    print(f"refusing: {msg}; {tail}", file=sys.stderr)
    return EXIT_REFUSED


def prepare_manifest_dir(d):
    """The manifest dir must be usable BEFORE the backup and the backfill run,
    so a bad one is a refusal rather than a failure after the database was
    written. Created 0700 when missing; never a symlink; proven writable by
    creating and removing a probe file. Raises OSError otherwise."""
    if os.path.islink(d):
        raise OSError(f"manifest dir is a symlink ({d})")
    os.makedirs(d, mode=0o700, exist_ok=True)
    if not os.path.isdir(d):
        raise NotADirectoryError(d)
    fd, probe = tempfile.mkstemp(dir=d, prefix=".prospective-probe-")
    os.close(fd)
    os.unlink(probe)


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
    try:
        prepare_manifest_dir(a.manifest_dir)
    except OSError as e:
        return refuse(f"manifest dir unusable ({e.__class__.__name__}: {a.manifest_dir})")
    backup_path = None
    if a.write:
        try:
            backup_path = backup(a.db, a.backup_dir, stamp)
        except (OSError, sqlite3.Error) as e:
            return refuse(f"backup failed ({e.__class__.__name__})")
    try:
        counts = run_backfill(a.human_bin, a.db, a.write, now)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as e:
        return refuse(f"backfill failed ({e})", backup_path, write_attempted=a.write)
    payload = {"schema_version": 1, "measured_at": stamp,
               "mode": "write" if a.write else "dry_run",
               "backup_written": backup_path is not None, "counts": counts}
    try:
        path = cn.write_jsonl_private(
            os.path.join(a.manifest_dir, f"prospective-backfill-{stamp}.json"), [payload])
    except OSError as e:
        # Second line of defence: the dir was checked, but the write still
        # failed. The backfill already ran, so keep its counts.
        print(json.dumps(counts))
        done = (f"database WAS written (backup at {backup_path})" if a.write
                else "dry run, database untouched")
        print(f"manifest not written ({e.__class__.__name__}); {done}; counts are on stdout",
              file=sys.stderr)
        return EXIT_NO_MANIFEST
    print(json.dumps(counts))
    print(f"wrote {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
