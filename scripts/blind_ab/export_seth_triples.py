#!/usr/bin/env python3
"""Export real {context, seth_reply} pairs from the macOS iMessage DB for the
blind A/B (US-4). Decodes attributedBody (modern macOS stores message text
there, not in `text`).

This is step 1 of the blind A/B pipeline:
  1. export_seth_triples.py   -> contexts + your REAL replies  (THIS SCRIPT)
  2. gen_huuman_replies.py     -> h-uman's reply for each context
  3. make_rating_sheet.py      -> blind 2AFC sheet (real vs h-uman)
  4. score.py                  -> detection rate + verdict

Privacy: runs 100% locally, writes one JSON file on your disk. Contact handles
are aliased by default (contact_1, contact_2, ...) so phone numbers/emails are
never written to the rating sheet raters see. Nothing is sent anywhere.

Usage:
  python3 export_seth_triples.py                      # 150 pairs -> seth_triples.json
  python3 export_seth_triples.py --limit 200 --min-len 12
  python3 export_seth_triples.py --keep-handles       # write real handles (NOT recommended for raters)
  python3 export_seth_triples.py --out /tmp/triples.json

If the DB is locked or permission-denied: grant your terminal Full Disk Access
(System Settings > Privacy & Security > Full Disk Access), or copy the DB first:
  cp ~/Library/Messages/chat.db /tmp/chat.db && python3 export_seth_triples.py --db /tmp/chat.db
"""
import argparse
import json
import os
import sqlite3
import sys

DEFAULT_DB = os.path.expanduser("~/Library/Messages/chat.db")


def decode_attributed_body(blob):
    """Best-effort extraction of the message string from a streamtyped
    NSAttributedString archive. Works for the common case; returns None when it
    can't confidently extract (caller skips those rows and reports the count)."""
    if not blob:
        return None
    try:
        data = bytes(blob)
        # Trim trailing attribute metadata that follows the string payload.
        if b"NSAttributedString" in data and b"NSString" not in data:
            data = data.split(b"NSAttributedString", 1)[1]
        elif b"NSString" in data:
            data = data.split(b"NSString", 1)[1]
        else:
            return None
        # Skip class-version bytes after the marker.
        data = data[5:]
        if not data:
            return None
        # Length prefix: 0x81 -> uint16 LE follows; otherwise a single-byte len.
        if data[0] == 0x81:
            if len(data) < 3:
                return None
            length = int.from_bytes(data[1:3], "little")
            data = data[3:]
        else:
            length = data[0]
            data = data[1:]
        if length <= 0 or length > len(data):
            # Length didn't validate — fall back to a conservative UTF-8 run.
            return None
        text = data[:length].decode("utf-8", errors="replace").strip()
        return text or None
    except Exception:
        return None


def msg_text(text, body):
    if text and text.strip():
        return text.strip()
    return decode_attributed_body(body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=DEFAULT_DB)
    ap.add_argument("--limit", type=int, default=150, help="max triples to emit")
    ap.add_argument("--min-len", type=int, default=8, help="skip replies shorter than this")
    ap.add_argument("--max-per-contact", type=int, default=25,
                    help="cap pairs per contact so one thread can't dominate (0 = no cap)")
    ap.add_argument("--out", default="seth_triples.json")
    ap.add_argument("--keep-handles", action="store_true",
                    help="write real contact handles instead of aliases (NOT for raters)")
    a = ap.parse_args()

    if not os.path.exists(a.db):
        sys.exit(f"DB not found: {a.db}")
    try:
        con = sqlite3.connect(f"file:{a.db}?mode=ro", uri=True)
    except sqlite3.Error as e:
        sys.exit(f"cannot open DB (try Full Disk Access or copy the db): {e}")

    # All non-tapback messages, oldest->newest, with their chat + sender.
    rows = con.execute(
        """
        SELECT cmj.chat_id, m.is_from_me, m.text, m.attributedBody, h.id, m.date
        FROM message m
        JOIN chat_message_join cmj ON cmj.message_id = m.ROWID
        LEFT JOIN handle h ON h.ROWID = m.handle_id
        WHERE m.associated_message_type = 0 AND m.item_type = 0
        ORDER BY cmj.chat_id, m.date ASC
        """
    ).fetchall()

    by_chat = {}           # chat_id -> [ {context, seth_reply, handle} ] in time order
    seen_replies = set()
    last_inbound = {}      # chat_id -> (text, handle)
    decoded_fail = 0
    sent_seen = 0

    for chat_id, is_from_me, text, body, handle, _date in rows:
        content = msg_text(text, body)
        if is_from_me == 0:
            if content:
                last_inbound[chat_id] = (content, handle or "unknown")
            elif body is not None:
                decoded_fail += 1
            continue
        # is_from_me == 1 (a reply from Seth)
        sent_seen += 1
        if not content:
            if body is not None:
                decoded_fail += 1
            continue
        if len(content) < a.min_len:
            continue
        ctx = last_inbound.get(chat_id)
        if not ctx:
            continue  # need an inbound message to reply to
        context_text, ctx_handle = ctx
        key = (context_text, content)
        if key in seen_replies:
            continue
        seen_replies.add(key)
        by_chat.setdefault(chat_id, []).append(
            {"context": context_text, "seth_reply": content, "handle": ctx_handle})

    con.close()

    # Interleave round-robin across contacts for diversity; cap per contact so
    # one big thread can't dominate the sheet.
    cap = a.max_per_contact if a.max_per_contact and a.max_per_contact > 0 else None
    buckets = [(v[:cap] if cap else v) for v in by_chat.values()]
    alias = {}
    triples = []
    depth = 0
    while len(triples) < a.limit and any(depth < len(b) for b in buckets):
        for b in buckets:
            if depth < len(b) and len(triples) < a.limit:
                item = b[depth]
                cname = item["handle"] if a.keep_handles \
                    else alias.setdefault(item["handle"], f"contact_{len(alias) + 1}")
                triples.append({
                    "contact_name": cname,
                    "context": item["context"],
                    "seth_reply": item["seth_reply"],
                })
        depth += 1
    with open(a.out, "w") as f:
        json.dump(triples, f, indent=2, ensure_ascii=False)

    # Counts only — never prints message bodies.
    print(f"wrote {len(triples)} triples -> {a.out}")
    print(f"  sent messages scanned: {sent_seen}")
    print(f"  distinct contacts: {len(alias) if not a.keep_handles else 'n/a (handles kept)'}")
    print(f"  attributedBody rows that failed to decode (skipped): {decoded_fail}")
    if len(triples) < 100:
        print("  NOTE: <100 triples. Lower --min-len, or your history is short. "
              "Aim for 100-200 for a meaningful blind A/B.")
    print("\nNext: python3 gen_huuman_replies.py (generate h-uman replies for these contexts), "
          "then make_rating_sheet.py, then score.py. See PROTOCOL.md.")


if __name__ == "__main__":
    main()
