#!/usr/bin/env python3
"""Tests for rating_ingest.py

Tests the core ingest logic without requiring a real chat.db or rating sheet.
Uses synthetic fixtures and temporary files.
"""
import csv
import json
import os
import sqlite3
import tempfile
import unittest
from pathlib import Path

# Import the code under test
import rating_ingest as ingest


class TestAttributedBodyDecoder(unittest.TestCase):
    """Test the NSAttributedString decoder."""

    def test_decode_simple_string(self):
        """Decode a simple NSString in attributedBody format."""
        from imessage_text import decode_attributed_body

        # Synthetic NSString format: marker + class version + length + text
        # NSString marker + 5 bytes (class version) + 1-byte length + text
        text_bytes = b"Hello"
        blob = b"NSString" + b"\x00\x00\x00\x00\x00" + bytes([len(text_bytes)]) + text_bytes
        result = decode_attributed_body(blob)
        self.assertEqual(result, "Hello")

    def test_decode_utf8_emoji(self):
        """Decode UTF-8 text with emoji in attributedBody."""
        from imessage_text import decode_attributed_body

        text_bytes = "Hi 👋".encode("utf-8")
        blob = b"NSString" + b"\x00\x00\x00\x00\x00" + bytes([len(text_bytes)]) + text_bytes
        result = decode_attributed_body(blob)
        self.assertEqual(result, "Hi 👋")

    def test_decode_with_uint16_length(self):
        """Decode when length is specified as uint16 LE (0x81 marker)."""
        from imessage_text import decode_attributed_body

        text_bytes = b"A" * 200  # Length too large for single byte
        blob = (
            b"NSString"
            + b"\x00\x00\x00\x00\x00"
            + bytes([0x81])
            + len(text_bytes).to_bytes(2, "little")
            + text_bytes
        )
        result = decode_attributed_body(blob)
        self.assertEqual(result, "A" * 200)

    def test_decode_none_on_invalid(self):
        """Return None on invalid blob."""
        from imessage_text import decode_attributed_body

        self.assertIsNone(decode_attributed_body(None))
        self.assertIsNone(decode_attributed_body(b""))
        self.assertIsNone(decode_attributed_body(b"garbage"))


class TestAnswerParsing(unittest.TestCase):
    """Test the A/B answer parser."""

    def test_parse_simple_a(self):
        choice, conf = ingest.parse_answer("A")
        self.assertEqual(choice, "A")
        self.assertEqual(conf, 3)  # Default confidence

    def test_parse_simple_b(self):
        choice, conf = ingest.parse_answer("B")
        self.assertEqual(choice, "B")
        self.assertEqual(conf, 3)

    def test_parse_with_confidence(self):
        choice, conf = ingest.parse_answer("A 5")
        self.assertEqual(choice, "A")
        self.assertEqual(conf, 5)

    def test_parse_lowercase(self):
        choice, conf = ingest.parse_answer("b")
        self.assertEqual(choice, "B")
        self.assertEqual(conf, 3)

    def test_parse_with_punc(self):
        choice, conf = ingest.parse_answer("A)")
        self.assertEqual(choice, "A")
        self.assertEqual(conf, 3)

    def test_parse_reject_prose(self):
        self.assertIsNone(ingest.parse_answer("maybe A?"))
        self.assertIsNone(ingest.parse_answer("A or B"))

    def test_parse_reject_too_long(self):
        self.assertIsNone(ingest.parse_answer("A " * 100))

    def test_parse_reject_invalid(self):
        self.assertIsNone(ingest.parse_answer("C"))
        self.assertIsNone(ingest.parse_answer(""))


class TestAppleTimestamp(unittest.TestCase):
    """Test Apple timestamp conversion."""

    def test_apple_to_unix(self):
        # Known reference: 2001-01-01 00:00:00 UTC is the Apple epoch
        apple_ns = 0  # Time 0 in Apple nanoseconds = 2001-01-01 00:00:00
        unix = ingest.apple_ts_to_unix(apple_ns)
        self.assertEqual(unix, 978307200)  # APPLE_EPOCH in seconds

    def test_apple_to_unix_one_second_later(self):
        apple_ns = 1e9  # 1 second in nanoseconds
        unix = ingest.apple_ts_to_unix(apple_ns)
        self.assertEqual(unix, 978307200 + 1)


class TestFirstAnswerAfter(unittest.TestCase):
    """Test finding the first answer after a given timestamp."""

    def test_finds_earliest_answer(self):
        """Earliest answer wins (descending order, last match is first in time)."""
        rows = [
            (1000, None, b"", 1e9 * 2),  # message at time +2 seconds (newest)
            (1001, "A", None, 1e9 * 1),  # answer at time +1 second
            (1002, "B", None, 1e9 * 0),  # answer at time 0 (oldest)
        ]
        # Look for answers after time 0.5 seconds
        since_unix = ingest.apple_ts_to_unix(0.5e9)
        result = ingest.first_answer_after(rows, since_unix)
        # Should get the earliest answer after 0.5s, which is "A" at 1s
        self.assertIsNotNone(result)
        rowid, choice, conf = result
        self.assertEqual(choice, "A")

    def test_no_answer_found(self):
        """Return None if no A/B answer is found."""
        rows = [
            (1000, "hello world", None, 1e9 * 1),  # Not A/B-shaped
            (1001, "maybe A?", None, 1e9 * 0),  # Prose, not strict A/B
        ]
        since_unix = ingest.apple_ts_to_unix(0)
        result = ingest.first_answer_after(rows, since_unix)
        self.assertIsNone(result)

    def test_stops_at_before_timestamp(self):
        """Stop iterating once we reach messages before the timestamp."""
        rows = [
            (1000, "A", None, 1e9 * 3),  # After (included)
            (1001, "B", None, 1e9 * 2),  # After (included)
            (1002, "A", None, 1e9 * 1),  # Before (stop here)
        ]
        since_unix = ingest.apple_ts_to_unix(1.5e9)
        result = ingest.first_answer_after(rows, since_unix)
        # Should find "B" at time 2s (only one after 1.5s)
        self.assertIsNotNone(result)
        rowid, choice, conf = result
        self.assertEqual(choice, "B")


class TestChatDBFixture(unittest.TestCase):
    """Test with a synthetic chat.db."""

    def setUp(self):
        """Create a temporary SQLite DB with the minimal chat schema."""
        self.temp_dir = tempfile.mkdtemp()
        self.db_path = os.path.join(self.temp_dir, "test_chat.db")

        # Create minimal chat.db schema
        con = sqlite3.connect(self.db_path)
        cur = con.cursor()
        cur.execute(
            "CREATE TABLE chat (ROWID INTEGER PRIMARY KEY, chat_identifier TEXT)"
        )
        cur.execute(
            "CREATE TABLE handle (ROWID INTEGER PRIMARY KEY, id TEXT)"
        )
        cur.execute(
            """CREATE TABLE message (
                ROWID INTEGER PRIMARY KEY,
                text TEXT,
                attributedBody BLOB,
                date INTEGER,
                is_from_me INTEGER,
                handle_id INTEGER
            )"""
        )
        cur.execute(
            "CREATE TABLE chat_message_join (chat_id INTEGER, message_id INTEGER)"
        )

        # Insert test chat
        cur.execute(
            "INSERT INTO chat (chat_identifier) VALUES (?)",
            ("sethford@me.com",),
        )
        chat_id = cur.lastrowid

        # Insert test messages
        base_time = 100 * 1e9  # Arbitrary base time in Apple nanoseconds

        # Message 1: drip sends a question (is_from_me=1, long text)
        drip_question = "[h-uman rating 1/12] which sounds more like you?\nA) hello\nB) hi"
        cur.execute(
            "INSERT INTO message (text, date, is_from_me) VALUES (?, ?, ?)",
            (drip_question, int(base_time), 1),
        )
        msg1_id = cur.lastrowid
        cur.execute(
            "INSERT INTO chat_message_join VALUES (?, ?)", (chat_id, msg1_id)
        )

        # Message 2: Seth replies "A" (is_from_me=1, short text)
        cur.execute(
            "INSERT INTO message (text, date, is_from_me) VALUES (?, ?, ?)",
            ("A", int(base_time + 1e9), 1),  # +1 second
        )
        msg2_id = cur.lastrowid
        cur.execute(
            "INSERT INTO chat_message_join VALUES (?, ?)", (chat_id, msg2_id)
        )

        # Message 3: Another message (not an answer)
        cur.execute(
            "INSERT INTO message (text, date, is_from_me) VALUES (?, ?, ?)",
            ("just a note", int(base_time + 2e9), 1),  # +2 seconds
        )
        msg3_id = cur.lastrowid
        cur.execute(
            "INSERT INTO chat_message_join VALUES (?, ?)", (chat_id, msg3_id)
        )

        con.commit()
        con.close()

    def tearDown(self):
        """Clean up temp directory."""
        import shutil
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def test_query_chat_replies(self):
        """Query the synthetic chat.db for Seth's replies."""
        since_unix = ingest.apple_ts_to_unix(100e9)  # Just after the drip question
        rows = ingest.query_chat_replies("sethford@me.com", since_unix, self.db_path)
        self.assertGreater(len(rows), 0)
        # Should include the "A" reply
        texts = [r[1] for r in rows]
        self.assertIn("A", texts)

    def test_ingest_answer_from_synthetic_db(self):
        """Test ingest_answer against the synthetic chat.db."""
        since_unix = ingest.apple_ts_to_unix(100e9)  # Just after the drip question
        result = ingest.ingest_answer("sethford@me.com", since_unix, [], self.db_path)
        self.assertIsNotNone(result)
        rowid, choice, conf = result
        self.assertEqual(choice, "A")
        self.assertEqual(conf, 3)  # Default confidence


class TestRatingSheet(unittest.TestCase):
    """Test rating sheet operations."""

    def setUp(self):
        """Create temp rating sheet."""
        self.temp_dir = tempfile.mkdtemp()
        self.sheet_path = os.path.join(self.temp_dir, "rating_sheet.csv")

        # Create a minimal rating sheet
        with open(self.sheet_path, "w", newline="") as f:
            w = csv.DictWriter(
                f,
                fieldnames=[
                    "id",
                    "context",
                    "option_A",
                    "option_B",
                    "choice",
                    "confidence",
                ],
            )
            w.writeheader()
            w.writerows(
                [
                    {
                        "id": "t001",
                        "context": "hi",
                        "option_A": "hello",
                        "option_B": "hey",
                        "choice": "",
                        "confidence": "",
                    },
                    {
                        "id": "t002",
                        "context": "how are you?",
                        "option_A": "good",
                        "option_B": "fine",
                        "choice": "",
                        "confidence": "",
                    },
                ]
            )

    def tearDown(self):
        """Clean up temp directory."""
        import shutil
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def test_write_choice(self):
        """Test writing a choice to the rating sheet."""
        result = ingest.write_choice(self.sheet_path, "t001", "A", 5)
        self.assertTrue(result)

        # Verify the sheet was updated
        rows, _ = ingest.load_sheet(self.sheet_path)
        t001 = next(r for r in rows if r["id"] == "t001")
        self.assertEqual(t001["choice"], "A")
        self.assertEqual(t001["confidence"], "5")

    def test_write_choice_not_found(self):
        """Return False if the row ID doesn't exist."""
        result = ingest.write_choice(self.sheet_path, "t999", "A", 5)
        self.assertFalse(result)

    def test_load_sheet(self):
        """Test loading a rating sheet."""
        rows, fields = ingest.load_sheet(self.sheet_path)
        self.assertEqual(len(rows), 2)
        self.assertIn("id", fields)
        self.assertIn("choice", fields)


class TestState(unittest.TestCase):
    """Test state file operations."""

    def setUp(self):
        """Create temp state directory."""
        self.temp_dir = tempfile.mkdtemp()
        # Patch the STATE path
        self.orig_state = ingest.STATE
        ingest.STATE = os.path.join(self.temp_dir, "drip_state.json")

    def tearDown(self):
        """Restore original STATE path."""
        ingest.STATE = self.orig_state
        import shutil
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def test_save_and_load_state(self):
        """Test state persistence."""
        st = {"pending_row": "t001", "question_unix": 12345, "ingested_rowids": [1, 2, 3]}
        ingest.save_state(st)

        loaded = ingest.load_state()
        self.assertEqual(loaded["pending_row"], "t001")
        self.assertEqual(loaded["question_unix"], 12345)
        self.assertEqual(loaded["ingested_rowids"], [1, 2, 3])


if __name__ == "__main__":
    unittest.main()
