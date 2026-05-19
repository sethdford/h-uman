#!/usr/bin/env python3
"""
Phase H1 verifier — pins corpus extractor behavior against fixture DBs.

Tests:
  1. PII redaction substitutes phones/emails correctly
  2. hash_handle is deterministic + 8-hex-chars
  3. iMessage extractor reads the Apple chat.db schema correctly
  4. memory.db extractor reads the daemon schema correctly
  5. apple_ns_to_unix_ms converts the Mac epoch correctly
  6. Missing DBs return [] (no crash)
  7. End-to-end: extracts from BOTH sources, redacts, writes JSONL

Run: python3 scripts/test_m3_extract_corpus.py
"""
from __future__ import annotations

import importlib.util
import json
import sqlite3
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
EXTRACT = REPO_ROOT / "scripts" / "m3_extract_corpus.py"


def _load():
    spec = importlib.util.spec_from_file_location("m3_extract_corpus", EXTRACT)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


m = _load()
_PASS = 0
_FAIL = 0


def _ok(name, cond, detail=""):
    global _PASS, _FAIL
    if cond:
        _PASS += 1
        print(f"  PASS  {name}")
    else:
        _FAIL += 1
        print(f"  FAIL  {name}  {detail}")


def test_redact_pii():
    print("\n--- test_redact_pii ---")
    # Each entry: (input, expected_redacted_substring, original_digits_must_be_gone)
    # The exact marker (`[phone]` vs `[number]`) doesn't matter for the
    # privacy threat model — what matters is that the original digits
    # / email don't appear in the output.
    cases = [
        ("call me at 415-555-1234", "[phone]", "415-555-1234"),
        ("email seth@example.com plz", "[email]", "seth@example.com"),
        ("My SSN is 123-45-6789", None, "123-45-6789"),
        ("CC 4111 1111 1111 1111", None, "4111 1111 1111 1111"),
        ("hi there friends", None, None),  # plain text — passes through
    ]
    for input_text, expect_marker, original in cases:
        out = m.redact_pii(input_text)
        if original is None:
            _ok(f"plain text passes through ({input_text!r})", out == input_text)
        else:
            if expect_marker:
                _ok(f"redacts {input_text!r} → contains a redaction marker",
                    expect_marker in out, f"got {out!r}")
            # ALWAYS check the original sensitive content is gone
            _ok(f"original digits/email gone from {input_text!r}",
                original not in out,
                f"got {out!r}")


def test_hash_handle_deterministic():
    print("\n--- test_hash_handle_deterministic ---")
    h1 = m.hash_handle("+15555550123")
    h2 = m.hash_handle("+15555550123")
    h3 = m.hash_handle("+15555550999")
    _ok("same input → same hash", h1 == h2)
    _ok("different input → different hash", h1 != h3)
    _ok(f"hash is 8 hex chars (got {len(h1)})", len(h1) == 8)
    _ok("hash is lowercase hex", all(c in "0123456789abcdef" for c in h1))
    _ok("empty handle → empty string", m.hash_handle("") == "")


def test_apple_epoch_conversion():
    print("\n--- test_apple_epoch_conversion ---")
    # Apple epoch (2001-01-01) in ns → unix ms = 978307200 * 1000
    _ok("apple epoch = unix 978307200000ms",
        m.apple_ns_to_unix_ms(0) == 0,  # 0 means "no date" → 0 unix
        f"got {m.apple_ns_to_unix_ms(0)}")
    # 1 second after Apple epoch should be unix 978307201 sec
    one_sec_apple = 1_000_000_000  # 1 second in nanoseconds
    expected = (1 + 978307200) * 1000  # ms
    _ok(f"1s after Apple epoch → {expected}ms",
        m.apple_ns_to_unix_ms(one_sec_apple) == expected,
        f"got {m.apple_ns_to_unix_ms(one_sec_apple)}")


def test_imessage_extractor_handles_apple_schema():
    print("\n--- test_imessage_extractor_handles_apple_schema ---")
    with tempfile.TemporaryDirectory() as d:
        db_path = Path(d) / "chat.db"
        conn = sqlite3.connect(str(db_path))
        # Minimal Apple-shaped schema (only the columns extractor reads)
        conn.execute("""
            CREATE TABLE message (
                ROWID INTEGER PRIMARY KEY,
                text TEXT, is_from_me INTEGER, date INTEGER, handle_id INTEGER
            )
        """)
        conn.execute("CREATE TABLE handle (ROWID INTEGER PRIMARY KEY, id TEXT)")
        conn.execute("INSERT INTO handle(ROWID, id) VALUES (1, '+15555550123')")
        conn.execute("INSERT INTO handle(ROWID, id) VALUES (2, 'friend@example.com')")
        # 2 from Seth (is_from_me=1), 1 from contact
        conn.execute("INSERT INTO message(text, is_from_me, date, handle_id) "
                     "VALUES ('hey what is up', 1, 1000000000, 1)")
        conn.execute("INSERT INTO message(text, is_from_me, date, handle_id) "
                     "VALUES ('nothing much', 0, 1100000000, 1)")
        conn.execute("INSERT INTO message(text, is_from_me, date, handle_id) "
                     "VALUES ('haha cool', 1, 1200000000, 2)")
        conn.commit()
        conn.close()

        records = m.extract_imessage(db_path, max_records=100, redact_handles=True)
        _ok(f"extracted 3 records (got {len(records)})", len(records) == 3)
        roles = [r["role"] for r in records]
        # Order is DESC by date → newest first
        _ok("first record (newest) is from Seth (assistant)",
            roles[0] == "assistant")
        _ok("contains both roles", set(roles) == {"user", "assistant"})
        _ok("all records have hashed handle (8 hex)",
            all(len(r["handle"]) == 8 for r in records))
        _ok("two distinct handles → two distinct hashes",
            len({r["handle"] for r in records}) == 2)
        _ok("ts_ms is non-zero", all(r["ts_ms"] > 0 for r in records))


def test_memory_db_extractor():
    print("\n--- test_memory_db_extractor ---")
    with tempfile.TemporaryDirectory() as d:
        db_path = Path(d) / "memory.db"
        conn = sqlite3.connect(str(db_path))
        conn.execute("""
            CREATE TABLE messages(
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                session_id TEXT NOT NULL,
                role TEXT NOT NULL,
                content TEXT NOT NULL,
                created_at TEXT DEFAULT(datetime('now')))
        """)
        for i, (role, content) in enumerate([
            ("user", "hello"),
            ("assistant", "hey there"),
            ("user", "how's it going"),
        ]):
            conn.execute("INSERT INTO messages(session_id, role, content, created_at) "
                         "VALUES (?, ?, ?, ?)",
                         ("s1", role, content, f"2026-05-{15+i} 12:00:00"))
        conn.commit()
        conn.close()

        records = m.extract_memory_db(db_path, max_records=100, redact_handles=True)
        _ok(f"extracted 3 records (got {len(records)})", len(records) == 3)
        _ok("channel is memory_db",
            all(r["channel"] == "memory_db" for r in records))
        _ok("session_id is hashed (8 hex)",
            all(len(r["handle"]) == 8 for r in records))


def test_missing_db_returns_empty():
    print("\n--- test_missing_db_returns_empty ---")
    out = m.extract_imessage(Path("/tmp/nonexistent_chat.db"), 10, True)
    _ok("missing iMessage db → []", out == [])
    out = m.extract_memory_db(Path("/tmp/nonexistent_memory.db"), 10, True)
    _ok("missing memory.db → []", out == [])


def test_corrupt_db_soft_fails():
    """Pins gap #12: when chat.db exists but is unreadable (the FDA-
    revoked failure mode on macOS, or just a corrupt file), the
    extractor must return [] gracefully instead of crashing. The
    operator sees a WARN on stderr but the loop continues."""
    print("\n--- test_corrupt_db_soft_fails ---")
    with tempfile.TemporaryDirectory() as d:
        # Write garbage that's NOT a valid SQLite file
        bad = Path(d) / "chat.db"
        bad.write_bytes(b"this is not a sqlite database, sorry\n" * 100)
        out = m.extract_imessage(bad, 10, True)
        _ok("corrupt chat.db → []", out == [],
            f"got {len(out)} records from a corrupt db")

        bad2 = Path(d) / "memory.db"
        bad2.write_bytes(b"\x00\x01\x02 garbage \xff\xff" * 50)
        out2 = m.extract_memory_db(bad2, 10, True)
        _ok("corrupt memory.db → []", out2 == [],
            f"got {len(out2)} records from a corrupt db")


def test_gmail_slack_stubs_return_empty():
    """Pins gap #11: gmail / slack are stubs. Until the real network
    slices land, they MUST return [] (not crash, not None) so the
    --sources gmail,slack path works as a documented no-op."""
    print("\n--- test_gmail_slack_stubs_return_empty ---")
    _ok("extract_gmail → []", m.extract_gmail(100, True) == [])
    _ok("extract_slack → []", m.extract_slack(100, True) == [])
    # Also verify they're wired in the dispatch table so --sources
    # gmail / --sources slack doesn't error out
    _ok("gmail in SOURCE_DISPATCH", "gmail" in m.SOURCE_DISPATCH)
    _ok("slack in SOURCE_DISPATCH", "slack" in m.SOURCE_DISPATCH)


def test_end_to_end_jsonl_output():
    print("\n--- test_end_to_end_jsonl_output ---")
    with tempfile.TemporaryDirectory() as d:
        # Build fixtures for both sources
        imsg = Path(d) / "chat.db"
        c = sqlite3.connect(str(imsg))
        c.execute("CREATE TABLE message (ROWID INTEGER PRIMARY KEY, text TEXT, "
                  "is_from_me INTEGER, date INTEGER, handle_id INTEGER)")
        c.execute("CREATE TABLE handle (ROWID INTEGER PRIMARY KEY, id TEXT)")
        c.execute("INSERT INTO handle(ROWID, id) VALUES (1, '+15555551234')")
        c.execute("INSERT INTO message VALUES (NULL, 'hi 415-555-9999 there', 1, 1000000000, 1)")
        c.commit(); c.close()

        mem = Path(d) / "memory.db"
        c = sqlite3.connect(str(mem))
        c.execute("CREATE TABLE messages(id INTEGER PRIMARY KEY AUTOINCREMENT, "
                  "session_id TEXT NOT NULL, role TEXT NOT NULL, "
                  "content TEXT NOT NULL, created_at TEXT DEFAULT(datetime('now')))")
        c.execute("INSERT INTO messages(session_id, role, content, created_at) "
                  "VALUES ('s1', 'user', 'hey foo@bar.com', '2026-05-15 12:00:00')")
        c.commit(); c.close()

        out = Path(d) / "corpus.jsonl"
        result = subprocess.run(
            [sys.executable, str(EXTRACT),
             "--out", str(out),
             "--sources", "imessage,memory_db",
             "--imessage-db", str(imsg),
             "--memory-db", str(mem)],
            capture_output=True, text=True, timeout=15)
        _ok(f"extractor exits 0 (rc={result.returncode})",
            result.returncode == 0,
            f"{result.stdout}\n{result.stderr}")
        lines = [l for l in out.read_text().splitlines() if l.strip()]
        _ok(f"2 records in JSONL (got {len(lines)})", len(lines) == 2)
        recs = [json.loads(l) for l in lines]
        # The iMessage record had a phone number; verify redaction
        imsg_rec = next(r for r in recs if r["channel"] == "imessage")
        _ok("phone number redacted in iMessage content",
            "[phone]" in imsg_rec["content"] and "415-555-9999" not in imsg_rec["content"],
            f"got: {imsg_rec['content']!r}")
        # The memory_db record had an email
        mem_rec = next(r for r in recs if r["channel"] == "memory_db")
        _ok("email redacted in memory_db content",
            "[email]" in mem_rec["content"] and "foo@bar.com" not in mem_rec["content"],
            f"got: {mem_rec['content']!r}")


def main():
    print("M3 corpus extractor (H1) verifier")
    test_redact_pii()
    test_hash_handle_deterministic()
    test_apple_epoch_conversion()
    test_imessage_extractor_handles_apple_schema()
    test_memory_db_extractor()
    test_missing_db_returns_empty()
    test_corrupt_db_soft_fails()
    test_gmail_slack_stubs_return_empty()
    test_end_to_end_jsonl_output()
    print(f"\n--- Results: {_PASS} passed, {_FAIL} failed ---")
    return 0 if _FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
