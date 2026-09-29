# scripts/second_opinion/run_nightly.py
"""Nightly runner for the second-opinion lane (spec §5, §6).

cd scripts && python3 -m second_opinion.run_nightly --deadline 09:00
Exit: 0 done / window closed / another run holds the lock / job(s) failed but
not every attempted job or item did; 2 refused (nothing written); 3 every
attempted job raised, or every attempted item failed (manifest still
written).

Each job (audit, gold, judge, the two report writes) is isolated in its own
try/except: an exception there is recorded as {"error": "<ExceptionType>"}
(never the exception's message/args — those can carry note text or handles)
under that job's manifest key, counted as a failed job, and the run continues
with the next job. `finish_run` always runs once the run row exists, even if
something outside those per-job guards raises."""
import argparse
import contextlib
import datetime as dt
import fcntl
import json
import os
import re
import sqlite3
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from insight_stream import resolve_deadline  # noqa: E402

from . import audit, backend as be, gold, judge, store  # noqa: E402

HOME = os.path.expanduser("~")
JOBS = ("audit", "gold", "judge", "report")
_HHMM_RE = re.compile(r"^([01]\d|2[0-3]):([0-5]\d)$")


def resolve_jobs(spec, today):
    if spec == "auto":
        return ["audit", "gold"] + (["judge", "report"] if today.weekday() == 6 else [])
    jobs = [j.strip() for j in spec.split(",") if j.strip()]
    bad = [j for j in jobs if j not in JOBS]
    if bad:
        raise ValueError(f"unknown jobs: {bad}")
    return jobs


def _hhmm(value):
    if not _HHMM_RE.match(value):
        raise argparse.ArgumentTypeError(f"--deadline must be HH:MM (24h), got {value!r}")
    return value


def _ro(path):
    con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    con.execute("SELECT name FROM sqlite_master LIMIT 1").fetchall()
    return con


def _atomic_json(path, obj):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    tmp = path + ".tmp"
    with store.private_open(tmp) as f:
        json.dump(obj, f, indent=1, sort_keys=True)
    os.replace(tmp, path)


def _parse(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--jobs", default="auto")
    ap.add_argument("--judge-run-dir")
    ap.add_argument("--blind-ab-root", default=os.path.join(HOME, "blind_ab_run"))
    ap.add_argument("--backend", choices=["gemma", "vertex"], default="gemma")
    ap.add_argument("--deadline", type=_hhmm, help="HH:MM local; stop between items after this")
    ap.add_argument("--audit-limit", type=int, default=25)
    ap.add_argument("--gold-limit", type=int, default=10)
    ap.add_argument("--store", default=store.DEFAULT_PATH)
    ap.add_argument("--manifest-dir", default=os.path.join(HOME, ".human/logs"))
    ap.add_argument("--reports-dir",
                    default=os.path.join(HOME, ".human/logs/second-opinion-reports"))
    ap.add_argument("--mem-db", default=os.path.join(HOME, ".human/memory.db"))
    ap.add_argument("--chat-db", default=os.path.join(HOME, "Library/Messages/chat.db"))
    ap.add_argument("--lock", default=os.path.join(HOME, ".human/second_opinion.lock"))
    ap.add_argument("--dry-run", action="store_true")
    return ap.parse_args(argv)


def _default_attribute(chat_path, mem_path, since):
    import eval_conversation_quality as cq
    return cq.attribute(chat_path, mem_path, since)


def _default_utcnow():
    return dt.datetime.now(dt.timezone.utc)


def main(argv=None, *, now_local=None, serve=be.serve_gemma, attribute=None,
         vertex_token=be._adc_token, utcnow=_default_utcnow):
    try:
        a = _parse(argv)
    except SystemExit as e:
        # A malformed flag (e.g. --deadline 25:00) refuses before anything is
        # touched; argparse's own exit(2) IS the refusal contract, just
        # normalized to a return so callers never see a raised SystemExit.
        return e.code if isinstance(e.code, int) else 2

    now_local = now_local or dt.datetime.now().astimezone()
    attribute = attribute or _default_attribute
    try:
        jobs = resolve_jobs(a.jobs, now_local.date())
    except ValueError as e:
        print(f"refusing: {e}", file=sys.stderr)
        return 2

    deadline = None
    if a.deadline:
        deadline = resolve_deadline(a.deadline, now_local)
        if deadline is None:
            print("window closed; nothing to do", file=sys.stderr)
            return 0

    os.makedirs(os.path.dirname(a.lock) or ".", exist_ok=True)
    try:
        # O_NOFOLLOW: refuse a --lock pointed at a symlink rather than follow
        # it into an unrelated file. No O_TRUNC: a misdirected --lock must
        # never zero out whatever it names.
        lock_fd = os.open(a.lock, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    except OSError as e:
        print(f"refusing: cannot open lock file ({e})", file=sys.stderr)
        return 2
    lock = os.fdopen(lock_fd, "r+")
    try:
        return _run_locked(a, jobs, deadline, now_local, serve, attribute, vertex_token, utcnow,
                           lock)
    finally:
        lock.close()  # released on every path: normal return, refusal, or exception


def _run_locked(a, jobs, deadline, now_local, serve, attribute, vertex_token, utcnow, lock):
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print("another second-opinion run holds the lock; nothing to do", file=sys.stderr)
        return 0

    try:
        mem = _ro(a.mem_db)
        chat = _ro(a.chat_db)
    except sqlite3.Error as e:
        print(f"refusing: memory.db or chat.db unreadable ({e})", file=sys.stderr)
        return 2

    if a.backend == "vertex":
        try:
            vertex_token()
        except be.BackendError as e:
            # Credential preflight, before open_store/start_run and before any
            # manifest path is touched: spec §5 requires this refuse exit 2
            # and write nothing, not attempt every item and exit 3.
            print(f"refusing: {e}", file=sys.stderr)
            return 2
        ctx = contextlib.nullcontext(be.VertexBackend(allow=True, token=vertex_token))
    else:
        ctx = serve(log_path=os.path.join(a.manifest_dir, "second-opinion-gemma.log")
                    if not a.dry_run else None)

    started = time.monotonic()
    man = {"jobs": jobs, "dry_run": a.dry_run, "exit_reason": "ok"}
    try:
        with ctx as backend:
            con = store.open_store(":memory:" if a.dry_run else a.store)
            run_id = store.start_run(con, backend.name, ",".join(jobs), store.now_ms())
            man["backend"] = backend.name
            rc = 0
            try:
                rc = _run_jobs(a, jobs, deadline, now_local, attribute, utcnow, con, backend, mem,
                               chat, man)
            finally:
                man["elapsed_s"] = round(time.monotonic() - started, 1)
                store.finish_run(con, run_id, rc, json.dumps(man), store.now_ms())
    except be.BackendError as e:
        print(f"refusing: {e}", file=sys.stderr)
        return 2
    suffix = "-dryrun" if a.dry_run else ""
    _atomic_json(os.path.join(a.manifest_dir,
                              f"second-opinion-{now_local.strftime('%Y%m%d')}{suffix}.json"), man)
    return rc


def _run_jobs(a, jobs, deadline, now_local, attribute, utcnow, con, backend, mem, chat, man):
    item_attempted = item_errors = 0
    jobs_run = jobs_failed = 0

    if "audit" in jobs:
        jobs_run += 1
        try:
            man["audit"] = audit.audit_pass(con, backend, mem, chat, a.audit_limit, deadline)
            item_attempted += man["audit"]["attempted"]
            item_errors += man["audit"]["errors"]
        except Exception as e:
            man["audit"] = {"error": type(e).__name__}
            jobs_failed += 1

    if "gold" in jobs:
        jobs_run += 1
        try:
            since = now_local - dt.timedelta(days=7)
            try:
                att = attribute(a.chat_db, a.mem_db, since)
            except Exception:
                att = None  # critiques still run; reference replies need attribution
                man["gold_attribution_error"] = 1
            runs = [d for d in [judge.latest_run_dir(a.blind_ab_root)] if d]
            man["gold"] = gold.gold_pass(con, backend, runs, att, mem, a.gold_limit, deadline,
                                         wide_live=os.environ.get("HU_INSIGHT_WIDE") == "live")
            item_attempted += man["gold"]["attempted"]
            item_errors += man["gold"]["errors"]
        except Exception as e:
            man["gold"] = {"error": type(e).__name__}
            jobs_failed += 1

    if "judge" in jobs:
        attempted_now = False
        try:
            run_dir = a.judge_run_dir or judge.latest_run_dir(a.blind_ab_root)
            if run_dir is None:
                man["judge"] = {"skipped": "no rating sheet"}
            elif a.dry_run:
                # Never for real in dry-run: judge_pass writes message text to
                # reports-dir and merge-writes ~/.human/blind_ab_gate.json.
                man["judge"] = {"skipped": "dry_run"}
            elif deadline is not None and utcnow() >= deadline:
                man["judge"] = {"skipped": "deadline_skipped"}
            else:
                attempted_now = True
                jobs_run += 1
                stamp = now_local.strftime("%Y%m%d")
                out = os.path.join(a.reports_dir, f"judge-{stamp}")
                remaining = None
                if deadline is not None:
                    remaining = max(0.0, (deadline - utcnow()).total_seconds())
                man["judge"] = judge.judge_pass(backend, run_dir, out, timeout=remaining)
                if "calibration" in man["judge"]:
                    _atomic_json(os.path.join(a.reports_dir, f"judge-{stamp}.json"),
                                man["judge"]["calibration"])
        except Exception as e:
            man["judge"] = {"error": type(e).__name__}
            if not attempted_now:
                jobs_run += 1
            jobs_failed += 1

    if "report" in jobs and not a.dry_run:
        jobs_run += 1
        week_ago = store.now_ms() - 7 * 86400 * 1000
        stamp = now_local.strftime("%Y%m%d")
        report_errors = 0
        try:
            _atomic_json(os.path.join(a.reports_dir, f"audit-{stamp}.json"),
                        audit.audit_report(con, week_ago))
        except Exception as e:
            man.setdefault("report", {})["audit_error"] = type(e).__name__
            report_errors += 1
        try:
            _atomic_json(os.path.join(a.reports_dir, f"gold-{stamp}.json"),
                        gold.gold_report(con, week_ago))
        except Exception as e:
            man.setdefault("report", {})["gold_error"] = type(e).__name__
            report_errors += 1
        if report_errors == 2:
            jobs_failed += 1

    if jobs_run > 0 and jobs_failed == jobs_run:
        man["exit_reason"] = "every job failed"
        return 3
    if item_attempted > 0 and item_errors == item_attempted:
        man["exit_reason"] = "every item failed"
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
