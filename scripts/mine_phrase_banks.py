#!/usr/bin/env python3
"""Mine Seth's OWN phrase distributions from sent iMessages -> phrase banks.

Why: src/context/conversation.c ships hardcoded generic-English DEFAULT_*
arrays (fillers, starters, backchannels, farewells). Those are someone else's
texting habits. This miner extracts the real distributions from is_from_me=1
messages in chat.db and writes ~/.human/phrase_banks.json, which the daemon
loads at init (hu_conversation_phrase_banks_load) so the DEFAULT_* arrays
become pure fallbacks.

Extracted per channel ("imessage" only in v1):
  fillers/starters  message-initial tokens ("haha ", "ngl ", ...) with freq.
                    Stored WITH a trailing space — exactly the shape the C
                    consumers use for prefix matching / injection.
  backchannels      full-message one-worders ("yeah", "fr", "ha") with freq.
  farewells         last own message before a >gap-hours silence in the chat
                    (or the final message of a chat), <=3 words, with freq.

Safety:
  - read-only on chat.db (sqlite URI mode=ro)
  - min-frequency floor (default 5) — rare phrases never enter the bank
  - PII scrub: drop anything containing digits; drop words the corpus itself
    marks as proper nouns (capitalized in the majority of their mid-sentence
    occurrences — message-initial capitalization is meaningless because of
    autocapitalize). Same spirit as the redaction in the mine-corrections
    miner (src/ml/dpo_miner.c): scrub before anything is persisted.

Usage:
  scripts/mine_phrase_banks.py [--db ~/Library/Messages/chat.db] \
      [--out ~/.human/phrase_banks.json] [--min-freq 5] [--gap-hours 6]
"""

import argparse
import json
import re
import sqlite3
import sys
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from harvest_imessage_voice import _REACTION_PREFIXES, decode_attributed_body

DEFAULT_CHAT_DB = Path.home() / "Library" / "Messages" / "chat.db"
DEFAULT_OUT = Path.home() / ".human" / "phrase_banks.json"

TOP_N = {"fillers": 20, "starters": 20, "backchannels": 15, "farewells": 15}
STANDALONE_MIN = 2  # standalone occurrences required before a token is a filler
MAX_TOKEN_LEN = 12
MAX_FAREWELL_WORDS = 3
MAX_FAREWELL_LEN = 30

_WORD_RE = re.compile(r"[A-Za-z'][A-Za-z']*")
_CLEAN_TOKEN_RE = re.compile(r"^[a-z][a-z']*$")


def apple_date_to_seconds(raw):
    """chat.db `date` is ns since 2001-01-01 on modern macOS, seconds on
    ancient rows. Normalize to seconds (epoch base is irrelevant here — we
    only ever take differences)."""
    if raw is None:
        return 0.0
    raw = float(raw)
    return raw / 1e9 if abs(raw) > 1e11 else raw


def _decode_text(text, body):
    if text:
        return (text.decode("utf-8", "replace") if isinstance(text, bytes) else str(text)).strip()
    return decode_attributed_body(body)


def iter_messages(db_path):
    """Yield (handle_id, is_from_me, seconds, text) for every decodable
    message, ordered per conversation by time. Read-only."""
    con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    con.text_factory = bytes
    try:
        rows = con.execute(
            "SELECT handle_id, is_from_me, date, text, attributedBody FROM message "
            "ORDER BY handle_id, date"
        )
        for handle_id, is_from_me, date, text, body in rows:
            t = _decode_text(text, body)
            if not t:
                continue
            yield handle_id, int(is_from_me or 0), apple_date_to_seconds(date), t
    finally:
        con.close()


def _is_reaction_echo(text):
    return text.startswith(_REACTION_PREFIXES)


def _strip_token(tok):
    """Lowercase a raw token and strip surrounding punctuation/quotes."""
    return tok.strip("\"'“”‘’.,!?()[]…-—:;").lower()


def build_proper_noun_set(sent_texts, min_occurrences=2, cap_ratio=0.5):
    """Words that are capitalized in the majority of their MID-sentence
    occurrences are treated as proper nouns (names) and scrubbed. The first
    word of a message is excluded from the stats — autocapitalize makes its
    case meaningless."""
    total = Counter()
    capitalized = Counter()
    for text in sent_texts:
        words = _WORD_RE.findall(text)
        for w in words[1:]:
            key = w.lower()
            total[key] += 1
            if w[0].isupper():
                capitalized[key] += 1
    return {
        w
        for w, n in total.items()
        if n >= min_occurrences and capitalized[w] / n > cap_ratio
    }


def _phrase_is_clean(phrase, proper_nouns):
    """PII scrub predicate for a lowercase candidate phrase."""
    if any(ch.isdigit() for ch in phrase):
        return False
    words = phrase.split()
    if not words:
        return False
    for w in words:
        if not _CLEAN_TOKEN_RE.match(w):
            return False
        if w in proper_nouns:
            return False
    return True


def _bank(counter, proper_nouns, min_freq, top_n, suffix=""):
    entries = [
        {"text": phrase + suffix, "freq": freq}
        for phrase, freq in counter.most_common()
        if freq >= min_freq and _phrase_is_clean(phrase, proper_nouns)
    ]
    return entries[:top_n]


def mine(db_path, min_freq=5, gap_hours=6.0):
    """Extract the phrase banks. Returns {"imessage": {fillers, starters,
    backchannels, farewells}} where each value is [{"text":..,"freq":..}]."""
    by_chat = defaultdict(list)
    for handle_id, is_from_me, sec, text in iter_messages(db_path):
        by_chat[handle_id].append((sec, is_from_me, text))

    sent_texts = [
        t
        for msgs in by_chat.values()
        for (_, from_me, t) in msgs
        if from_me and not _is_reaction_echo(t)
    ]
    proper_nouns = build_proper_noun_set(sent_texts)

    initial = Counter()  # message-initial tokens (fillers + starters)
    backchannels = Counter()  # full-message one-worders
    farewells = Counter()  # last own message before a long silence

    for text in sent_texts:
        raw_words = text.split()
        if len(raw_words) == 1:
            w = _strip_token(raw_words[0])
            if w:
                backchannels[w] += 1
        elif len(raw_words) >= 2:
            tok = _strip_token(raw_words[0])
            if tok and 2 <= len(tok) <= MAX_TOKEN_LEN:
                initial[tok] += 1

    gap_seconds = gap_hours * 3600.0
    for msgs in by_chat.values():
        msgs.sort(key=lambda m: m[0])
        for i, (sec, from_me, text) in enumerate(msgs):
            if not from_me or _is_reaction_echo(text):
                continue
            is_last = i + 1 == len(msgs)
            if not is_last and msgs[i + 1][0] - sec <= gap_seconds:
                continue
            phrase = " ".join(_strip_token(w) for w in text.split())
            if not phrase or len(phrase) > MAX_FAREWELL_LEN:
                continue
            if len(phrase.split()) > MAX_FAREWELL_WORDS:
                continue
            farewells[phrase] += 1

    # A filler is injected as a reply PREFIX, so it must read naturally on its
    # own — require repeated standalone one-word occurrences (measured on real
    # data: true fillers "yeah"/"oh"/"sure"/"ok" have >=7, while "you"/"hey"
    # have exactly 1 from stray "you?"-style replies). Starters are only
    # prefix-MATCHED (bubble splitting), so the full message-initial
    # distribution is correct for them.
    standalone_capable = Counter(
        {tok: n for tok, n in initial.items() if backchannels[tok] >= STANDALONE_MIN}
    )
    channel = {
        "fillers": _bank(standalone_capable, proper_nouns, min_freq, TOP_N["fillers"], suffix=" "),
        "starters": _bank(initial, proper_nouns, min_freq, TOP_N["starters"], suffix=" "),
        "backchannels": _bank(backchannels, proper_nouns, min_freq, TOP_N["backchannels"]),
        "farewells": _bank(farewells, proper_nouns, min_freq, TOP_N["farewells"]),
    }
    return {"imessage": channel}


def main(argv=None):
    ap = argparse.ArgumentParser(description="Mine phrase banks from sent iMessages")
    ap.add_argument("--db", type=Path, default=DEFAULT_CHAT_DB)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--min-freq", type=int, default=5)
    ap.add_argument("--gap-hours", type=float, default=6.0)
    args = ap.parse_args(argv)

    if not args.db.exists():
        print(f"[error] chat.db not found: {args.db}", file=sys.stderr)
        return 1
    try:
        banks = mine(args.db, min_freq=args.min_freq, gap_hours=args.gap_hours)
    except sqlite3.OperationalError as e:
        print(f"[error] cannot read chat.db (Full Disk Access required?): {e}", file=sys.stderr)
        return 1

    banks["_meta"] = {
        "generated_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "min_freq": args.min_freq,
        "gap_hours": args.gap_hours,
        "source": "mine_phrase_banks.py",
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(banks, indent=2, ensure_ascii=False) + "\n")

    ch = banks["imessage"]
    print(
        json.dumps(
            {
                "out": str(args.out),
                "fillers": len(ch["fillers"]),
                "starters": len(ch["starters"]),
                "backchannels": len(ch["backchannels"]),
                "farewells": len(ch["farewells"]),
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
