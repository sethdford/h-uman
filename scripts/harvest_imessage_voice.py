#!/usr/bin/env python3
"""
Harvest the user's OWN sent iMessages as a voice corpus — the no-new-inbound
path to a viable training set.

The problem: on modern macOS, Messages stores text in the binary `attributedBody`
blob, not the `text` column. h-uman's live ingest decodes it, but the historical
harvest (`human persona learn-banks --from-imessage`) reads `text` and so misses
nearly everything. This tool decodes `attributedBody` for the sent (is_from_me=1)
messages — pure voice signal that ALREADY EXISTS on the machine — and writes a
voice corpus, then reports the data-viability verdict on it.

Read-only on chat.db. Writes the corpus to a private path (default ~/.human/),
never to the repo. Run on your machine (needs Full Disk Access to chat.db).

Usage:
  scripts/harvest_imessage_voice.py [--chat-db ~/Library/Messages/chat.db] \\
      [--out ~/.human/voice_corpus.jsonl] [--min-len 3]
"""

import argparse
import json
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import voice_data_audit as vda  # reuse the verdict logic

DEFAULT_CHAT_DB = Path.home() / "Library" / "Messages" / "chat.db"
DEFAULT_OUT = Path.home() / ".human" / "voice_corpus.jsonl"


def decode_attributed_body(blob):
    """Heuristic NSAttributedString (streamtyped) text extraction — the approach
    common iMessage exporters use. Finds the primary NSString payload after the
    class marker and reads its length-prefixed UTF-8. Returns '' if not found."""
    if not blob:
        return ""
    if isinstance(blob, str):
        return blob
    marker = blob.find(b"NSString")
    if marker < 0:
        return ""
    plus = blob.find(b"+", marker)
    if plus < 0 or plus + 1 >= len(blob):
        return ""
    k = plus + 1
    n = blob[k]
    if n == 0x81:  # 0x81 => 2-byte little-endian length follows
        if k + 3 > len(blob):
            return ""
        length = int.from_bytes(blob[k + 1:k + 3], "little")
        start = k + 3
    else:
        length = n
        start = k + 1
    text = blob[start:start + length]
    return text.decode("utf-8", "replace").strip()


_REACTION_PREFIXES = ("Liked ", "Loved ", "Disliked ", "Laughed at ", "Emphasized ",
                      "Questioned ", "Reacted ")


def is_voice_signal(text):
    """Keep only messages that carry the user's distinctive prose voice. Drops
    URL-only sends, iMessage tapback-echo strings, and pure emoji/punctuation —
    quality over quantity (the research: 200 curated beat 2,000 sloppy)."""
    t = text.strip()
    if not t:
        return False
    if t.startswith(_REACTION_PREFIXES):
        return False
    # Strip URLs; require real prose to remain.
    import re
    prose = re.sub(r"https?://\S+", "", t).strip()
    if len(prose) < 3:
        return False  # URL-only or trivial
    if not any(c.isalpha() for c in prose):
        return False  # pure emoji / punctuation / numbers
    return True


def harvest(chat_db, min_len):
    """Yield (chat_id, text) for each decodable sent message. chat_id groups by
    conversation (handle_id) so the audit can count sessions."""
    con = sqlite3.connect(f"file:{chat_db}?mode=ro", uri=True)
    con.text_factory = bytes  # we decode text/attributedBody ourselves
    try:
        rows = con.execute(
            "SELECT handle_id, text, attributedBody FROM message "
            "WHERE is_from_me=1 ORDER BY handle_id, date"
        )
        for handle_id, text, body in rows:
            t = ""
            if text:
                t = text.decode("utf-8", "replace").strip() if isinstance(text, bytes) else str(text).strip()
            if not t:
                t = decode_attributed_body(body)
            if t and len(t) >= min_len and is_voice_signal(t):
                yield f"h{handle_id}", t
    finally:
        con.close()


def main():
    ap = argparse.ArgumentParser(description="Harvest sent iMessages as a voice corpus")
    ap.add_argument("--chat-db", type=Path, default=DEFAULT_CHAT_DB)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--min-len", type=int, default=3)
    args = ap.parse_args()

    if not args.chat_db.exists():
        print(f"[error] chat.db not found: {args.chat_db}", file=sys.stderr)
        return 1
    try:
        pairs = list(harvest(args.chat_db, args.min_len))
    except sqlite3.OperationalError as e:
        print(f"[error] cannot read chat.db (Full Disk Access required?): {e}", file=sys.stderr)
        return 1

    # Write the corpus (one JSON object per line) — private, never the repo.
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w") as f:
        for _, text in pairs:
            f.write(json.dumps({"text": text}) + "\n")

    # Audit the harvested corpus using the W7 verdict logic (treat each sent
    # message as an assistant turn so the thresholds apply to voice signal).
    rows = [(cid, "assistant", text) for cid, text in pairs]
    stats = vda.audit_rows(rows)
    verdict, reasons = vda.data_viability_verdict(stats)
    print(json.dumps({"harvested": len(pairs), "out": str(args.out),
                      "verdict": verdict, "reasons": reasons, "stats": stats}, indent=2))
    print(f"\n=== HARVESTED VOICE CORPUS: {len(pairs)} sent messages → {verdict.upper()} ===",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
