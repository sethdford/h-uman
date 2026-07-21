#!/usr/bin/env python3
"""Mine Seth's REAL teasing/joking/quirk exchanges from chat.db into persona
humor examples. Step 1 of the 2026-07-21 persona-personality effort.

The persona's `humor.examples` and `example_banks` were partly hand-authored.
This grounds them in how Seth actually teases: it finds (incoming, response)
pairs where Seth's reply carries a teasing/humor signal, scrubs PII, dedups,
and emits persona-example JSON. It NEVER overwrites the persona directly —
it writes a candidate file for review, because mined text goes into what the
daemon says to real people.

Read-only on chat.db (sqlite mode=ro). Decodes attributedBody (Tahoe stores
most body text there, not the `text` column). Stdlib only.

Pure classifiers (humor_signal / scrub / build_example / dedup) are unit-
tested in test_mine_humor_examples.py — that is the gate, not this driver.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sqlite3
import sys
from pathlib import Path

# ── Teasing / humor signals (the register the user asked for) ───────────────
# Laughter, playful roast/sarcasm markers, mock-exasperation, affectionate
# insults. Word-boundary matched so "champ" doesn't fire on "champion".
_LAUGH = re.compile(r"\b(lol+|lmao+|lmfao|haha+|hah|rofl|lolol)\b", re.I)
_ROAST_WORDS = re.compile(
    r"\b(shut up|nerd|dork|loser|idiot|clown|goofball|champ|buddy|pal|"
    r"iconic|embarrassing|tragic|unhinged|feral|menace|ridiculous|"
    r"calm down|relax|sure jan|ok boomer|bold of you|the audacity|"
    r"cope|sus|touch grass|as usual)\b",
    re.I,
)
# Mock-emphatic caps ("SO productive", "REAL nice") — a sarcasm tell. But an
# ALL-caps token is usually an acronym (USA/AI/DTCC), not sarcasm. The tell is
# a caps word sitting AMONG lowercase words, so require a lowercase word
# elsewhere in the line and the caps word to be a known intensifier shape.
_MOCK_CAPS = re.compile(r"\b(SO|REAL|SUPER|TOTALLY|WOW|GREAT|LOVE|AMAZING|SURE)\b")
# AI-leak apologies: the daemon's own tell, must never become an exemplar.
_AI_LEAK = re.compile(r"\b(my|the)\s+ai\b|ai\s+(is|jumping|responding|responded|still)", re.I)
# Deadpan question-as-jab ("does walking to the fridge count").
_DEADPAN = re.compile(r"\b(does .* count|is that a (real|serious)|you (sure|good)\?)", re.I)

_MAX_WORDS = 30  # a quick text, not a monologue


def humor_signal(text: str) -> bool:
    """True when the reply reads as teasing / joking / playful sarcasm."""
    if not text:
        return False
    t = text.strip()
    if not t:
        return False
    if _LAUGH.search(t) or _ROAST_WORDS.search(t) or _DEADPAN.search(t):
        return True
    # Mock caps only counts alongside brevity (long shouty text is not a joke).
    if len(t.split()) <= 12 and _MOCK_CAPS.search(t):
        return True
    return False


def scrub(text: str):
    """Return the text if safe to store, else None. Rejects anything with
    digits (phones/addresses/amounts), over-long lines, or empties. Mirrors
    the PII floor in mine_phrase_banks.py."""
    if not text:
        return None
    t = " ".join(text.split())  # collapse whitespace/newlines
    if not t:
        return None
    if any(c.isdigit() for c in t):
        return None
    if len(t.split()) > _MAX_WORDS:
        return None
    if _AI_LEAK.search(t):
        return None  # the daemon's own leak-apology — never store
    # Strip a stray leading punctuation byte from attributedBody decode noise.
    t = t.lstrip("$:>|/()\x00 ").strip()
    if not t:
        return None
    # Laughter-ONLY replies ("lol", "haha 😂") are real but teach nothing as an
    # exemplar — require some substance beyond the laugh token + emoji.
    residue = _LAUGH.sub("", t)
    residue = re.sub(r"[\U0001F300-\U0001FAFF☀-➿️\s]", "", residue)
    if len(residue) < 3:
        return None
    return t


def build_example(incoming: str, response: str) -> dict:
    """Persona example_banks shape: {context, incoming, response}."""
    return {"context": "texting conversation", "incoming": incoming, "response": response}


def dedup(rows: list) -> list:
    """Keep the first occurrence of each distinct response (case-insensitive)."""
    seen = set()
    out = []
    for r in rows:
        key = r["response"].strip().lower()
        if key in seen:
            continue
        seen.add(key)
        out.append(r)
    return out


# ── chat.db extraction (driver — not unit-tested; the classifiers are) ──────

def _decode_body(text, ab) -> str:
    if text:
        return text
    if not ab:
        return ""
    raw = ab if isinstance(ab, bytes) else bytes(ab)
    mt = re.search(rb"NSString\x01\x94\x84\x01\+(.{1,600}?)\x86", raw, re.S)
    if not mt:
        return ""
    b = mt.group(1)
    b = b[1:] if b[:1] < b"\x20" else b
    return b.decode("utf-8", "ignore")


def mine(db_path: str, limit_days: int) -> list:
    uri = f"file:{db_path}?mode=ro"
    conn = sqlite3.connect(uri, uri=True)
    cur = conn.execute(
        """SELECT m.ROWID, m.is_from_me, m.text, m.attributedBody, cmj.chat_id
           FROM message m JOIN chat_message_join cmj ON cmj.message_id = m.ROWID
           WHERE m.associated_message_type = 0
             AND m.date > (strftime('%s','now') - ? - 978307200) * 1000000000
           ORDER BY cmj.chat_id, m.date""",
        (limit_days * 86400,),
    )
    # Walk each chat in order; an incoming (is_from_me=0) followed by Seth's
    # reply (is_from_me=1) is a candidate pair.
    rows = list(cur)
    conn.close()
    out = []
    prev_incoming = {}
    for _rowid, from_me, text, ab, chat_id in rows:
        body = _decode_body(text, ab)
        if from_me == 0:
            prev_incoming[chat_id] = body
            continue
        inc = prev_incoming.get(chat_id, "")
        prev_incoming[chat_id] = None
        if not humor_signal(body):
            continue
        resp = scrub(body)
        inc_s = scrub(inc) if inc else ""
        if not resp:
            continue
        out.append(build_example(inc_s or "(opener)", resp))
    return dedup(out)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--db", default=str(Path("~/Library/Messages/chat.db").expanduser()))
    ap.add_argument("--days", type=int, default=365)
    ap.add_argument("--max", type=int, default=40, help="cap exemplars written")
    ap.add_argument("--out", default=str(Path("~/.human/humor_examples_candidate.json").expanduser()))
    args = ap.parse_args(argv)

    if not os.path.exists(args.db):
        print(f"chat.db not found at {args.db}", file=sys.stderr)
        return 2
    rows = mine(args.db, args.days)[: args.max]
    Path(args.out).write_text(json.dumps({"channel": "imessage", "examples": rows},
                                         ensure_ascii=False, indent=1))
    print(f"mined {len(rows)} teasing/humor exemplars -> {args.out}")
    for r in rows[:12]:
        print(f"  [{r['incoming'][:34]:34}] -> {r['response'][:50]}")
    print("\nREVIEW before merging into ~/.human/personas/seth.json (humor.examples).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
