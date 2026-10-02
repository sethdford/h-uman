#!/usr/bin/env python3
"""Reply delivery rate per chat service (iMessage / SMS / RCS) — counts only.

Why (2026-09-26 incident): a 99-char reply to an RCS contact was generated,
both send paths failed, and nothing anywhere said so. From her side the twin
ignored her. "Generated" and "delivered" are different events, and only
chat.db knows the second one.

For every reply the daemon GENERATED (an `assistant` row in memory.db
`messages`, keyed by the contact's handle) in the last N days, look for an
`is_from_me` row to that handle in chat.db from 60 s before it (the row is
saved around the send, not strictly before it) to WINDOW seconds after it.
Bucket by the service of the contact's latest inbound message before the
reply (the chat the reply should have gone back to).

Reads both files READ-ONLY (sqlite `mode=ro`). Prints counts per service —
no message text, no handles, no hashes of handles.

Caveat: the owner sends from the same account, so an is_from_me row inside
the window may be his own message, which makes `delivered` an UPPER bound.
Undelivered is therefore a LOWER bound on lost replies.

Usage:
  scripts/measure_reply_delivery_by_service.py [--days 30] [--window 300]
      [--chat-db PATH] [--memory-db PATH] [--json]
"""

import argparse
import json
import os
import sqlite3
import sys
from collections import defaultdict
from datetime import datetime, timezone

APPLE_EPOCH = 978307200  # 2001-01-01 in unix seconds


def ro(path):
    return sqlite3.connect(f"file:{path}?mode=ro", uri=True)


def to_unix(created_at):
    """memory.db `created_at` is SQLite datetime('now') — UTC, no zone."""
    dt = datetime.strptime(created_at, "%Y-%m-%d %H:%M:%S").replace(tzinfo=timezone.utc)
    return int(dt.timestamp())


def normalize_service(s):
    if not s:
        return "unknown"
    low = s.lower()
    if low == "imessage":
        return "iMessage"
    if low == "sms":
        return "SMS"
    if low == "rcs":
        return "RCS"
    return "other"


def measure(chat_db, memory_db, days, window):
    mem = ro(memory_db)
    since = mem.execute("SELECT datetime('now', ?)", (f"-{int(days)} days",)).fetchone()[0]
    replies = mem.execute(
        "SELECT session_id, created_at FROM messages "
        "WHERE role = 'assistant' AND created_at >= ? ORDER BY id",
        (since,),
    ).fetchall()
    mem.close()

    chat = ro(chat_db)
    # One statement each, bound per reply; handle ids are matched exactly.
    inbound_sql = (
        "SELECT m.service FROM message m JOIN handle h ON h.ROWID = m.handle_id "
        "WHERE h.id = ? AND m.is_from_me = 0 AND m.date <= ? "
        "ORDER BY m.date DESC LIMIT 1"
    )
    # is_from_me rows land on the chat's handle; a 1:1 send carries it in
    # message.handle_id. Join through chat_handle_join too, for rows whose
    # handle_id is 0 (seen on some outbound SMS/RCS rows).
    outbound_sql = (
        "SELECT COUNT(*) FROM message m "
        "WHERE m.is_from_me = 1 AND m.associated_message_type = 0 "
        "  AND m.date > ? AND m.date <= ? "
        "  AND (m.handle_id IN (SELECT ROWID FROM handle WHERE id = ?) "
        "       OR m.ROWID IN (SELECT cmj.message_id FROM chat_message_join cmj "
        "                      JOIN chat_handle_join chj ON chj.chat_id = cmj.chat_id "
        "                      JOIN handle h ON h.ROWID = chj.handle_id "
        "                      JOIN chat c ON c.ROWID = cmj.chat_id "
        "                      WHERE h.id = ? AND c.style = 45))"
    )

    stats = defaultdict(lambda: {"generated": 0, "delivered": 0})
    skipped_non_handle = 0
    for session_id, created_at in replies:
        handle = session_id or ""
        if not (handle.startswith("+") or "@" in handle):
            skipped_non_handle += 1  # group / non-iMessage session
            continue
        t = to_unix(created_at)
        lo = (t - APPLE_EPOCH) * 1_000_000_000
        hi = (t + window - APPLE_EPOCH) * 1_000_000_000
        row = chat.execute(inbound_sql, (handle, lo)).fetchone()
        svc = normalize_service(row[0] if row else None)
        n = chat.execute(outbound_sql, (lo - 60 * 1_000_000_000, hi, handle, handle)).fetchone()[0]
        stats[svc]["generated"] += 1
        if n > 0:
            stats[svc]["delivered"] += 1
    chat.close()

    out = {
        "days": days,
        "window_s": window,
        "skipped_non_handle_sessions": skipped_non_handle,
        "by_service": {},
    }
    for svc in sorted(stats):
        g = stats[svc]["generated"]
        d = stats[svc]["delivered"]
        out["by_service"][svc] = {
            "generated": g,
            "delivered_within_window": d,
            "undelivered": g - d,
            "delivery_rate": round(d / g, 3) if g else None,
        }
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--days", type=int, default=30)
    ap.add_argument("--window", type=int, default=300, help="seconds after generation")
    ap.add_argument("--chat-db", default=os.path.expanduser("~/Library/Messages/chat.db"))
    ap.add_argument("--memory-db", default=os.path.expanduser("~/.human/memory.db"))
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    for p in (a.chat_db, a.memory_db):
        if not os.path.exists(p):
            print(f"missing: {p}", file=sys.stderr)
            return 2
    out = measure(a.chat_db, a.memory_db, a.days, a.window)
    if a.json:
        print(json.dumps(out, indent=2))
        return 0
    print(f"reply delivery, last {a.days}d, window {a.window}s (delivered is an upper bound)")
    print(f"{'service':<10}{'generated':>10}{'delivered':>11}{'lost':>6}{'rate':>7}")
    for svc, s in out["by_service"].items():
        rate = "-" if s["delivery_rate"] is None else f"{s['delivery_rate']:.2f}"
        print(
            f"{svc:<10}{s['generated']:>10}{s['delivered_within_window']:>11}"
            f"{s['undelivered']:>6}{rate:>7}"
        )
    print(f"skipped non-handle sessions: {out['skipped_non_handle_sessions']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
