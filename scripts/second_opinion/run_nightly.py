# scripts/second_opinion/run_nightly.py
"""Nightly runner for the second-opinion lane (spec §5, §6).

cd scripts && python3 -m second_opinion.run_nightly --deadline 09:00
Exit: 0 done / window closed / another run holds the lock; 2 refused (nothing
written); 3 every attempted item failed (manifest still written)."""
import argparse
import contextlib
import datetime as dt
import fcntl
import json
import os
import sqlite3
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from insight_stream import resolve_deadline  # noqa: E402

from . import audit, backend as be, gold, judge, store  # noqa: E402

HOME = os.path.expanduser("~")
JOBS = ("audit", "gold", "judge", "report")


def resolve_jobs(spec, today):
    if spec == "auto":
        return ["audit", "gold"] + (["judge", "report"] if today.weekday() == 6 else [])
    jobs = [j.strip() for j in spec.split(",") if j.strip()]
    bad = [j for j in jobs if j not in JOBS]
    if bad:
        raise SystemExit(f"unknown jobs: {bad}")
    return jobs


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
    ap.add_argument("--deadline", help="HH:MM local; stop between items after this")
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


def main(argv=None, *, now_local=None, serve=be.serve_gemma, attribute=None):
    a = _parse(argv)
    now_local = now_local or dt.datetime.now().astimezone()
    attribute = attribute or _default_attribute
    jobs = resolve_jobs(a.jobs, now_local.date())

    deadline = None
    if a.deadline:
        deadline = resolve_deadline(a.deadline, now_local)
        if deadline is None:
            print("window closed; nothing to do", file=sys.stderr)
            return 0

    os.makedirs(os.path.dirname(a.lock) or ".", exist_ok=True)
    lock = open(a.lock, "w")
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
        ctx = contextlib.nullcontext(be.VertexBackend(allow=True))
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
            attempted = errors = 0
            if "audit" in jobs:
                man["audit"] = audit.audit_pass(con, backend, mem, chat, a.audit_limit, deadline)
                attempted += man["audit"]["attempted"]
                errors += man["audit"]["errors"]
            if "gold" in jobs:
                since = now_local - dt.timedelta(days=7)
                try:
                    att = attribute(a.chat_db, a.mem_db, since)
                except Exception:
                    att = None  # critiques still run; reference replies need attribution
                    man["gold_attribution_error"] = 1
                runs = [d for d in [judge.latest_run_dir(a.blind_ab_root)] if d]
                man["gold"] = gold.gold_pass(con, backend, runs, att, mem, a.gold_limit, deadline,
                                             wide_live=os.environ.get("HU_INSIGHT_WIDE") == "live")
                attempted += man["gold"]["attempted"]
                errors += man["gold"]["errors"]
            if "judge" in jobs:
                run_dir = a.judge_run_dir or judge.latest_run_dir(a.blind_ab_root)
                if run_dir is None:
                    man["judge"] = {"skipped": "no rating sheet"}
                else:
                    stamp = now_local.strftime("%Y%m%d")
                    out = os.path.join(a.reports_dir, f"judge-{stamp}")
                    try:
                        man["judge"] = judge.judge_pass(backend, run_dir, out)
                    except Exception as e:
                        man["judge"] = {"error": type(e).__name__}
                    if not a.dry_run and "calibration" in man["judge"]:
                        _atomic_json(os.path.join(a.reports_dir, f"judge-{stamp}.json"),
                                     man["judge"]["calibration"])
            if "report" in jobs and not a.dry_run:
                week_ago = store.now_ms() - 7 * 86400 * 1000
                stamp = now_local.strftime("%Y%m%d")
                _atomic_json(os.path.join(a.reports_dir, f"audit-{stamp}.json"),
                             audit.audit_report(con, week_ago))
                _atomic_json(os.path.join(a.reports_dir, f"gold-{stamp}.json"),
                             gold.gold_report(con, week_ago))
            rc = 0
            if attempted > 0 and errors == attempted:
                rc = 3
                man["exit_reason"] = "every item failed"
            man["elapsed_s"] = round(time.monotonic() - started, 1)
            store.finish_run(con, run_id, rc, json.dumps(man), store.now_ms())
    except be.BackendError as e:
        print(f"refusing: {e}", file=sys.stderr)
        return 2
    suffix = "-dryrun" if a.dry_run else ""
    _atomic_json(os.path.join(a.manifest_dir,
                              f"second-opinion-{now_local.strftime('%Y%m%d')}{suffix}.json"), man)
    return rc


if __name__ == "__main__":
    sys.exit(main())
