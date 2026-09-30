#!/usr/bin/env python3
"""One-time retype of UNKNOWN graph entities (spec 2026-09-29 named-entity-extraction §4.5).

Asks the LOCAL model to type each contact's UNKNOWN names in batches of 40, then
backs graph.db up with the SQLite online backup API (0600, verified read-only by
integrity_check and an equal entity count) right BEFORE the only write, and writes
the answers through `human memory import-facts` as retype-only entity lines
(source names:migrate). Backing up after the model phase keeps the window of
daemon writes a restore would discard to seconds, not the model phase's minutes. Python never writes graph.db. Nothing is deleted
and nothing is created: an unanswered name stays UNKNOWN, a name that vanished
since the read is skipped by the importer, and the importer's retype policy
never downgrades a row that got a name type in the meantime. Idempotent: a
re-run only sees what is still UNKNOWN.

  --dry-run  counts only: no model, no backup, no import
  --write    importer capability probe (throwaway temp graph) -> classify ->
             lock probe -> verified backup -> import

A model answer's name is matched to its batch case-insensitively and written back
in the STORED spelling (retype-only keys on contact_id + the stored name); a match
on two stored names that differ only by case is ambiguous and skipped. stdout is
counts only.

Locked graph: the lock probe refuses (exit 2) when another connection holds the
write lock for more than 1 s. After it, the importer waits up to 5 s per write
(busy_timeout); a write that still cannot get the lock is skipped and stays
UNKNOWN, so a re-run picks it up.

Deploy first: `--human-bin` must import entity lines (this branch's importer).
Before the model phase the script imports one probe line into a throwaway temp
graph and refuses (exit 2) unless the binary reports entities >= 1.

Restore -- discards EVERY daemon write made to graph.db after the backup was taken:
  launchctl bootout gui/$(id -u)/ai.human.service-loop   # KeepAlive restarts a kill
  lsof ~/.human/graph.db                                 # must print nothing
  rm -f ~/.human/graph.db-wal ~/.human/graph.db-shm      # before the copy
  cp <backup> ~/.human/graph.db && chmod 600 ~/.human/graph.db
  launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/ai.human.service-loop.plist
"""
import argparse
import contextlib
import datetime as dt
import json
import os
import re
import sqlite3
import sys
import tempfile
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import curator_names as cn  # noqa: E402
from insight_stream import call_model, is_loopback_url  # noqa: E402  (module attrs: tests patch)

HOME = os.path.expanduser("~")
UNKNOWN = 6  # HU_ENTITY_UNKNOWN in include/human/memory/graph.h
BATCH = 40
SOURCE = "names:migrate"
CONFIDENCE = 0.6
DEFAULT_URL = "http://127.0.0.1:8741/v1/chat/completions"
DEFAULT_MODEL = "GLM-4.5-Air-4bit"
PHONE_RE = re.compile(r"[+()\-. \d]{7,}")
LOCK_WAIT_S = 1.0
SERVICE = "ai.human.service-loop"
PROBE_LINE = {"kind": "entity", "contact": "probe", "name": "Probe", "type": "topic",
              "source": "names:probe", "confidence": 0.5}

SYSTEM = (
    "Each line below is a name or phrase from Seth Ford's texts with one person. Type "
    "each one: person, place, org (a company, school, team or group), event (a named "
    "occasion: a trip, a wedding, a game), or topic (anything that is not a proper name). "
    "Skip a line you cannot tell.\n\n"
    "Output ONLY a JSON array of objects: {\"name\": the line exactly as given, \"type\": "
    "\"person\"|\"place\"|\"org\"|\"event\"|\"topic\"}. No prose before or after."
)


def _ro(path):
    return sqlite3.connect(f"file:{path}?mode=ro", uri=True)


def unknown_entities(con):
    """{contact: [name, ...]} of UNKNOWN rows, skipping the contact's own id,
    phone-number-shaped names (placeholders, not names) and names with a line
    break (the model sees one name per line and could not echo them)."""
    out = {}
    for cid, name in con.execute(
            "SELECT contact_id, name FROM entities WHERE type = ? ORDER BY contact_id, id",
            (UNKNOWN,)):
        if (not name or name == cid or PHONE_RE.fullmatch(name)
                or "\n" in name or "\r" in name):
            continue
        out.setdefault(cid, []).append(name)
    return out


def batches(names, size=BATCH):
    return [names[i:i + size] for i in range(0, len(names), size)]


def parse_types(text, batch, stats=None):
    """Model answer -> {stored name: type}. An answer's name is matched to THIS
    batch's names case-insensitively (surrounding whitespace aside) and mapped back
    to the STORED spelling, because retype-only keys on contact_id + the stored name
    and the model often changes a name's casing. Counted, never mapped:
    stats["ambiguous"] when two stored names in the batch differ only by case and the
    answer matches them; stats["ignored"] for a name not in the batch, an unknown
    type, or a repeat answer for a stored name (the first wins). An answer with no
    JSON array counts stats["parse_failed"] and yields {}."""
    stats = stats if stats is not None else {}
    arr = cn.parse_answer(text)
    if arr is None:
        stats["parse_failed"] = stats.get("parse_failed", 0) + 1
        return {}
    by_lower = {}
    for n in batch:
        by_lower.setdefault(n.strip().lower(), []).append(n)
    out = {}
    for o in arr:
        o = o if isinstance(o, dict) else {}
        name = o.get("name") if isinstance(o.get("name"), str) else ""
        stored = by_lower.get(name.strip().lower(), [])
        t = str(o.get("type") or "").strip().lower()
        if len(stored) > 1:
            stats["ambiguous"] = stats.get("ambiguous", 0) + 1
        elif stored and t in cn.TYPES and stored[0] not in out:
            out[stored[0]] = t
        else:
            stats["ignored"] = stats.get("ignored", 0) + 1
    return out


def lock_probe(path):
    """None when no other connection holds the write lock, else the error text.
    BEGIN IMMEDIATE + ROLLBACK takes and releases the lock without writing;
    mode=rw never creates a missing graph."""
    try:
        con = sqlite3.connect(f"file:{path}?mode=rw", uri=True, timeout=LOCK_WAIT_S,
                              isolation_level=None)
        try:
            con.execute("BEGIN IMMEDIATE")
            con.execute("ROLLBACK")
        finally:
            con.close()
        return None
    except sqlite3.Error as e:
        return str(e)


def _private_dir(d):
    if os.path.islink(d):
        raise OSError(f"backup dir is a symlink ({d})")
    os.makedirs(d, mode=0o700, exist_ok=True)
    os.chmod(d, 0o700)


def backup(path, backup_dir, now):
    """Online backup to <dir>/graph.db.bak-retype-<ts> (0600, dir 0700). The source
    is read read-only inside one read transaction, so the counted entities and the
    copied pages are the same snapshot even while the daemon writes (WAL). Verified
    by reopening the copy read-only: integrity_check == ok and the same entity count.
    Raises on any failure, after removing the partial copy."""
    _private_dir(backup_dir)
    dst_path = os.path.join(backup_dir, f"graph.db.bak-retype-{now.strftime('%Y%m%d-%H%M%S')}")
    fd = os.open(dst_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    os.close(fd)
    try:
        src = sqlite3.connect(f"file:{path}?mode=ro", uri=True, isolation_level=None)
        dst = sqlite3.connect(dst_path, isolation_level=None)
        try:
            src.execute("BEGIN")
            n_src = src.execute("SELECT COUNT(*) FROM entities").fetchone()[0]
            src.backup(dst)
            src.execute("ROLLBACK")
            # A standalone file: no -wal/-shm siblings needed to read or restore it.
            dst.execute("PRAGMA journal_mode=DELETE")
        finally:
            dst.close()
            src.close()
        os.chmod(dst_path, 0o600)
        chk = _ro(dst_path)
        try:
            ok = chk.execute("PRAGMA integrity_check").fetchone()[0]
            n_dst = chk.execute("SELECT COUNT(*) FROM entities").fetchone()[0]
        finally:
            chk.close()
        if ok != "ok" or n_src != n_dst:
            raise RuntimeError(f"backup verification failed (integrity={ok}, "
                               f"entities {n_dst}/{n_src})")
        return dst_path
    except BaseException:
        for p in (dst_path, dst_path + "-journal", dst_path + "-wal", dst_path + "-shm"):
            with contextlib.suppress(OSError):
                os.remove(p)
        raise


def refuse(msg):
    print(f"refusing: {msg}; graph.db unchanged", file=sys.stderr)
    return 2


def restore_steps(graph_db, backup_path):
    g = graph_db
    return (f"Restore (discards EVERY daemon write made to {g} after the backup was taken):\n"
            f"  launchctl bootout gui/$(id -u)/{SERVICE}\n"
            f"  lsof {g}    # must print nothing\n"
            f"  rm -f {g}-wal {g}-shm\n"
            f"  cp {backup_path} {g} && chmod 600 {g}\n"
            f"  launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/{SERVICE}.plist")


def import_capable(human_bin):
    """True when `human_bin` imports an entity line: one probe line into a throwaway
    temp graph (HU_GRAPH_DB points there; the real graph is never named). A binary
    that predates entity lines skips it and reports entities 0."""
    with tempfile.TemporaryDirectory(prefix="retype-probe-") as d:
        path = cn.write_jsonl_private(os.path.join(d, "probe.jsonl"), [PROBE_LINE])
        n, code = cn.run_import(human_bin, os.path.join(d, "probe.db"), path, timeout=60)
    return code == 0 and n is not None and n >= 1


def count_unknown(path):
    try:
        con = _ro(path)
        try:
            return sum(len(v) for v in unknown_entities(con).values())
        finally:
            con.close()
    except sqlite3.Error:
        return None


def main(argv=None):
    ap = argparse.ArgumentParser(description="Retype UNKNOWN graph entities (one-time).")
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--dry-run", action="store_true")
    mode.add_argument("--write", action="store_true")
    ap.add_argument("--graph-db", default=os.path.join(HOME, ".human/graph.db"))
    ap.add_argument("--backup-dir", default=os.path.join(HOME, ".human/backups"))
    ap.add_argument("--work-dir", default=os.path.join(HOME, ".human/names"))
    ap.add_argument("--human-bin", default=os.path.join(HOME, ".local/bin/human-daemon"))
    ap.add_argument("--url", default=DEFAULT_URL)
    ap.add_argument("--model", default=DEFAULT_MODEL)
    a = ap.parse_args(argv)
    if not is_loopback_url(a.url):
        return refuse("the migration reads real names and only talks to a local model")
    try:
        con = _ro(a.graph_db)
        try:
            todo = unknown_entities(con)
        finally:
            con.close()
    except sqlite3.Error as e:
        return refuse(f"graph.db unreadable ({e})")
    counts = {"unknown_entities": sum(len(v) for v in todo.values()), "contacts": len(todo),
              "batches": sum(len(batches(v)) for v in todo.values())}
    if a.dry_run:
        print(json.dumps(counts, sort_keys=True))
        return 0

    def done(rc=0):
        counts["unknown_after"] = count_unknown(a.graph_db)  # the authoritative figure
        print(json.dumps(counts, sort_keys=True))
        return rc

    counts["import_entities"] = 0
    if not todo:
        return done()
    if not os.access(a.human_bin, os.X_OK):
        return refuse(f"human binary not executable ({a.human_bin})")
    if not import_capable(a.human_bin):
        return refuse("human binary cannot import entity lines -- deploy the new build first")
    try:
        urllib.request.urlopen(a.url.rsplit("/v1/", 1)[0] + "/health", timeout=5)
    except Exception as e:
        return refuse(f"model server down ({type(e).__name__})")
    lines, errors, stats = [], 0, {"ignored": 0, "ambiguous": 0, "parse_failed": 0}
    by_type = dict.fromkeys(cn.TYPES, 0)
    for cid, names in todo.items():
        for batch in batches(names):
            try:
                text = call_model(a.url, a.model, SYSTEM, "\n".join(batch), max_tokens=2000)
            except Exception:
                errors += 1
                continue
            kept = [{"name": n, "type": t} for n, t in parse_types(text, batch, stats).items()]
            for k in kept:
                by_type[k["type"]] += 1
            lines.extend(cn.entity_lines(cid, kept, source=SOURCE, confidence=CONFIDENCE,
                                         retype_only=True))
    counts.update(answered=len(lines), unanswered=counts["unknown_entities"] - len(lines),
                  model_errors=errors, parse_failed=stats["parse_failed"],
                  ignored=stats["ignored"], ambiguous=stats["ambiguous"], by_type=by_type)
    if errors + stats["parse_failed"] == counts["batches"]:
        print("every batch failed (model error or unparseable); nothing imported",
              file=sys.stderr)
        return done(3)
    if not lines:
        return done()
    # The only write follows. Lock probe and verified backup sit right before it, so
    # a restore loses seconds of daemon writes rather than the whole model phase.
    locked = lock_probe(a.graph_db)
    if locked:
        return refuse(f"graph.db is locked ({locked}); retry when the daemon is idle")
    now = dt.datetime.now()
    try:
        counts["backup"] = backup(a.graph_db, a.backup_dir, now)
    except Exception as e:
        return refuse(f"backup failed ({type(e).__name__}: {e})")
    path = cn.write_jsonl_private(
        os.path.join(a.work_dir, f"retype-{now.strftime('%Y%m%d-%H%M%S')}.jsonl"), lines)
    imported, code = cn.run_import(a.human_bin, a.graph_db, path)
    if code != 0 and imported == 0:
        done()  # the CLI ran and exits non-zero only when nothing was imported
        return refuse("`human memory import-facts` imported nothing")
    if code != 0 or imported is None:
        # Timeout (the child was killed after committing some per-row writes) or
        # unreadable output: never claim the graph is unchanged.
        done()
        print("`human memory import-facts` did not finish; graph.db may be partially "
              f"retyped. Re-run (idempotent) or restore the backup {counts['backup']}.\n"
              + restore_steps(a.graph_db, counts["backup"]), file=sys.stderr)
        return 2
    # import_entities counts every matched row, retyped or refused by the no-downgrade
    # policy; unknown_after (re-read) is what is actually left UNKNOWN.
    counts["import_entities"] = imported
    print(f"done. backup: {counts['backup']}\n" + restore_steps(a.graph_db, counts["backup"]),
          file=sys.stderr)
    return done()

if __name__ == "__main__":
    sys.exit(main())
