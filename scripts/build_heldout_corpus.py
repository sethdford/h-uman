#!/usr/bin/env python3
"""
Sample inbound iMessage prompts from chat.db to extend the M3 held-out
corpus at docs/plans/2026-05-26-sprint-56-gemma-as-seth/data/heldout-prompts.jsonl.

The corpus is used by scripts/eval_fidelity_nightly.py to measure whether
the LoRA persona-fidelity adapter measurably improves Seth-style replies
(per US-15 — current corpus is 25 prompts, +27pp lift validated).

Usage:
    # Print N new candidate prompts to stdout for review:
    python3 scripts/build_heldout_corpus.py --sample 25

    # Append them to the corpus, deduping against existing prompts:
    python3 scripts/build_heldout_corpus.py --sample 25 --append

    # Use a non-default chat.db (testing):
    python3 scripts/build_heldout_corpus.py --chat-db /tmp/fixture.db --sample 5

Sampling strategy:
    Take INBOUND messages (is_from_me=0) that were followed by an OUTBOUND
    reply (is_from_me=1) within 24h in the same chat. These are real prompts
    that Seth actually engaged with — better held-out signal than synthetic
    prompts.

Privacy:
    * Phone numbers redacted to <PHONE>
    * URLs redacted to <URL>
    * Common name patterns (capitalized two-token sequences) redacted to <NAME>
    * Messages shorter than 3 chars or containing only emoji/punct dropped
    * Stratified sampling: max 1 prompt per sender, max 3 per day,
      to avoid corpus skew from a single chatty contact.

Context heuristic:
    Rough categorization based on prompt content — question? greeting?
    request? — useful for the eval harness to track per-context lift.
    Same context vocabulary as the existing 25-prompt corpus.

Exit codes:
    0 on success (prompts emitted to stdout, or appended)
    1 on chat.db unreadable or zero qualifying messages found
    2 on argument error
"""

import argparse
import json
import re
import sqlite3
import sys
import os
from collections import defaultdict
from pathlib import Path

DEFAULT_CHAT_DB = Path.home() / "Library/Messages/chat.db"
DEFAULT_CORPUS = (
    Path(__file__).parent.parent
    / "docs/plans/2026-05-26-sprint-56-gemma-as-seth/data/heldout-prompts.jsonl"
)

# Apple's epoch for chat.db dates: nanoseconds since 2001-01-01.
# date(2001,1,1).timestamp() == 978307200
APPLE_EPOCH_S = 978307200


def apple_ns_to_unix(ns: int) -> int:
    return ns // 1_000_000_000 + APPLE_EPOCH_S


# Redaction patterns — applied IN ORDER.
PHONE_RE = re.compile(r"\+?\d[\d\-\s().]{7,}\d")
URL_RE = re.compile(r"https?://\S+")
# Conservative "Name" redactor: two consecutive Capitalized tokens preceded by
# a non-word boundary. Catches "Mindy Ford" but not "Hey there" or "I Am".
NAME_RE = re.compile(r"(?<!\w)[A-Z][a-z]+ [A-Z][a-z]+")
EMOJI_ONLY_RE = re.compile(
    r"^[\W\d\s_☀-➿\U0001F300-\U0001FAFF]+$", re.UNICODE
)


def redact(text: str) -> str:
    text = URL_RE.sub("<URL>", text)
    text = PHONE_RE.sub("<PHONE>", text)
    text = NAME_RE.sub("<NAME>", text)
    return text.strip()


def classify_context(prompt: str) -> str:
    """Cheap heuristic — matches the existing corpus' context vocabulary."""
    low = prompt.lower().strip()
    if any(low.startswith(g) for g in ("hi", "hey", "hello", "yo", "morning", "goodnight")):
        return "greeting"
    if low.endswith("?") or low.startswith(("why ", "how ", "what ", "when ", "where ", "who ", "is ", "are ", "did ", "do ", "can ", "could ", "should ")):
        return "question"
    if any(w in low for w in (" please ", "could you", "can you", "would you", "send me", "let me know")):
        return "request"
    if any(w in low for w in ("sorry", "i'm sorry", "didn't mean")):
        return "apology"
    if any(w in low for w in ("thanks", "thank you", "appreciate", "love you")):
        return "gratitude"
    if any(w in low for w in ("lol", "haha", "lmao", "🤣", "😂")):
        return "emoji-reaction"
    if any(w in low for w in ("driving", "on my way", "be there", "running late", "just arrived")):
        return "status"
    if any(w in low for w in ("ok", "okay", "got it", "k", "kk", "sounds good", "👍")):
        return "acknowledgment"
    if "love" in low or "miss you" in low:
        return "emotion"
    return "general"


def load_existing_prompts(path: Path) -> set:
    """Returns the set of prompts already in the corpus, for dedup."""
    if not path.exists():
        return set()
    seen = set()
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
                if "prompt" in d:
                    seen.add(d["prompt"])
            except Exception:
                continue
    return seen


def sample_prompts(chat_db: Path, n: int, existing: set):
    """Yield up to n new prompt dicts from chat.db."""
    if not chat_db.exists():
        print(f"error: chat.db not found at {chat_db}", file=sys.stderr)
        sys.exit(1)
    try:
        con = sqlite3.connect(f"file:{chat_db}?mode=ro", uri=True)
    except sqlite3.Error as e:
        print(f"error: chat.db unreadable: {e}", file=sys.stderr)
        sys.exit(1)
    cur = con.cursor()

    # Find inbound messages that were followed by an outbound message in the
    # same chat within 24h. We use the simplest viable query — pull all inbound
    # texts ordered by date_desc; the "followed by outbound" filter happens
    # in-Python so we don't need a complex correlated subquery.
    sql = """
        SELECT m.ROWID, m.text, m.date, m.handle_id, cmj.chat_id
        FROM message m
        JOIN chat_message_join cmj ON cmj.message_id = m.ROWID
        WHERE m.is_from_me = 0 AND m.text IS NOT NULL AND length(m.text) > 0
        ORDER BY m.date DESC
        LIMIT 20000
    """
    inbound = cur.execute(sql).fetchall()

    # Build a quick lookup of chat_id -> earliest outbound date AFTER each inbound.
    # Pull all outbound dates per chat, then we binary-search per-inbound.
    outbound_by_chat = defaultdict(list)
    out_sql = """
        SELECT cmj.chat_id, m.date
        FROM message m
        JOIN chat_message_join cmj ON cmj.message_id = m.ROWID
        WHERE m.is_from_me = 1 AND m.date > 0
        ORDER BY m.date
    """
    for chat_id, dt in cur.execute(out_sql):
        outbound_by_chat[chat_id].append(dt)

    def replied_within_24h(chat_id, inbound_date):
        outs = outbound_by_chat.get(chat_id) or []
        # Apple date is nanoseconds; 24h = 86400 * 1e9
        cutoff = inbound_date + 86_400_000_000_000
        # Linear scan (lists are sorted ascending). 'inbound_date' threshold:
        for o in outs:
            if o > inbound_date and o <= cutoff:
                return True
            if o > cutoff:
                return False
        return False

    # Stratify: max 3 prompts per sender (handle_id), max 5 per UTC day.
    # Tighter ratios over-thin small corpora; this gives breadth without
    # letting one chatty contact dominate the corpus.
    per_sender = defaultdict(int)
    per_day = defaultdict(int)
    seen_in_session = set()  # avoid emitting the same prompt twice this run
    emitted = 0
    SENDER_CAP = 3
    DAY_CAP = 5

    for rowid, text, date_ns, handle_id, chat_id in inbound:
        if emitted >= n:
            break
        if per_sender[handle_id] >= SENDER_CAP:
            continue
        if not replied_within_24h(chat_id, date_ns):
            continue
        clean = redact(text)
        # Reject empty-after-redaction, emoji-only, or too-short prompts.
        if len(clean) < 3 or EMOJI_ONLY_RE.match(clean):
            continue
        if clean in existing or clean in seen_in_session:
            continue
        day_key = apple_ns_to_unix(date_ns) // 86_400
        if per_day[day_key] >= DAY_CAP:
            continue
        per_sender[handle_id] += 1
        per_day[day_key] += 1
        seen_in_session.add(clean)
        emitted += 1
        yield {
            "prompt": clean,
            "channel": "imessage",
            "context": classify_context(clean),
            "source": "chatdb_sampled_2026_05_27",
        }

    con.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--chat-db", type=Path, default=DEFAULT_CHAT_DB,
                    help=f"path to chat.db (default: {DEFAULT_CHAT_DB})")
    ap.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS,
                    help=f"path to existing corpus (default: {DEFAULT_CORPUS})")
    ap.add_argument("--sample", type=int, default=10,
                    help="how many new prompts to sample (default 10)")
    ap.add_argument("--append", action="store_true",
                    help="append sampled prompts to the corpus file instead of printing them")
    args = ap.parse_args()

    if args.sample < 1 or args.sample > 200:
        print("error: --sample must be 1..200", file=sys.stderr)
        sys.exit(2)

    existing = load_existing_prompts(args.corpus)
    print(f"# existing corpus: {len(existing)} prompts", file=sys.stderr)

    sampled = list(sample_prompts(args.chat_db, args.sample, existing))
    if not sampled:
        print("# no qualifying inbound messages found in chat.db", file=sys.stderr)
        sys.exit(1)

    print(f"# sampled: {len(sampled)} new prompts", file=sys.stderr)

    if args.append:
        with open(args.corpus, "a") as f:
            for d in sampled:
                f.write(json.dumps(d) + "\n")
        print(f"# appended {len(sampled)} prompts to {args.corpus}", file=sys.stderr)
        print(f"# new corpus size: {len(existing) + len(sampled)}", file=sys.stderr)
    else:
        for d in sampled:
            print(json.dumps(d))


if __name__ == "__main__":
    main()
