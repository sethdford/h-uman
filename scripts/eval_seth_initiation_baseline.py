#!/usr/bin/env python3
"""US-3 — Seth's own initiation-response baseline from chat.db.

Measures how often Seth's own conversation-opening sends go unanswered,
using the same "unanswered within a window" definition as
scripts/eval_when_to_speak.py's FIR (False-Interruption Rate), so the
daemon's FIR has a real human reference point instead of an arbitrary
threshold.

Definitions (see sprints/sprint-better-than-human-2026-09-05/designs/US-3.md
for the full write-up and citations):

  Initiation — an outbound (Seth) message in a DM chat that is either the
    first message ever recorded for that chat, or is preceded (in that
    same chat_id) by a gap of at least INITIATION_GAP_HOURS with no
    message from either party.

  Unanswered / false-initiation — an initiation with no inbound message
    in the same chat_id within FIR_WINDOW_HOURS (imported from
    eval_when_to_speak.py, so this rate is comparable to the daemon's own
    FIR on the same window definition — this is the entire point of
    AC-3.1).

Tapback reactions (chat.db rows with a non-zero associated_message_type,
e.g. 2000-2006 for heart/like/etc. reacts) are excluded from BOTH sides of
the measurement by load_dm_messages_excluding_tapbacks() (Finding F1): a
bare react is not a reply, and an outbound react is not an initiation.

Everything runs locally against ~/Library/Messages/chat.db, opened
read-only (mode=ro&immutable=1, via this module's own open_ro() — see
that function's docstring for why it is not eval_when_to_speak.py's
open_ro(), which lacks immutable=1). This script NEVER writes to
chat.db — see open_ro() below and the static no-write-statement test in
scripts/test_eval_seth_initiation_baseline.py.

Only chat_id and contact identifiers are used, and only as in-memory
grouping keys — no message text, phone number, or contact name ever
reaches compute_baseline()'s return value or anything written to disk or
printed to stdout. Output is counts, a rate, a 95% Wilson CI, and a date
range only (AC-3.5).

Refuses (exit non-zero, writes nothing) when:
  - chat.db is not found at --chat-db
  - zero DM messages are found in the lookback window
  - n (initiations) < --min-n (default 30) — a rate computed from too few
    examples is not a rate; see
    ~/.claude/rules/no-number-without-a-measurement.md. Per Finding 2 in
    the design doc, the 30-day-ish retention floor is a moving target, so
    this refusal is a real, honestly-recorded possible outcome on a
    future run — not just a test fixture (AC-3.3).
"""
import argparse
import json
import os
import sqlite3
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "blind_ab"))
from eval_when_to_speak import FIR_WINDOW_HOURS, APPLE_EPOCH, apple_ns_to_unix  # noqa: E402
from score import wilson  # noqa: E402


def open_ro(path):
    """Read-only, immutable connection to chat.db: mode=ro&immutable=1,
    matching eval_persona_evolution.py's convention (AC-3.2 / the HARD
    NO-GO for this story: chat.db is read-only). Deliberately NOT
    eval_when_to_speak.py's open_ro() — that helper only sets `mode=ro`
    (verified: `grep -n immutable scripts/eval_when_to_speak.py` has no
    hits) and omits `immutable=1`. FIR_WINDOW_HOURS, APPLE_EPOCH, and
    apple_ns_to_unix are still reused unmodified from eval_when_to_speak.py
    (see the design doc's reuse precedent); only the connection helper and
    the DM-message loader (see load_dm_messages_excluding_tapbacks below)
    are re-declared here — the former to satisfy AC-3.2's stronger
    requirement, the latter for Finding F1 (tapback exclusion, see that
    function's docstring)."""
    if not os.path.exists(path):
        return None
    return sqlite3.connect(f"file:{path}?mode=ro&immutable=1", uri=True)


def load_dm_messages_excluding_tapbacks(chat_db, since_unix):
    """Same contract as eval_when_to_speak.load_dm_messages(): returns
    [(chat_id, contact, ts_unix, is_from_me), ...] for every message in a
    1:1 (DM) chat since `since_unix`, with group chats excluded via the
    identical `HAVING COUNT(DISTINCT handle_id) = 1` rule.

    Additionally excludes tapback/reaction rows (Finding F1): chat.db
    represents a tapback (heart/like/etc. react to another message) as an
    ordinary `message` row whose `associated_message_type` is one of
    2000-2006 rather than 0/NULL. Neither find_initiations() nor
    label_unanswered() should treat a bare react as a real conversational
    turn — a heart-react on an inbound message is not a "reply", and an
    outbound heart-react is not an "initiation". Measured live against
    this machine's chat.db (2026-09-05): non-zero associated_message_type
    accounts for 3.8% of in-window DM rows (90 of 2413), concentrated at
    2000 (tapback added) with a handful of 2001/2003-2006 (tapback
    variants/removals).

    Deliberately duplicates (rather than imports) load_dm_messages()'s
    DM-chat-detection query instead of modifying eval_when_to_speak.py —
    that script's load_dm_messages() is shared with US-4
    (scripts/fit_reply_delay_model.py also imports it unmodified) and this
    story's fix must not change its behavior for other callers. See
    Finding F1 in designs/US-3.md."""
    cur = chat_db.cursor()

    # DM chats: exactly one handle joined to the chat. Identical to
    # eval_when_to_speak.load_dm_messages()'s own query.
    cur.execute(
        """
        SELECT chj.chat_id, MIN(h.id) AS contact
        FROM chat_handle_join chj
        JOIN handle h ON h.ROWID = chj.handle_id
        GROUP BY chj.chat_id
        HAVING COUNT(DISTINCT chj.handle_id) = 1
        """
    )
    dm_chat_contact = {row[0]: row[1] for row in cur.fetchall()}
    if not dm_chat_contact:
        return []

    since_apple_ns = (since_unix - APPLE_EPOCH) * 1_000_000_000
    cur.execute(
        """
        SELECT cmj.chat_id, m.date, m.is_from_me
        FROM message m
        JOIN chat_message_join cmj ON cmj.message_id = m.ROWID
        WHERE m.date >= ?
          AND m.is_system_message = 0
          AND m.item_type = 0
          AND COALESCE(m.associated_message_type, 0) = 0
        ORDER BY m.date ASC
        """,
        (since_apple_ns,),
    )
    out = []
    for chat_id, date_ns, is_from_me in cur.fetchall():
        contact = dm_chat_contact.get(chat_id)
        if not contact:
            continue  # not a DM chat
        ts = apple_ns_to_unix(date_ns)
        if ts <= 0:
            continue
        out.append((chat_id, contact, ts, bool(is_from_me)))
    return out


# Mirrors the *value* (not a shared name) of eval_when_to_speak.py's own
# --seth-already-engaged-hours default (see eval_when_to_speak.py:307-309:
# "FIR exclusion window: Seth sent something in this window before the
# daemon's send"). That flag encodes the same underlying judgment this
# script needs -- "a gap this short means still the same conversation, not
# a fresh opener" -- just applied from the daemon's side. Not promoted to
# a shared module-level constant: it is a CLI default inside
# eval_when_to_speak.py's main(), not a module-level name, and the only
# cross-file coupling AC-3.1 asks for is FIR_WINDOW_HOURS above.
INITIATION_GAP_HOURS = 6.0


def find_initiations(messages, gap_hours=INITIATION_GAP_HOURS):
    """messages: [(chat_id, contact, ts, is_from_me), ...] from
    load_dm_messages_excluding_tapbacks(). Returns [{"chat_id": ..., "ts": ...}, ...] for
    every outbound message that is either the first row in its chat_id or
    preceded (by either party) by a gap >= gap_hours. Grouped and sorted
    per chat_id internally; chat_id/contact never leave this module's
    output (see compute_baseline)."""
    gap_secs = gap_hours * 3600.0
    by_chat = {}
    for chat_id, contact, ts, is_from_me in messages:
        by_chat.setdefault(chat_id, []).append((ts, is_from_me))

    initiations = []
    for chat_id, rows in by_chat.items():
        rows.sort(key=lambda r: r[0])
        prev_ts = None
        for ts, is_from_me in rows:
            if is_from_me:
                if prev_ts is None or (ts - prev_ts) >= gap_secs:
                    initiations.append({"chat_id": chat_id, "ts": ts})
            prev_ts = ts
    return initiations


def label_unanswered(initiations, messages, window_hours=FIR_WINDOW_HOURS):
    """Adds "unanswered": bool to each initiation dict — True iff no
    inbound (is_from_me=0) row in the same chat_id arrives within
    window_hours after ts. Returns a new list; does not mutate the input."""
    window_secs = window_hours * 3600.0
    inbound_by_chat = {}
    for chat_id, contact, ts, is_from_me in messages:
        if not is_from_me:
            inbound_by_chat.setdefault(chat_id, []).append(ts)
    for v in inbound_by_chat.values():
        v.sort()

    labeled = []
    for init in initiations:
        inbound_ts_list = inbound_by_chat.get(init["chat_id"], [])
        got_reply = any(
            init["ts"] < r_ts <= init["ts"] + window_secs for r_ts in inbound_ts_list
        )
        labeled.append({**init, "unanswered": not got_reply})
    return labeled


def compute_baseline(labeled):
    """Returns {"n": int, "unanswered": int, "rate": float|None,
    "wilson_ci": [lo, hi]} using wilson(unanswered, n) — None rate and
    [0.0, 0.0] CI when n==0 (the caller refuses before this is reached in
    practice per --min-n, but the function itself stays total so it can
    be unit-tested directly on degenerate input)."""
    n = len(labeled)
    unanswered = sum(1 for row in labeled if row["unanswered"])
    if n == 0:
        return {"n": 0, "unanswered": 0, "rate": None, "wilson_ci": [0.0, 0.0]}
    _p, lo, hi = wilson(unanswered, n)
    return {
        "n": n,
        "unanswered": unanswered,
        "rate": unanswered / n,
        "wilson_ci": [lo, hi],
    }


def date_range_of(timestamps):
    """ISO date strings (date precision only) for the earliest/latest of
    the given unix timestamps. Returns {"first": None, "last": None} for
    an empty input."""
    if not timestamps:
        return {"first": None, "last": None}
    return {
        "first": time.strftime("%Y-%m-%d", time.gmtime(min(timestamps))),
        "last": time.strftime("%Y-%m-%d", time.gmtime(max(timestamps))),
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--days", type=int, default=90, help="lookback window (default 90)")
    ap.add_argument("--gap-hours", type=float, default=INITIATION_GAP_HOURS,
                    help="silence gap (either party) that marks a fresh initiation (default 6h)")
    ap.add_argument("--fir-window-hours", type=float, default=FIR_WINDOW_HOURS,
                    help="contact-replied-within window for 'answered', same definition "
                         "eval_when_to_speak.py's FIR uses (default 24h)")
    ap.add_argument("--min-n", type=int, default=30, help="refuse below this denominator (default 30)")
    ap.add_argument("--chat-db", default=os.path.expanduser("~/Library/Messages/chat.db"))
    ap.add_argument("--out-dir", default=os.path.expanduser("~/.human/logs"))
    args = ap.parse_args(argv)

    now = time.time()
    since = now - args.days * 86400.0

    chat_db = open_ro(args.chat_db)
    if chat_db is None:
        print(f"REFUSE: chat.db not found at {args.chat_db}", file=sys.stderr)
        return 2

    messages = load_dm_messages_excluding_tapbacks(chat_db, since)
    if not messages:
        print("REFUSE: no DM messages found in the lookback window", file=sys.stderr)
        return 2

    initiations = find_initiations(messages, gap_hours=args.gap_hours)
    labeled = label_unanswered(messages=messages, initiations=initiations, window_hours=args.fir_window_hours)
    baseline = compute_baseline(labeled)

    if baseline["n"] < args.min_n:
        print(
            f"REFUSE: insufficient n (n={baseline['n']}, min_n={args.min_n}) "
            "— not writing a result.",
            file=sys.stderr,
        )
        return 2

    date_range = date_range_of([init["ts"] for init in initiations])

    result = {
        "generated_at": int(now),
        "days": args.days,
        "gap_hours": args.gap_hours,
        "fir_window_hours": args.fir_window_hours,
        "date_range": date_range,
        "n": baseline["n"],
        "unanswered": baseline["unanswered"],
        "rate": baseline["rate"],
        "wilson_ci": baseline["wilson_ci"],
    }

    os.makedirs(args.out_dir, exist_ok=True)
    date_str = time.strftime("%Y-%m-%d", time.localtime(now))
    out_path = os.path.join(args.out_dir, f"seth-initiation-baseline-{date_str}.json")
    with open(out_path, "w") as f:
        json.dump(result, f, indent=2)

    print(f"wrote {out_path}")
    print(
        f"rate: {baseline['rate']:.3f} (unanswered {baseline['unanswered']}/{baseline['n']}, "
        f"95% CI [{baseline['wilson_ci'][0]:.3f}, {baseline['wilson_ci'][1]:.3f}])"
    )
    print(f"date_range: {date_range['first']} .. {date_range['last']}")
    print(f"gap_hours={args.gap_hours} fir_window_hours={args.fir_window_hours}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
