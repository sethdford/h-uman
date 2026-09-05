#!/usr/bin/env python3
"""Contract C5, Part B — When2Speak MIR/FIR measurement.

Measures how well h-uman's proactive "should I speak now?" policy tracks
Seth's own behavior, using two rates borrowed from the When2Speak /
turn-taking literature:

  MIR (Missed-Intervention Rate) — of the moments chat.db shows Seth
    himself judged worth engaging with (he replied to an inbound DM within
    REPLY_WINDOW_HOURS), what fraction did the daemon's proactive policy
    DECLINE or never propose reaching out for, in the surrounding window?
    High MIR = the policy is too passive relative to Seth's own instincts.

  FIR (False-Interruption Rate) — of the daemon's actual SEND decisions,
    what fraction were (a) genuinely proactive (Seth had not already
    replied to that contact shortly before) and (b) landed with no
    engagement (the contact did not reply within FIR_WINDOW_HOURS)?
    High FIR = the policy interrupts people who don't want to hear from it
    right now.

Everything runs locally against two SQLite files already on this machine:

  ~/Library/Messages/chat.db   (read-only; NEVER copied or written to —
                                 this is the ground-truth "what actually
                                 happened between Seth and his contacts")
  ~/.human/memory.db            (the daemon's own decision/outcome log)

Decision source, in priority order:
  1. `proactive_decisions` (contract C5 Part A) once it has rows for the
     window under measurement.
  2. Fallback: synthesize "send" decisions from `production_outcomes` +
     `proactive_sends` (both pre-date Part A and only ever recorded
     SENDS, never declines — see FALLBACK NOTE below).

Nothing computed here leaves the machine — this script only reads local
sqlite files and writes one JSON summary to ~/.human/logs/. No message
content is included in the output; only counts, timestamps, and a
truncated one-way hash of each contact identifier.

Refuses (exit 2, writes nothing) when either measurement's denominator is
below --min-n (default 30), per
~/.claude/rules/no-number-without-a-measurement.md — a rate computed from
too few examples is not a rate, it's noise wearing a rate's clothes.
"""
import argparse
import hashlib
import json
import os
import sqlite3
import sys
import time

APPLE_EPOCH = 978307200  # 2001-01-01 00:00:00 UTC, chat.db's `date` epoch

FIR_WINDOW_HOURS = 24.0  # contact-replied-within window for FIR; also the comparison
                          # window for scripts/eval_seth_initiation_baseline.py's rate.


def apple_ns_to_unix(apple_ns):
    if not apple_ns:
        return 0
    return APPLE_EPOCH + (apple_ns / 1_000_000_000.0)


def contact_ref(contact):
    """One-way, truncated reference for a contact identifier — enough to
    group rows by contact in the output JSON without writing a raw phone
    number/email into a file. Not used for anything security-sensitive."""
    if not contact:
        return None
    return hashlib.sha256(contact.encode("utf-8", "replace")).hexdigest()[:12]


def open_ro(path):
    if not os.path.exists(path):
        return None
    return sqlite3.connect(f"file:{path}?mode=ro", uri=True)


# ── chat.db extraction ──────────────────────────────────────────────────


def load_dm_messages(chat_db, since_unix):
    """Returns list of (chat_id, contact, ts_unix, is_from_me) for every
    message in a 1:1 (DM) chat since `since_unix`. Group chats (chats with
    more than one non-Seth participant) are excluded — When2Speak concerns
    one-on-one proactive outreach, and a group's "reply" isn't attributable
    to a single relationship the way a DM's is."""
    cur = chat_db.cursor()

    # DM chats: exactly one handle joined to the chat.
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


def label_positives(messages, reply_window_secs):
    """For every inbound (is_from_me=False) message, label positive=True
    iff there is a later outbound message in the SAME chat within
    reply_window_secs. Returns list of dicts:
      {chat_id, contact, ts, positive}
    """
    by_chat = {}
    for chat_id, contact, ts, is_from_me in messages:
        by_chat.setdefault(chat_id, []).append((ts, is_from_me, contact))

    positives = []
    for chat_id, rows in by_chat.items():
        rows.sort(key=lambda r: r[0])
        outbound_ts = [ts for ts, ifm, _ in rows if ifm]
        for i, (ts, is_from_me, contact) in enumerate(rows):
            if is_from_me:
                continue
            # First outbound strictly after this inbound message, within window.
            replied = any(ts < o_ts <= ts + reply_window_secs for o_ts in outbound_ts)
            positives.append(
                {"chat_id": chat_id, "contact": contact, "ts": ts, "positive": replied}
            )
    return positives


def seth_initiated_sends(messages):
    """Outbound (Seth) messages per chat, sorted — used for the FIR
    "Seth had not already replied" exclusion."""
    by_chat = {}
    for chat_id, contact, ts, is_from_me in messages:
        if is_from_me:
            by_chat.setdefault(chat_id, []).append(ts)
    for v in by_chat.values():
        v.sort()
    return by_chat


def contact_to_chat(messages):
    """contact -> chat_id, for joining decision rows (keyed by contact
    identifier, e.g. a phone number) back to chat.db chat ids."""
    m = {}
    for chat_id, contact, _ts, _ifm in messages:
        m.setdefault(contact, chat_id)
    return m


# ── decision-log extraction (~/.human/memory.db) ────────────────────────


def load_decisions(memory_db, since_unix):
    """Returns (rows, source) where rows is a list of
      {ts, contact, decision, sent}
    and source is 'proactive_decisions' or 'fallback'."""
    cur = memory_db.cursor()
    cur.execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name='proactive_decisions'"
    )
    if cur.fetchone():
        cur.execute(
            "SELECT ts, contact, decision, sent FROM proactive_decisions "
            "WHERE ts >= ? AND contact IS NOT NULL",
            (since_unix,),
        )
        rows = [
            {"ts": ts, "contact": contact, "decision": decision, "sent": bool(sent)}
            for ts, contact, decision, sent in cur.fetchall()
        ]
        if rows:
            return rows, "proactive_decisions"
        # Table exists but is empty for this window — fall through to the
        # fallback so a freshly-deployed C5 Part A doesn't silently
        # produce a "no decisions" (n=0) result before it's had time to
        # accumulate rows.

    # FALLBACK NOTE: production_outcomes and proactive_sends predate
    # contract C5 and only ever recorded rows for ACTUAL sends — there is
    # no decline/defer history in them. Every row here therefore has
    # decision='send'. This makes the fallback's MIR structurally biased
    # toward "missed" for any positive moment with no nearby send (which
    # is expected — it is exactly what MIR is supposed to catch), but it
    # cannot distinguish "the policy considered and declined" from "the
    # policy never ran a tick for this contact at all". Once
    # proactive_decisions has real rows this distinction becomes visible.
    rows = []
    cur.execute("SELECT name FROM sqlite_master WHERE type='table' AND name='proactive_sends'")
    if cur.fetchone():
        cur.execute(
            "SELECT sent_timestamp, contact FROM proactive_sends WHERE sent_timestamp >= ?",
            (since_unix,),
        )
        rows += [{"ts": ts, "contact": contact, "decision": "send", "sent": True} for ts, contact in cur.fetchall()]
    cur.execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name='production_outcomes'"
    )
    if cur.fetchone():
        cur.execute(
            "SELECT send_timestamp, target FROM production_outcomes WHERE send_timestamp >= ?",
            (since_unix,),
        )
        rows += [{"ts": ts, "contact": target, "decision": "send", "sent": True} for ts, target in cur.fetchall()]
    return rows, "fallback"


# ── MIR / FIR computation ───────────────────────────────────────────────


def compute_mir(positives, decisions_by_contact, before_secs, after_secs):
    total = 0
    missed = 0
    for p in positives:
        if not p["positive"]:
            continue
        total += 1
        window = decisions_by_contact.get(p["contact"], [])
        nearby = [d for d in window if p["ts"] - before_secs <= d["ts"] <= p["ts"] + after_secs]
        has_send = any(d["decision"] == "send" for d in nearby)
        if not has_send:
            missed += 1
    return {"n": total, "missed": missed, "rate": (missed / total) if total else None}


def compute_fir(decisions, seth_sends_by_chat, contact_chat_map, contact_replies_by_contact,
                seth_before_secs, contact_after_secs):
    eligible = 0
    false_interruptions = 0
    for d in decisions:
        if d["decision"] != "send":
            continue
        chat_id = contact_chat_map.get(d["contact"])
        seth_ts_list = seth_sends_by_chat.get(chat_id, []) if chat_id is not None else []
        # Exclusion: Seth already sent something to this contact shortly
        # before the daemon's send — this wasn't a case of Seth-silence.
        seth_already_engaged = any(
            d["ts"] - seth_before_secs <= s_ts < d["ts"] for s_ts in seth_ts_list
        )
        if seth_already_engaged:
            continue
        eligible += 1
        reply_ts_list = contact_replies_by_contact.get(d["contact"], [])
        got_reply = any(d["ts"] < r_ts <= d["ts"] + contact_after_secs for r_ts in reply_ts_list)
        if not got_reply:
            false_interruptions += 1
    return {
        "n": eligible,
        "false_interruptions": false_interruptions,
        "rate": (false_interruptions / eligible) if eligible else None,
    }


def contact_inbound_ts(messages):
    """contact -> sorted list of inbound (is_from_me=False) timestamps —
    used by compute_fir to check whether the contact replied after a
    daemon send."""
    m = {}
    for chat_id, contact, ts, is_from_me in messages:
        if not is_from_me:
            m.setdefault(contact, []).append(ts)
    for v in m.values():
        v.sort()
    return m


def decisions_by_contact_index(decisions):
    m = {}
    for d in decisions:
        m.setdefault(d["contact"], []).append(d)
    return m


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--days", type=int, default=90, help="lookback window (default 90)")
    ap.add_argument("--reply-window-hours", type=float, default=6.0,
                    help="Seth-replied-within window for the 'positive' label (default 6h)")
    ap.add_argument("--fir-window-hours", type=float, default=FIR_WINDOW_HOURS,
                    help="contact-replied-within window for FIR (default 24h)")
    ap.add_argument("--decision-window-before-hours", type=float, default=1.0,
                    help="how far BEFORE a positive moment to look for a decision (default 1h)")
    ap.add_argument("--decision-window-after-hours", type=float, default=24.0,
                    help="how far AFTER a positive moment to look for a decision (default 24h)")
    ap.add_argument("--seth-already-engaged-hours", type=float, default=6.0,
                    help="FIR exclusion window: Seth sent something in this window before "
                         "the daemon's send (default 6h)")
    ap.add_argument("--min-n", type=int, default=30, help="refuse below this denominator (default 30)")
    ap.add_argument("--chat-db", default=os.path.expanduser("~/Library/Messages/chat.db"))
    ap.add_argument("--memory-db", default=os.path.expanduser("~/.human/memory.db"))
    ap.add_argument("--out-dir", default=os.path.expanduser("~/.human/logs"))
    args = ap.parse_args()

    now = time.time()
    since = now - args.days * 86400.0

    chat_db = open_ro(args.chat_db)
    if chat_db is None:
        print(f"REFUSE: chat.db not found at {args.chat_db}", file=sys.stderr)
        return 2
    memory_db = open_ro(args.memory_db)
    if memory_db is None:
        print(f"REFUSE: memory.db not found at {args.memory_db}", file=sys.stderr)
        return 2

    messages = load_dm_messages(chat_db, since)
    if not messages:
        print("REFUSE: no DM messages found in the lookback window", file=sys.stderr)
        return 2

    positives = label_positives(messages, args.reply_window_hours * 3600.0)
    seth_sends_by_chat = seth_initiated_sends(messages)
    contact_chat_map = contact_to_chat(messages)
    contact_replies = contact_inbound_ts(messages)

    decisions, decisions_source = load_decisions(memory_db, since)
    decisions_by_contact = decisions_by_contact_index(decisions)

    mir = compute_mir(
        positives,
        decisions_by_contact,
        before_secs=args.decision_window_before_hours * 3600.0,
        after_secs=args.decision_window_after_hours * 3600.0,
    )
    fir = compute_fir(
        decisions,
        seth_sends_by_chat,
        contact_chat_map,
        contact_replies,
        seth_before_secs=args.seth_already_engaged_hours * 3600.0,
        contact_after_secs=args.fir_window_hours * 3600.0,
    )

    if mir["n"] < args.min_n or fir["n"] < args.min_n:
        print(
            f"REFUSE: insufficient n (MIR n={mir['n']}, FIR n={fir['n']}, min_n={args.min_n}) "
            "— not writing a result.",
            file=sys.stderr,
        )
        return 2

    total_positive_examples = sum(1 for p in positives if p["positive"])
    result = {
        "generated_at": int(now),
        "days": args.days,
        "decisions_source": decisions_source,
        "windows": {
            "reply_window_hours": args.reply_window_hours,
            "fir_window_hours": args.fir_window_hours,
            "decision_window_before_hours": args.decision_window_before_hours,
            "decision_window_after_hours": args.decision_window_after_hours,
            "seth_already_engaged_hours": args.seth_already_engaged_hours,
        },
        "inputs": {
            "dm_messages": len(messages),
            "inbound_messages": len(positives),
            "positive_moments": total_positive_examples,
            "decisions_loaded": len(decisions),
        },
        "mir": mir,
        "fir": fir,
    }

    os.makedirs(args.out_dir, exist_ok=True)
    date_str = time.strftime("%Y-%m-%d", time.localtime(now))
    out_path = os.path.join(args.out_dir, f"when-to-speak-{date_str}.json")
    with open(out_path, "w") as f:
        json.dump(result, f, indent=2)

    print(f"wrote {out_path}")
    print(f"MIR: {mir['rate']:.3f} (missed {mir['missed']}/{mir['n']})" if mir["rate"] is not None else "MIR: n/a")
    print(
        f"FIR: {fir['rate']:.3f} (false_interruptions {fir['false_interruptions']}/{fir['n']})"
        if fir["rate"] is not None
        else "FIR: n/a"
    )
    print(f"decisions_source={decisions_source}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
