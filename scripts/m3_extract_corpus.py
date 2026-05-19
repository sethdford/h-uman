#!/usr/bin/env python3
"""
Phase H1 (2026-05-18) — multi-channel corpus extractor.

Pulls Seth-authored conversation turns from every available source,
emits a unified JSONL ready for downstream training / counterfactual
generation. Each line:

    {
      "channel": "imessage" | "gmail" | "slack" | "memory_db",
      "ts_ms": int,
      "handle": "<contact id, hashed if --redact>",
      "role": "user" | "assistant",   # "assistant" = Seth-authored
      "content": "<text, PII-redacted>"
    }

Why each source:
  - imessage (~/Library/Messages/chat.db) — the LARGEST Seth-authored
    corpus on this machine (2000+ outgoing turns). Schema: Apple's
    `message` table; `is_from_me=1` means Seth wrote it. Requires
    Full Disk Access on macOS (granted to Terminal/the binary).
  - memory_db (~/.human/memory.db) — what the DAEMON has ingested
    so far. Smaller, possibly stale, but includes channels other
    than iMessage when configured.
  - gmail — uses HUMAN_GMAIL_REFRESH_TOKEN (from ~/.human/config.json).
    Stubbed for now (network call complexity); flag stays for parity.
  - slack — uses HUMAN_SLACK_BOT_TOKEN; same stub status.

PII redaction (mandatory by default):
  - phone numbers → "[phone]"
  - email addresses → "[email]"
  - long digit strings (≥7 digits) → "[number]"
  - contact handles → SHA-256 truncated to 8 hex chars

Run:
    python3 scripts/m3_extract_corpus.py \\
        --out ~/.human/training-data/m3-corpus.jsonl \\
        --sources imessage,memory_db \\
        --max-per-source 5000

Exit:
    0 — wrote at least one record
    2 — input/permission failure (no sources reachable)
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sqlite3
import sys
import time
from pathlib import Path

HOME = Path.home()
IMSG_DB = HOME / "Library" / "Messages" / "chat.db"
MEMORY_DB = HOME / ".human" / "memory.db"
DEFAULT_OUT = HOME / ".human" / "training-data" / "m3-corpus.jsonl"


# ─────────────────────────────────────────────────────────────────────
# PII redaction — applied to every record
# ─────────────────────────────────────────────────────────────────────

PHONE_RE = re.compile(r"\+?\d[\d\s\-().]{7,}")
EMAIL_RE = re.compile(r"\b[\w.+-]+@[\w-]+\.[\w.-]+\b")
LONG_DIGITS_RE = re.compile(r"\b\d{7,}\b")
# Credit-card-ish (very loose; trips on long numbers but those go through
# LONG_DIGITS_RE anyway)
CC_RE = re.compile(r"\b(?:\d[ -]*?){13,16}\b")


def redact_pii(text: str) -> str:
    """Apply the redaction pipeline. Order matters: emails before long
    digits so the latter doesn't munch numeric local-parts."""
    if not text:
        return ""
    text = EMAIL_RE.sub("[email]", text)
    text = PHONE_RE.sub("[phone]", text)
    text = CC_RE.sub("[number]", text)
    text = LONG_DIGITS_RE.sub("[number]", text)
    return text


def hash_handle(handle: str, salt: str = "") -> str:
    """One-way hash of a contact handle. Keeps per-contact grouping
    intact across the corpus without storing the actual phone/email."""
    if not handle:
        return ""
    h = hashlib.sha256((salt + handle).encode("utf-8")).hexdigest()
    return h[:8]


# ─────────────────────────────────────────────────────────────────────
# iMessage extractor — reads ~/Library/Messages/chat.db
# ─────────────────────────────────────────────────────────────────────

# Apple stores message dates as nanoseconds since 2001-01-01 (Mac epoch).
# Convert to unix ms: (apple_ns / 1e9) + 978307200 (= seconds from
# 1970 to 2001), then * 1000.
APPLE_EPOCH_OFFSET_SEC = 978307200


def apple_ns_to_unix_ms(apple_ns: int) -> int:
    if apple_ns is None or apple_ns == 0:
        return 0
    return int((apple_ns / 1_000_000_000.0 + APPLE_EPOCH_OFFSET_SEC) * 1000)


def extract_imessage(db_path: Path, max_records: int, redact_handles: bool) -> list[dict]:
    """Read from Apple's chat.db. JOINs message → handle for the
    contact identifier. Skips rows with no text content (attachments,
    reactions, etc. live in other tables we don't pull from here)."""
    if not db_path.exists():
        return []
    out = []
    try:
        conn = sqlite3.connect(str(db_path))
        conn.text_factory = bytes  # tolerate mojibake
        cur = conn.cursor()
        cur.execute(
            "SELECT m.text, m.is_from_me, m.date, h.id "
            "FROM message m "
            "LEFT JOIN handle h ON m.handle_id = h.ROWID "
            "WHERE m.text IS NOT NULL AND length(m.text) > 0 "
            "ORDER BY m.date DESC "
            "LIMIT ?",
            (max_records,))
        for text_b, is_from_me, apple_ns, handle_b in cur.fetchall():
            text = (text_b.decode("utf-8", "replace")
                    if isinstance(text_b, bytes) else (text_b or ""))
            handle = (handle_b.decode("utf-8", "replace")
                      if isinstance(handle_b, bytes) else (handle_b or ""))
            text = text.strip()
            if not text or len(text) < 2:
                continue
            out.append({
                "channel": "imessage",
                "ts_ms": apple_ns_to_unix_ms(apple_ns),
                "handle": hash_handle(handle) if redact_handles else handle,
                "role": "assistant" if is_from_me else "user",
                "content": redact_pii(text),
            })
        conn.close()
    except sqlite3.Error as e:
        print(f"  WARN: iMessage read failed ({e}). On macOS this needs Full Disk "
              f"Access for the running shell.", file=sys.stderr)
    return out


# ─────────────────────────────────────────────────────────────────────
# memory.db extractor — the daemon's own conversation log
# ─────────────────────────────────────────────────────────────────────

def extract_memory_db(db_path: Path, max_records: int, redact_handles: bool) -> list[dict]:
    """Read from the daemon's memory.db `messages` table. Schema:
    (id, session_id, role, content, created_at)."""
    if not db_path.exists():
        return []
    out = []
    try:
        conn = sqlite3.connect(str(db_path))
        conn.text_factory = bytes
        cur = conn.cursor()
        cur.execute(
            "SELECT session_id, role, content, created_at "
            "FROM messages "
            "ORDER BY id DESC LIMIT ?",
            (max_records,))
        for sess_b, role_b, content_b, created_at_b in cur.fetchall():
            content = (content_b.decode("utf-8", "replace")
                       if isinstance(content_b, bytes) else (content_b or ""))
            role = (role_b.decode("utf-8", "replace")
                    if isinstance(role_b, bytes) else (role_b or ""))
            sess = (sess_b.decode("utf-8", "replace")
                    if isinstance(sess_b, bytes) else (sess_b or ""))
            content = content.strip()
            if not content or len(content) < 2:
                continue
            # created_at is TEXT like "2026-05-18 14:23:45". Parse → ms.
            ts_ms = 0
            try:
                ca = (created_at_b.decode("utf-8", "replace")
                      if isinstance(created_at_b, bytes) else (created_at_b or ""))
                ts_ms = int(time.mktime(time.strptime(ca, "%Y-%m-%d %H:%M:%S")) * 1000)
            except (ValueError, TypeError):
                pass
            out.append({
                "channel": "memory_db",
                "ts_ms": ts_ms,
                "handle": hash_handle(sess) if redact_handles else sess,
                "role": role,  # already "user" or "assistant"
                "content": redact_pii(content),
            })
        conn.close()
    except sqlite3.Error as e:
        print(f"  WARN: memory.db read failed ({e})", file=sys.stderr)
    return out


# ─────────────────────────────────────────────────────────────────────
# Stubs for credentialed sources — present for the operator surface
# ─────────────────────────────────────────────────────────────────────

def extract_gmail(_max: int, _redact: bool) -> list[dict]:
    """Gmail extractor stub. The credential is in
    ~/.human/config.json::gmail_refresh_token; the actual OAuth flow +
    Sent folder iteration is a separate slice (Gmail API + nontrivial
    HTML/text body cleaning). Surface kept for parity so the unified
    --sources flag accepts the name."""
    print("  NOTE: gmail extractor is a stub (network slice not implemented).")
    return []


def extract_slack(_max: int, _redact: bool) -> list[dict]:
    """Slack extractor stub. Same shape as gmail — present in the CLI
    but doesn't fetch yet. The Slack API requires conversation history
    scopes per DM channel, which is a multi-step extraction."""
    print("  NOTE: slack extractor is a stub.")
    return []


# ─────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────

SOURCE_DISPATCH = {
    "imessage": lambda max_rec, redact: extract_imessage(IMSG_DB, max_rec, redact),
    "memory_db": lambda max_rec, redact: extract_memory_db(MEMORY_DB, max_rec, redact),
    "gmail": lambda max_rec, redact: extract_gmail(max_rec, redact),
    "slack": lambda max_rec, redact: extract_slack(max_rec, redact),
}


def main():
    global IMSG_DB, MEMORY_DB
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--sources", default="imessage,memory_db",
                    help="Comma-separated subset of: imessage,memory_db,gmail,slack")
    ap.add_argument("--max-per-source", type=int, default=10000)
    ap.add_argument("--no-redact-handles", action="store_true",
                    help="Skip the SHA-256 handle hashing (keeps raw phone/email "
                         "in the 'handle' field). PII redaction of content always runs.")
    ap.add_argument("--imessage-db", type=Path, default=IMSG_DB,
                    help="Override iMessage chat.db path (for tests)")
    ap.add_argument("--memory-db", type=Path, default=MEMORY_DB,
                    help="Override memory.db path (for tests)")
    args = ap.parse_args()

    # Apply overrides for tests
    IMSG_DB = args.imessage_db
    MEMORY_DB = args.memory_db

    sources = [s.strip() for s in args.sources.split(",") if s.strip()]
    unknown = [s for s in sources if s not in SOURCE_DISPATCH]
    if unknown:
        print(f"ERROR: unknown sources: {unknown}. Available: "
              f"{list(SOURCE_DISPATCH.keys())}", file=sys.stderr)
        return 2

    print(f"\n{'='*60}")
    print(f"  M3 CORPUS EXTRACTOR (H1)")
    print(f"{'='*60}")
    print(f"  Sources:    {sources}")
    print(f"  Out:        {args.out}")
    print(f"  Max/src:    {args.max_per_source}")
    print(f"  Redact:     content=on  handles={not args.no_redact_handles}")
    print(f"{'='*60}\n")

    redact_handles = not args.no_redact_handles
    args.out.parent.mkdir(parents=True, exist_ok=True)

    counts = {}
    total = 0
    with open(args.out, "w") as fout:
        for src in sources:
            recs = SOURCE_DISPATCH[src](args.max_per_source, redact_handles)
            counts[src] = len(recs)
            for r in recs:
                fout.write(json.dumps(r, ensure_ascii=False) + "\n")
                total += 1
            print(f"  {src:<12}  {len(recs)} records")

    print(f"\n  TOTAL: {total} records → {args.out}")
    if total == 0:
        print("  ERROR: no records extracted from any source", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
