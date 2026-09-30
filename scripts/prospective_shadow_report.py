#!/usr/bin/env python3
"""The §3 SHADOW numbers for prospective memory v2 (spec
docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §3).

Reads the daemon's dated service log (hu_log lines written by
hu_daemon_prospective_log_counts in src/daemon/daemon_prospective.c /
daemon_prospective_time.c) and memory.db READ-ONLY. Emits counts and rates
only -- no message text, no contact handles/names, no intention text -- for
the SHADOW gate over [--since, --until):

  would_fire_per_day        fire verdicts (shadow counts lines) / days
  resolved_before_cue_rate  already_resolved verdicts / judged candidates
  cue_to_fire_rate          keyword intentions with a fire verdict / keyword
                            intentions whose cue arrived in an inbound text
                            in the window
  uptake                    would-fires the delivered reply carried / would-
                            fires (from the "shadow uptake" line; keyword only
                            -- the daemon never emits a time-cue uptake line)
  miss_rate                 keyword intentions PENDING AT THE WINDOW START
                            (status='pending', created before --since) that
                            never produced an item line in the window at all
                            / that same pending-at-start set. Controller
                            ruling F6: the denominator is the pending set, not
                            "cued during the window" -- a cue is drawn from
                            live inbound text and undercounts intentions that
                            were already due for judging before anyone typed
                            anything.
  duplicate_rate            NOT MEASURABLE in SHADOW (ruling F5): SHADOW never
                            writes state (apply=false), so a still-pending
                            intention would-fires again on every later pass
                            and every later day it stays pending -- the
                            "fired on >1 day" count is structural, not a
                            signal about the judge. Always null, with a
                            machine-readable reason in `notes`, and excluded
                            from `rates` entirely (no number is emitted).
  judge_failure_rate        judge_err / candidates. EXCLUDES write_err
                            (ruling: write_err is a store fault after a FIRE
                            verdict, not a judge failure) -- write_err is
                            reported as its own count instead.
  time_*                    the same shape for time cues (time_would_fire_per_day,
                            time_resolved_rate, time_miss_rate, time_judge_failure_rate,
                            time_duplicate_rate=null). time_miss_rate keeps the
                            due-in-window denominator from the design brief
                            (time cues have no inbound-text "cue" concept, so
                            the F6 pending-at-start correction does not apply
                            the same way -- documented in `notes`); misses are
                            due intentions with no "time shadow item" line.

A rate whose denominator is 0 is `null`. Exit 0 on success, writing
~/.human/logs/prospective-shadow-<until>.json (0600, via the shared private
writer scripts/curator_names.py write_jsonl_private -- ruling F15). Exit 2
(nothing written) when:
  - the window holds no SHADOW log line at all, or
  - memory.db is missing or not migrated to the v2 columns, or
  - any log line matching the "prospective ... shadow ..." prefix fails to
    parse as one of the three known shapes (counts / item / uptake) with its
    required fields. Policy choice, documented here because it is not
    obvious: ANY malformed shadow line refuses the whole report, rather than
    skipping it or tolerating a count under some threshold. A malformed line
    most often means the log format drifted out from under this parser, and
    a drifted parser silently under-counts every OTHER number in the same
    report too -- so partial trust is worse than no report.

Log timestamps are LOCAL time (hu_log_format_line) -- the window is parsed
and compared in local time, via time.mktime/time.strptime.
"""
import argparse
import json
import os
import re
import sqlite3
import sys
import time
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import curator_names as cn  # noqa: E402
import eval_prospective_memory as epm  # noqa: E402

HOME = os.path.expanduser("~")

# The loose "is this even a shadow-tagged prospective line" prefix. Deliberately
# does not validate the timestamp's digit shapes -- an unparseable timestamp on
# an otherwise shadow-shaped line is exactly the kind of drift this report must
# not silently swallow (see MALFORMED handling in read_log).
PREFIX = re.compile(r"^(\S+)\s+\S+\s+\[prospective\]\s+(prospective .*shadow.*)$")
COUNTS = re.compile(r"^prospective (time )?shadow: (.+)$")
ITEM = re.compile(r"^prospective (time )?shadow item: (.+)$")
UPTAKE = re.compile(r"^prospective shadow uptake: (.+)$")
KV = re.compile(r"(\w+)=(\S+)")

COUNT_FIELDS = ("candidates", "fire", "resolved", "cancel", "not_now", "parse_fail",
                "judge_err", "expired", "capped")  # write_err is optional (older logs)


def refuse(msg):
    print(f"refusing: {msg}; nothing written", file=sys.stderr)
    return 2


def parse_day(s):
    """--since/--until are UTC-midnight day boundaries (matches
    eval_prospective_memory.inbound_by_contact's UTC treatment of
    messages.created_at)."""
    return int(datetime.strptime(s, "%Y-%m-%d").replace(tzinfo=timezone.utc).timestamp())


def local_epoch(stamp):
    """hu_log_format_line writes LOCAL time."""
    return int(time.mktime(time.strptime(stamp, "%Y-%m-%dT%H:%M:%S")))


def read_log(path, since, until):
    """-> (rows, malformed_count). rows: [{"ts", "time": bool, "kind", "kv"}].
    kind in {"counts", "item", "uptake"}. Only rows inside [since, until) are
    kept; an unparseable timestamp on a shadow-shaped line cannot be placed in
    the window, so it is always counted as malformed regardless of window."""
    rows = []
    malformed = 0
    with open(path, errors="replace") as f:
        for line in f:
            m = PREFIX.match(line.rstrip("\n"))
            if not m:
                continue
            ts_str, rest = m.groups()
            try:
                ts = local_epoch(ts_str)
            except ValueError:
                malformed += 1
                continue
            if not (since <= ts < until):
                continue
            cm, im, um = COUNTS.match(rest), ITEM.match(rest), UPTAKE.match(rest)
            if cm:
                is_time, kv = bool(cm.group(1)), dict(KV.findall(cm.group(2)))
                if not all(k in kv for k in COUNT_FIELDS):
                    malformed += 1
                    continue
                rows.append({"ts": ts, "time": is_time, "kind": "counts", "kv": kv})
            elif im:
                is_time, kv = bool(im.group(1)), dict(KV.findall(im.group(2)))
                if "id" not in kv or "verdict" not in kv or not kv["id"].isdigit():
                    malformed += 1
                    continue
                rows.append({"ts": ts, "time": is_time, "kind": "item", "kv": kv})
            elif um:
                kv = dict(KV.findall(um.group(1)))
                if "would_fire" not in kv or "used" not in kv:
                    malformed += 1
                    continue
                rows.append({"ts": ts, "time": False, "kind": "uptake", "kv": kv})
            else:
                malformed += 1  # shadow-shaped but not one of the three known lines
    return rows, malformed


def intentions(con):
    """{id: (contact_id, action)} for every row -- the semantic "intention" is
    the (contact, action) pair; several keyword trigger rows can share one."""
    return {i: (c, a) for i, c, a in
            con.execute("SELECT id, contact_id, action FROM prospective_memories")}


def cued_intentions(con, since, until):
    """Distinct (contact, action) keyword intentions whose trigger_value
    word-matches an inbound message in [since, until). Unaffected by F5/F6."""
    inbound = epm.inbound_by_contact(con, since)
    cued = set()
    for tid, cue, contact, action, created in con.execute(
            "SELECT id, trigger_value, contact_id, action, created_at FROM prospective_memories "
            "WHERE trigger_type='keyword' AND fired <> 2"):
        if not cue:
            continue
        rx = epm._word_re(cue)
        for ep, text in inbound.get(contact, ()):
            if max(created or 0, since) < ep < until and rx.search(text):
                cued.add((contact, action))
                break
    return cued


def pending_at_start(con, cue_kind, since):
    """Ruling F6: proxy for "intentions pending at the window start" -- rows
    of `cue_kind` created before --since whose status reads 'pending' now.
    SHADOW never writes (apply=false), so status cannot have moved off
    'pending' due to anything this report is measuring; a LIVE writer
    elsewhere in the same window is the one documented gap (not exercised by
    a SHADOW-only deployment)."""
    return {(c, a) for c, a in con.execute(
        "SELECT DISTINCT contact_id, action FROM prospective_memories "
        "WHERE cue_kind = ? AND status = 'pending' AND created_at < ?", (cue_kind, since))}


def due_in_window(con, since, until):
    """Time cues due in [since, until) -- the design brief's own denominator
    for time misses; kept as-is (see module docstring)."""
    return {(c, a) for c, a in con.execute(
        "SELECT DISTINCT contact_id, action FROM prospective_memories "
        "WHERE cue_kind='time' AND due_at >= ? AND due_at < ?", (since, until))}


def build(rows, con, since, until):
    ids = intentions(con)
    days = (until - since) / 86400.0
    c = {"days": days, "candidates": 0, "fire": 0, "resolved": 0, "cancel": 0, "not_now": 0,
         "parse_fail": 0, "judge_err": 0, "expired": 0, "capped": 0, "write_err": 0,
         "time_candidates": 0, "time_fire": 0, "time_resolved": 0, "time_cancel": 0,
         "time_not_now": 0, "time_parse_fail": 0, "time_judge_err": 0, "time_expired": 0,
         "time_capped": 0, "time_write_err": 0,
         "uptake_would_fire": 0, "uptake_used": 0}

    kw_item_ids, time_item_ids = set(), set()
    kw_fire_id_days, time_fire_id_days = {}, {}
    malformed_items = 0  # ids on an item line with no matching DB row -- excluded, not fatal

    for r in rows:
        kv = r["kv"]
        prefix = "time_" if r["time"] else ""
        if r["kind"] == "counts":
            for f in COUNT_FIELDS:
                c[prefix + f] += int(kv[f])
            c[prefix + "write_err"] += int(kv.get("write_err", 0))
        elif r["kind"] == "uptake":
            c["uptake_would_fire"] += int(kv["would_fire"])
            c["uptake_used"] += int(kv["used"])
        elif r["kind"] == "item":
            iid = int(kv["id"])
            if iid not in ids:
                malformed_items += 1
                continue
            day = r["ts"] // 86400
            if r["time"]:
                time_item_ids.add(iid)
                if kv["verdict"] == "fire":
                    time_fire_id_days.setdefault(iid, set()).add(day)
            else:
                kw_item_ids.add(iid)
                if kv["verdict"] == "fire":
                    kw_fire_id_days.setdefault(iid, set()).add(day)

    def reached(item_ids):
        return {ids[i] for i in item_ids}

    def fired(fire_id_days):
        return {ids[i] for i in fire_id_days}

    cued = cued_intentions(con, since, until)
    kw_pending_start = pending_at_start(con, "keyword", since)
    time_due = due_in_window(con, since, until)

    c["cued_intentions"] = len(cued)
    c["fire_intentions"] = len(fired(kw_fire_id_days))
    c["pending_at_start_intentions"] = len(kw_pending_start)
    c["missed_intentions"] = len(kw_pending_start - reached(kw_item_ids))
    c["time_due_intentions"] = len(time_due)
    c["time_fire_intentions"] = len(fired(time_fire_id_days))
    c["time_missed_intentions"] = len(time_due - reached(time_item_ids))
    c["malformed_items"] = malformed_items
    return c


def rates_of(c):
    def ratio(n, d):
        return (n / d) if d else None

    return {
        "would_fire_per_day": ratio(c["fire"], c["days"]),
        "resolved_before_cue_rate": ratio(c["resolved"], c["candidates"]),
        "cue_to_fire_rate": ratio(c["fire_intentions"], c["cued_intentions"]),
        "uptake": ratio(c["uptake_used"], c["uptake_would_fire"]),
        "miss_rate": ratio(c["missed_intentions"], c["pending_at_start_intentions"]),
        "duplicate_rate": None,  # F5 -- not measurable in SHADOW, see `notes`
        "judge_failure_rate": ratio(c["judge_err"], c["candidates"]),
        "time_would_fire_per_day": ratio(c["time_fire"], c["days"]),
        "time_resolved_rate": ratio(c["time_resolved"], c["time_candidates"]),
        "time_miss_rate": ratio(c["time_missed_intentions"], c["time_due_intentions"]),
        "time_duplicate_rate": None,  # F5
        "time_judge_failure_rate": ratio(c["time_judge_err"], c["time_candidates"]),
    }


NOTES = {
    "duplicate_rate": "not_measurable_in_shadow",
    "time_duplicate_rate": "not_measurable_in_shadow",
    "miss_rate_denominator": "pending_at_window_start (ruling F6)",
    "time_miss_rate_denominator": "due_in_window (design brief; F6 does not apply the same "
                                  "way to time cues, which have no inbound-text cue concept)",
    "judge_failure_rate_excludes": "write_err (reported separately as a count)",
}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log", default=os.path.join(HOME, ".human/logs/service-loop-error.log"))
    ap.add_argument("--memory-db", default=os.path.join(HOME, ".human/memory.db"))
    ap.add_argument("--since", required=True, help="YYYY-MM-DD (UTC midnight), inclusive")
    ap.add_argument("--until", required=True, help="YYYY-MM-DD (UTC midnight), exclusive")
    ap.add_argument("--out-dir", default=os.path.join(HOME, ".human/logs"))
    a = ap.parse_args(argv)

    since, until = parse_day(a.since), parse_day(a.until)
    if until <= since:
        return refuse("--until must be after --since")
    if not os.path.isfile(a.log):
        return refuse(f"no log at {a.log}")
    if not os.path.isfile(a.memory_db):
        return refuse(f"no database at {a.memory_db}")

    rows, malformed = read_log(a.log, since, until)
    if malformed:
        return refuse(f"{malformed} malformed shadow log line(s) in the window")
    if not rows:
        return refuse("no prospective shadow line in the window (is HU_PROSPECTIVE=shadow?)")

    con = sqlite3.connect(f"file:{a.memory_db}?mode=ro", uri=True)
    try:
        cols = {r[1] for r in con.execute("PRAGMA table_info(prospective_memories)")}
        if not {"cue_kind", "due_at", "status"} <= cols:
            return refuse("prospective_memories is not migrated")
        counts = build(rows, con, since, until)
    except sqlite3.Error as e:
        return refuse(f"cannot read the database ({e.__class__.__name__})")
    finally:
        con.close()

    payload = {"schema_version": 1, "since": a.since, "until": a.until, "counts": counts,
               "rates": rates_of(counts), "notes": NOTES}
    os.makedirs(a.out_dir, exist_ok=True)
    path = os.path.join(a.out_dir, f"prospective-shadow-{a.until}.json")
    cn.write_jsonl_private(path, [payload])
    print(json.dumps(payload["rates"]))
    print(f"wrote {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
