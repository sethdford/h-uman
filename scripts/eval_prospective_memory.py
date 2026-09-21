#!/usr/bin/env python3
"""better-than-human criterion 3 — prospective memory, a dated reading.

The roadmap gate is "prospective-memory F1 > 0.65". Tonight it has no
reading at all: the fire-once fix deployed 2026-09-19 and `fired=1` has been
0 ever since. This script turns "no reading" into a dated one, even when the
honest reading is a refusal.

What it counts (always written — counts are measurements, not rates):
  open            fired=0 and not expired
  expired_unfired fired=0 and expires_at <= now
  fired           fired=1  (the daemon injected it and marked it)
  pruned          fired=2  (retired by insight_stream.py --prune-triggers)
  cued            open|fired|expired triggers whose contact sent a message
                  AFTER the trigger was created and after --since (the deploy
                  that made firing possible) containing trigger_value as a
                  WHOLE WORD (the daemon matches with hu_str_contains_word_ci;
                  substring would count "works" for "work")

What it rates (refused below --min-n, per no-number-without-a-measurement):
  fire_rate_on_cue  fired / cued  — of the intentions whose cue arrived, how
                    many did the daemon act on. This is recall against the
                    cue, the only ground truth the database holds.
  precision / F1    NOT computable here: whether an injected reminder was
                    *appropriate* needs a rater or a recipient reaction. Left
                    null with the reason, so the card cannot mistake a
                    missing measurement for a good one.

Reads ~/.human/memory.db only (read-only URI). Writes one JSON under
~/.human/logs/. No message content, no contact identifiers in the output.
"""
import argparse
import json
import os
import re
import sqlite3
import sys
import time
from datetime import datetime, timezone

DEFAULT_SINCE = "2026-09-19T22:53:00Z"  # PR #419 deployed: first build that sets fired=1


def open_ro(path):
    return sqlite3.connect(f"file:{path}?mode=ro", uri=True)


def parse_since(s):
    dt = datetime.strptime(s, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
    return int(dt.timestamp())


def _word_re(trigger):
    return re.compile(r"(?<![a-z0-9])" + re.escape(trigger.lower()) + r"(?![a-z0-9])")


def load_triggers(con, now):
    rows = con.execute(
        "SELECT id, trigger_value, contact_id, fired, created_at, expires_at "
        "FROM prospective_memories WHERE trigger_type='keyword'"
    ).fetchall()
    out = []
    for tid, val, contact, fired, created, expires in rows:
        expired = fired == 0 and expires is not None and expires <= now
        out.append({
            "id": tid, "trigger": (val or "").lower(), "contact": contact,
            "fired": fired, "created_at": created, "expired": expired,
        })
    return out


def inbound_by_contact(con, since_epoch):
    """{contact: [(epoch, lowercased text), ...]} for role='user' rows after since.
    messages.created_at is TEXT UTC 'YYYY-MM-DD HH:MM:SS' — never compare it
    to an integer in SQL (silently vacuous); convert here."""
    since_txt = datetime.fromtimestamp(since_epoch, timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
    rows = con.execute(
        "SELECT session_id, content, created_at FROM messages "
        "WHERE role='user' AND created_at > ?", (since_txt,)
    ).fetchall()
    by = {}
    for sid, content, created in rows:
        try:
            ep = int(datetime.strptime(created, "%Y-%m-%d %H:%M:%S")
                     .replace(tzinfo=timezone.utc).timestamp())
        except (TypeError, ValueError):
            continue
        by.setdefault(sid, []).append((ep, (content or "").lower()))
    return by


def count_cued(triggers, inbound):
    cued = 0
    for t in triggers:
        if t["fired"] == 2 or not t["trigger"]:
            continue
        rx = _word_re(t["trigger"])
        for ep, text in inbound.get(t["contact"], ()):
            if ep > (t["created_at"] or 0) and rx.search(text):
                t["cued"] = True
                cued += 1
                break
    return cued


def evaluate(memory_db, since_epoch, min_n, now=None):
    now = now or int(time.time())
    con = open_ro(memory_db)
    try:
        triggers = load_triggers(con, now)
        inbound = inbound_by_contact(con, since_epoch)
    finally:
        con.close()
    counts = {
        "open": sum(1 for t in triggers if t["fired"] == 0 and not t["expired"]),
        "expired_unfired": sum(1 for t in triggers if t["expired"]),
        "fired": sum(1 for t in triggers if t["fired"] == 1),
        "pruned": sum(1 for t in triggers if t["fired"] == 2),
        "cued": count_cued(triggers, inbound),
        "fired_and_cued": sum(1 for t in triggers if t["fired"] == 1 and t.get("cued")),
        "inbound_messages_since": sum(len(v) for v in inbound.values()),
    }
    rates = {"precision": None, "f1": None,
             "precision_reason": "needs rater/recipient ground truth; not in memory.db"}
    if counts["cued"] < min_n:
        rates["status"] = "REFUSE"
        rates["reason"] = f"insufficient n (cued={counts['cued']}, min_n={min_n})"
        rates["fire_rate_on_cue"] = None
    else:
        rates["status"] = "OK"
        rates["fire_rate_on_cue"] = counts["fired_and_cued"] / counts["cued"]
    return {
        "schema_version": 1,
        "measured_at": datetime.fromtimestamp(now, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "since": datetime.fromtimestamp(since_epoch, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "min_n": min_n,
        "counts": counts,
        "rates": rates,
        "gate": {"metric": "f1", "threshold": 0.65, "pass": None,
                 "reason": "f1 not computable without precision ground truth"},
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--memory-db", default=os.path.expanduser("~/.human/memory.db"))
    ap.add_argument("--since", default=DEFAULT_SINCE, help="ISO UTC; cues before this cannot have fired")
    ap.add_argument("--min-n", type=int, default=30)
    ap.add_argument("--out-dir", default=os.path.expanduser("~/.human/logs"))
    ap.add_argument("--no-write", action="store_true")
    args = ap.parse_args(argv)

    result = evaluate(args.memory_db, parse_since(args.since), args.min_n)
    c, r = result["counts"], result["rates"]
    print(f"prospective: open={c['open']} expired_unfired={c['expired_unfired']} "
          f"fired={c['fired']} pruned={c['pruned']} cued_since={c['cued']} "
          f"inbound_since={c['inbound_messages_since']}")
    if r["status"] == "REFUSE":
        print(f"REFUSE: {r['reason']} — counts written, rates withheld")
    else:
        print(f"fire_rate_on_cue={r['fire_rate_on_cue']:.3f} (n={c['cued']}); f1 not computable")
    if not args.no_write:
        os.makedirs(args.out_dir, exist_ok=True)
        out = os.path.join(args.out_dir, f"prospective-memory-{result['measured_at'][:10]}.json")
        with open(out, "w") as f:
            json.dump(result, f, indent=2)
        print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
