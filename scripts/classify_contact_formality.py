#!/usr/bin/env python3
"""
Contact Formality Classifier — Analyze Seth's messaging register per contact.

Reads outgoing messages from ~/Library/Messages/chat.db and scores each contact's
communication style using an objective register metric (formality + warmth).
Produces a dry-run proposal table for seeding formal contacts into persona config.

Usage:
  classify_contact_formality.py [--dry-run]      (default: dry-run only, no writes)
  classify_contact_formality.py --apply --i-understand-live-persona
                                                  (would write seth.json — NOT EXECUTED)

Privacy: Never commits raw message text, phone numbers, or contact names to git.
Outputs hashed/truncated handles + scores only.
"""

import sqlite3
import re
import sys
import os
import struct
from pathlib import Path
from collections import defaultdict
from dataclasses import dataclass
from typing import Optional, Dict, List, Tuple
import hashlib


@dataclass
class ContactScore:
    """Per-contact scoring result."""
    handle: str                    # phone/email (truncated for privacy)
    handle_display_name: Optional[str]  # name if available
    n_messages: int
    formality_score: float         # [0, 1] — mean formality of outgoing msgs
    warmth_score: float            # [0, 1] — mean warmth of outgoing msgs
    register_avg: float            # simple mean

    def proposed_formality(self) -> str:
        """Classify as formal/casual/insufficient based on register score."""
        if self.n_messages < 15:
            return "insufficient"
        # Formality > 0.55 suggests professional/formal register
        if self.formality_score > 0.55:
            return "formal"
        else:
            return "casual"

    def confidence(self) -> float:
        """Higher n_messages → higher confidence. Subjective floor: 15 msgs."""
        if self.n_messages < 15:
            return 0.0
        return min(1.0, self.n_messages / 50.0)


def truncate_handle(handle: str, keep_chars: int = 4) -> str:
    """Truncate phone/email for privacy."""
    if not handle:
        return "[unknown]"
    # Hash the full handle
    h = hashlib.sha256(handle.encode()).hexdigest()[:8]
    # Keep last N chars if it's a phone, or domain if email
    if handle.startswith("+"):
        return f"{handle[-keep_chars:]}...{h}"
    elif "@" in handle:
        parts = handle.split("@")
        return f"***@{parts[-1][:4]}...{h}"
    else:
        return f"{handle[:keep_chars]}...{h}"


def decode_attributed_body(blob: bytes) -> Optional[str]:
    """
    Attempt to extract a UTF-8 string from an NSAttributedString typedstream blob.

    Apple typedstream format: NSString/NSMutableString payload is preceded by
    length-prefixing. Heuristics (in order):
    1. Find b"NSString" or b"NSMutableString", then look for length+payload
    2. Scan for plausible UTF-8 runs of increasing length
    3. Return longest valid UTF-8 found, or None

    Returns None if decoding fails or blob is too small.
    """
    if not blob or len(blob) < 4:
        return None

    # Heuristic 1: Look for NSString marker and extract length-prefixed string
    for marker in [b"NSString", b"NSMutableString"]:
        idx = blob.find(marker)
        if idx == -1:
            continue

        # Skip marker, look for length encoding starting within next ~50 bytes
        search_start = idx + len(marker)
        if search_start + 2 >= len(blob):
            continue

        # Try to parse length starting right after the marker
        # Length encoding: 1 byte if < 0x80, or 0x81 followed by uint16-LE
        offset = search_start
        text_len = None
        payload_start = None

        while offset < min(search_start + 50, len(blob) - 1):
            byte = blob[offset]

            # Try 1-byte length
            if byte < 0x80 and byte > 0:  # Plausible string length
                text_len = byte
                payload_start = offset + 1
                if payload_start + text_len <= len(blob):
                    try:
                        text = blob[payload_start:payload_start + text_len].decode('utf-8')
                        if text and text.isprintable() and len(text) > 2:
                            return text
                    except UnicodeDecodeError:
                        pass

            # Try 0x81 + uint16-LE encoding
            if byte == 0x81 and offset + 2 < len(blob):
                text_len = struct.unpack('<H', blob[offset+1:offset+3])[0]
                payload_start = offset + 3
                if payload_start + text_len <= len(blob) and text_len < 4000:
                    try:
                        text = blob[payload_start:payload_start + text_len].decode('utf-8')
                        if text and text.isprintable() and len(text) > 2:
                            return text
                    except (UnicodeDecodeError, struct.error):
                        pass

            offset += 1

    # Heuristic 2: Scan for longest valid UTF-8 runs (fallback)
    longest_text = None
    longest_len = 0

    for start in range(len(blob) - 3):
        for end in range(start + 3, min(start + 4000, len(blob))):
            try:
                text = blob[start:end].decode('utf-8')
                if text.isprintable() and len(text) > longest_len and len(text) > 2:
                    longest_text = text
                    longest_len = len(text)
            except UnicodeDecodeError:
                continue

    return longest_text if longest_text and longest_len > 5 else None


def _word_stretch_hit(tok: str, text_lower: str) -> bool:
    """
    Stretch-aware word match, mirroring contains_word_stretch_ci in
    src/eval/register.c: left word boundary (waived when the token starts
    non-alnum, e.g. "<3"), and a right word boundary after absorbing repeats
    of the token's final character (waived when the token ends non-alnum,
    e.g. "dear ", "love,"). Keeps "heyyy"/"yesss" recall while rejecting
    "they">"hey", "phone">"hon", "disregards">"regards", "unkindly">"kindly".
    """
    left = r"(?<![a-z0-9])" if tok[0].isalnum() else ""
    right = re.escape(tok[-1]) + r"*(?![a-z0-9])" if tok[-1].isalnum() else ""
    return re.search(left + re.escape(tok) + right, text_lower) is not None


def estimate_formality(text: str) -> float:
    """
    Estimate formality [0,1] from text. Mirrors the C logic in src/eval/register.c.

    - Casual markers: lol, u, ya, lowercase text, no punctuation
    - Formal markers: dear, sincerely, proper capitalization + punctuation
    """
    if not text or text.strip() == "":
        return 0.5  # neutral on blank

    text_lower = text.lower()

    # Casual word-boundary tokens (short, overlap-prone)
    casual_words = {
        "lol", "lmao", "omg", "idk", "tbh", "btw", "rn", "u", "ur",
        "ya", "yeah", "yep", "nah", "haha", "hey", "yo", "sup", "dude",
        "kinda", "gonna", "wanna", "gotta", "ngl", "fr", "af", "lowkey", "imo",
    }
    casual_hits = 0
    for word in casual_words:
        # Word boundary match: \b word \b
        if re.search(rf'\b{re.escape(word)}\b', text_lower):
            casual_hits += 1
    casual_hits = min(casual_hits, 3)

    # Formal tokens — stretch-aware word matched (mirrors register.c)
    formal_subs = [
        "dear ", "sincerely", "regards", "kindly", "please find",
        "i would like", "at your convenience", "to whom it may",
        "thank you for your", "best regards", "yours truly",
        "i will respond", "in due course", "revert to you",
        "regarding this matter", "i appreciate your",
    ]
    formal_hits = sum(1 for s in formal_subs if _word_stretch_hit(s, text_lower))
    formal_hits = min(formal_hits, 3)

    # All-lowercase alphabetic text → casual
    has_alpha = any(c.isalpha() for c in text)
    has_upper = any(c.isupper() for c in text)
    all_lower = has_alpha and not has_upper

    # Proper sentence: capitalized start + terminal punctuation
    first_alpha_idx = next((i for i, c in enumerate(text) if c.isalpha()), len(text))
    cap_start = first_alpha_idx < len(text) and text[first_alpha_idx].isupper()
    last_char = text.rstrip()[-1] if text.rstrip() else ""
    terminal_punct = last_char in ".!?"
    proper = cap_start and terminal_punct and casual_hits == 0

    score = 0.5
    score -= 0.18 * casual_hits
    score -= 0.20 if all_lower else 0.0
    score += 0.20 * formal_hits
    score += 0.12 if proper else 0.0

    return max(0.0, min(1.0, score))


def estimate_warmth(text: str) -> float:
    """
    Estimate warmth [0,1] from text. Mirrors the C logic in src/eval/register.c.

    - Warm markers: hey, hello, miss you, love, emoji
    - Distant markers: received, noted, per, transaction, follow up
    """
    if not text or text.strip() == "":
        return 0.5  # neutral on blank

    text_lower = text.lower()

    # Warm tokens — stretch-aware word matched so "heyyy"/"yesss" still hit
    # while "they"/"phone"/"honestly" no longer false-positive (mirrors register.c)
    warm_subs = [
        "hey", "hello", "miss you", "miss u", "thinking of you",
        "can't wait", "cant wait", "so happy", "love it", "love you",
        "amazing", "congrats", "xo", "<3", "so good to",
        "good to hear", "yay", "yess", "buddy", "babe",
        "sweetie", "hon", "cutie", "hugs", "talk soon",
        "my friend", "love,",
    ]
    warm_hits = sum(1 for s in warm_subs if _word_stretch_hit(s, text_lower))

    # Warm word-boundary tokens
    warm_words = {"hi", "yo", "morning", "love", "friend", "pal", "cheers"}
    warm_hits += sum(1 for w in warm_words if re.search(rf'\b{re.escape(w)}\b', text_lower))

    # Distant substrings
    distant_subs = [
        "received", "noted", "as discussed", "per the", "per my",
        "confirmed", "processed", "transaction", "attached",
        "in due course", "regarding this matter", "revert",
        "follow up shortly", "to confirm", "has been recorded",
        "as per", "for your records",
    ]
    distant_hits = sum(1 for s in distant_subs if _word_stretch_hit(s, text_lower))
    distant_hits = min(distant_hits, 3)

    warm_hits = min(warm_hits, 4)

    # Exclamation marks and emoji (high-byte chars)
    excl_count = text.count("!")
    excl_count = min(excl_count, 3)
    has_emoji = any(ord(c) >= 0x80 for c in text)

    score = 0.5
    score += 0.15 * warm_hits
    score += 0.05 * excl_count
    score += 0.08 if has_emoji else 0.0
    score -= 0.20 * distant_hits

    return max(0.0, min(1.0, score))


def read_messages_from_db(db_path: str) -> Tuple[Dict[str, List[str]], Dict[str, int]]:
    """
    Read outgoing messages from macOS Messages database using proper chat linkage.

    For outgoing messages (is_from_me=1), message.handle_id is almost always 0,
    so we must route through chat_message_join and chat_handle_join to attribute
    messages to contacts correctly.

    Filters to 1:1 DMs only (chats with exactly one handle) for clean per-contact attribution.
    Decodes attributedBody blobs to extract text when message.text is NULL.

    Returns:
      (dict: handle -> list of message texts, dict: stats about the data)

    Raises FileNotFoundError if db_path doesn't exist or is unreadable.
    """
    messages_by_contact = defaultdict(list)
    stats = {
        "total_messages_found": 0,
        "messages_with_text": 0,
        "messages_with_attributedBody_only": 0,
        "decoded_from_attributedBody": 0,
        "failed_to_decode": 0,
        "one_to_one_chats": 0,
        "group_chats_skipped": 0,
    }

    try:
        conn = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
        cursor = conn.cursor()

        # First, find all 1:1 chats (chats with exactly one handle)
        cursor.execute("""
            SELECT cmj.chat_id
            FROM chat_message_join cmj
            JOIN message m ON m.ROWID = cmj.message_id
            JOIN chat_handle_join chj ON chj.chat_id = cmj.chat_id
            WHERE m.is_from_me = 1
            GROUP BY cmj.chat_id
            HAVING COUNT(DISTINCT chj.handle_id) = 1
        """)

        one_to_one_chat_ids = [row[0] for row in cursor.fetchall()]
        stats["one_to_one_chats"] = len(one_to_one_chat_ids)

        if not one_to_one_chat_ids:
            conn.close()
            return messages_by_contact, stats

        # Placeholder for chat IDs
        placeholders = ",".join("?" * len(one_to_one_chat_ids))

        # Now get all outgoing messages from those 1:1 chats, with their contact handles
        query = f"""
            SELECT h.id, m.text, m.attributedBody, m.date
            FROM message m
            JOIN chat_message_join cmj ON cmj.message_id = m.ROWID
            JOIN chat_handle_join chj ON chj.chat_id = cmj.chat_id
            JOIN handle h ON h.ROWID = chj.handle_id
            WHERE m.is_from_me = 1
              AND cmj.chat_id IN ({placeholders})
              AND m.is_empty = 0
            ORDER BY h.id, m.date
        """

        cursor.execute(query, one_to_one_chat_ids)

        for handle, text, attributed_body, date in cursor.fetchall():
            if not handle:
                continue

            stats["total_messages_found"] += 1
            extracted_text = None

            # Prefer message.text
            if text and text.strip():
                extracted_text = text
                stats["messages_with_text"] += 1
            elif attributed_body is not None:
                # Try to decode attributedBody
                stats["messages_with_attributedBody_only"] += 1
                decoded = decode_attributed_body(attributed_body)
                if decoded and decoded.strip():
                    extracted_text = decoded
                    stats["decoded_from_attributedBody"] += 1
                else:
                    stats["failed_to_decode"] += 1

            if extracted_text and extracted_text.strip():
                messages_by_contact[handle].append(extracted_text)

        # Count group chats that were skipped
        cursor.execute("""
            SELECT COUNT(DISTINCT cmj.chat_id)
            FROM chat_message_join cmj
            JOIN message m ON m.ROWID = cmj.message_id
            JOIN chat_handle_join chj ON chj.chat_id = cmj.chat_id
            WHERE m.is_from_me = 1
            GROUP BY cmj.chat_id
            HAVING COUNT(DISTINCT chj.handle_id) > 1
        """)
        stats["group_chats_skipped"] = sum(1 for _ in cursor.fetchall())

        conn.close()
    except sqlite3.OperationalError as e:
        raise FileNotFoundError(f"Cannot read {db_path}: {e}") from e
    except FileNotFoundError as e:
        raise FileNotFoundError(f"Message database not found at {db_path}") from e

    return messages_by_contact, stats


def classify_contacts(messages_by_contact: Dict[str, List[str]]) -> List[ContactScore]:
    """
    Score each contact's messages and produce classification.
    Returns sorted list: formal/professional contacts first.
    """
    scores: List[ContactScore] = []

    for handle, texts in messages_by_contact.items():
        if not texts:
            continue

        # Compute mean formality and warmth across all messages
        formality_vals = [estimate_formality(t) for t in texts]
        warmth_vals = [estimate_warmth(t) for t in texts]

        formality_avg = sum(formality_vals) / len(formality_vals)
        warmth_avg = sum(warmth_vals) / len(warmth_vals)
        register_avg = (formality_avg + warmth_avg) / 2.0

        score = ContactScore(
            handle=handle,
            handle_display_name=None,  # No easy way to resolve from chat.db
            n_messages=len(texts),
            formality_score=formality_avg,
            warmth_score=warmth_avg,
            register_avg=register_avg,
        )
        scores.append(score)

    # Sort: formal contacts first (higher formality), then by n_messages descending
    scores.sort(key=lambda s: (-s.formality_score, -s.n_messages))

    return scores


def print_proposal_table(scores: List[ContactScore], stats: Dict[str, int]) -> None:
    """
    Print a dry-run proposal table: handle, n_messages, formality, warmth, proposed.
    Privacy: truncate handles to prevent accidental commit of phone numbers.
    """
    print("\n" + "=" * 110)
    print("CONTACT FORMALITY CLASSIFICATION — DRY-RUN PROPOSAL (WITH ATTRIBUTED BODY DECODING)")
    print("=" * 110)

    formal_count = sum(1 for s in scores if s.proposed_formality() == "formal")
    casual_count = sum(1 for s in scores if s.proposed_formality() == "casual")
    insufficient_count = sum(1 for s in scores if s.proposed_formality() == "insufficient")

    print(f"\nData Summary:")
    print(f"  Total outgoing messages: {stats['total_messages_found']}")
    print(f"  Messages with text field: {stats['messages_with_text']}")
    print(f"  Messages with attributedBody only: {stats['messages_with_attributedBody_only']}")
    print(f"  Successfully decoded from attributedBody: {stats['decoded_from_attributedBody']}")
    print(f"  Failed to decode: {stats['failed_to_decode']}")
    print(f"  Total analyzed (text + decoded): {stats['messages_with_text'] + stats['decoded_from_attributedBody']}")
    print(f"  One-to-one chats: {stats['one_to_one_chats']}")
    print(f"  Group chats skipped: {stats['group_chats_skipped']}")

    print(f"\nClassification Summary: {formal_count} formal | {casual_count} casual | "
          f"{insufficient_count} insufficient ({len(scores)} total contacts)\n")

    # Header
    print(f"{'Handle':<25} {'N Msgs':>8} {'Formality':>10} {'Warmth':>10} "
          f"{'Register':>10} {'Proposed':<12} {'Confidence':>10}")
    print("-" * 110)

    # Formal contacts first
    for s in scores:
        if s.proposed_formality() != "formal":
            continue
        print(f"{truncate_handle(s.handle):<25} {s.n_messages:>8d} "
              f"{s.formality_score:>10.3f} {s.warmth_score:>10.3f} "
              f"{s.register_avg:>10.3f} {s.proposed_formality():<12} "
              f"{s.confidence():>10.1%}")

    # Casual contacts
    for s in scores:
        if s.proposed_formality() != "casual":
            continue
        print(f"{truncate_handle(s.handle):<25} {s.n_messages:>8d} "
              f"{s.formality_score:>10.3f} {s.warmth_score:>10.3f} "
              f"{s.register_avg:>10.3f} {s.proposed_formality():<12} "
              f"{s.confidence():>10.1%}")

    # Insufficient data
    for s in scores:
        if s.proposed_formality() != "insufficient":
            continue
        print(f"{truncate_handle(s.handle):<25} {s.n_messages:>8d} "
              f"{s.formality_score:>10.3f} {s.warmth_score:>10.3f} "
              f"{s.register_avg:>10.3f} {s.proposed_formality():<12} "
              f"{s.confidence():>10.1%}")

    print("-" * 110)
    print(f"\nNote: Truncated handles + hashes for privacy. Full data NOT committed to git.")
    print(f"Formality threshold for 'formal' classification: > 0.55")
    print(f"Minimum messages for classification: >= 15")
    print(f"Coverage: {stats['messages_with_text'] + stats['decoded_from_attributedBody']} / {stats['total_messages_found']} messages analyzed")


def main():
    """Main entry point."""
    dry_run = "--dry-run" in sys.argv or "--dry-run" == " ".join(sys.argv[1:])
    apply = "--apply" in sys.argv
    understand_live = "--i-understand-live-persona" in sys.argv

    if apply and not understand_live:
        print("ERROR: --apply requires --i-understand-live-persona")
        print("       This classifier NEVER actually writes to persona files.")
        sys.exit(1)

    # Path to macOS Messages database
    db_path = str(Path.home() / "Library" / "Messages" / "chat.db")

    print(f"Classifier: Contact Formality (objective register metric + attributedBody decoding)")
    print(f"Data source: {db_path}")
    print(f"Mode: {'DRY-RUN (read-only)' if dry_run else 'APPLY (GUARDED — never executes)'}")
    print()

    # Read and classify
    try:
        print("Reading messages from chat.db...")
        messages_by_contact, stats = read_messages_from_db(db_path)
        print(f"Found {stats['one_to_one_chats']} one-to-one chats with {stats['total_messages_found']} outgoing messages")
        print(f"  Text messages: {stats['messages_with_text']}")
        print(f"  Decoded from attributedBody: {stats['decoded_from_attributedBody']}")
        print(f"  Failed to decode: {stats['failed_to_decode']}")

        contacts_classified = sum(1 for msgs in messages_by_contact.values() if msgs)
        print(f"Contacts with analyzable messages: {contacts_classified}")

        print("\nScoring formality and warmth...")
        scores = classify_contacts(messages_by_contact)
        print(f"Classified {len(scores)} contacts with valid data.")

        # Print proposal
        print_proposal_table(scores, stats)

        if apply:
            print("\n" + "=" * 110)
            print("APPLY MODE GUARDED: The --apply flag is recognized but disabled.")
            print("This is a READ-ONLY analysis tool. To seed formal contacts into seth.json,")
            print("review the proposal above and manually edit ~/.human/personas/seth.json")
            print("with warmth_level=\"high\" or relationship_stage=\"close\" for formal contacts.")
            print("=" * 110)
            sys.exit(0)

    except FileNotFoundError as e:
        print(f"\nERROR: {e}")
        print("\nFALLBACK: chat.db is not readable. This may be due to:")
        print("  - macOS TCC (System Preferences > Security & Privacy > Full Disk Access)")
        print("    needs to grant permission to your terminal or Python")
        print("  - The database doesn't exist (no Messages app data yet)")
        print("\nREPORTING PARTIAL: cannot access chat.db.")
        sys.exit(1)


if __name__ == "__main__":
    main()
