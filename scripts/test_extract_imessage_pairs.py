#!/usr/bin/env python3
"""Unit tests for attributedBody decoding in extract_imessage_pairs.py.

Regression tests for the length-prefix decode bug that corrupted ~20% of
`seth_reply` values in data/imessage/ground_truth.jsonl.

Background
----------
`attributedBody` is a NeXTSTEP *typedstream* archive (NSArchiver), not an
NSKeyedArchiver plist. An NSString payload is laid out as::

    "NSString" 01 94 84 01 2b <LEN> <utf-8 text bytes...>
                            ^^     ^^^^^
                            '+'    length prefix

`<LEN>` is either a single byte, or `0x81` followed by a little-endian uint16
when the byte length is >= 128.

The old decoder anchored on `+` and started reading text at `+ 1` — landing
on the length byte itself — then looked for a `\\x86` byte to find the end.
That produced two distinct defects:

1. **Leading-byte corruption.** The length byte was emitted as text whenever
   it happened to be printable (0x20-0x7E, i.e. byte lengths 32-126), giving
   replies like ``",I don't know..."`` (0x2c = 44 = the message's own byte
   length). Lengths below 0x20 were control characters that a downstream
   `re.sub` stripped, so those messages decoded correctly *by accident* —
   which is why only a fraction of rows looked broken.
2. **Mid-character truncation.** `\\x86` is a legal UTF-8 *continuation*
   byte (continuations span 0x80-0xBF), so any message containing a
   character encoded with 0x86 — e.g. "\U0001f606" (f0 9f 98 86) or
   "↩" (e2 86 a9), which opens every reply-quote — was truncated
   mid-character and usually dropped entirely.

Both are fixed by trusting the length prefix and slicing exactly.

Fixtures are synthetic, built by `_make_blob` to the byte layout verified
against ~/Library/Messages/chat.db (where `+` sat exactly 4 bytes past the
`NSString` marker in 2982/2982 rows). They are synthetic on purpose: this
repository is public, the defect is purely structural — it depends only on the
length byte's value and on 0x86 appearing in the payload, never on what a
message says — and every other decoder test in this tree builds its blobs the
same way (see scripts/test_m3_extract_corpus.py).
"""
import io
import os
import re
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))
from extract_imessage_pairs import (
    DECODE_FAILURE_BUDGET,
    extract_text_from_attributed_body,
    report_decode_failures,
)


def _make_blob(text, trailing_space=True):
    """Build a typedstream blob carrying ``text``, as macOS lays it out.

    Mirrors the real byte layout: the class-name preamble, the ``+`` marker 4
    bytes past the ``NSString`` marker, a correct length prefix (single byte,
    or ``0x81`` + uint16 LE at >= 128 bytes), the UTF-8 payload, then the
    ``0x86`` element terminator and trailing attribute metadata.

    Real payloads carry a trailing space that the declared length includes and
    the decoder strips, so reproduce that by default.
    """
    payload = text.encode("utf-8") + (b" " if trailing_space else b"")
    n = len(payload)
    length = bytes([n]) if n < 0x80 else b"\x81" + n.to_bytes(2, "little")
    return (
        b"\x04\x0bstreamtyped\x81\xe8\x03\x84\x01@"
        b"\x84\x84\x84\x12NSAttributedString\x00"
        b"\x84\x84\x08NSObject\x00\x85\x92\x84\x84\x84\x08"
        b"NSString\x01\x94\x84\x01"          # '+' lands 4 bytes past "NSString"
        b"+" + length + payload +
        b"\x86\x84\x02iI\x01"
        b"\x92\x84\x84\x84\x0cNSDictionary\x00\x94\x84\x01i\x01"
        b"\x86\x86\x86"
    )


def _length_byte(blob):
    """The raw length prefix byte — what the old decoder leaked as text."""
    return blob[blob.find(b"+", blob.find(b"NSString")) + 1]


# --- Fixtures --------------------------------------------------------------
# Byte lengths land in 0x20-0x7E, so the length byte is PRINTABLE and the old
# decoder prepended it verbatim. Tests assert that range rather than hardcoding
# byte counts, so editing a string cannot silently stop exercising the bug.
TEXT_LEAK_A = "We grabbed lunch at the new place"
TEXT_LEAK_B = "Should we book tickets for the weekend?"
TEXT_LEAK_C = "Finished the errands so I guess that helps"

# Byte length < 0x20, so the length byte is a CONTROL character that the old
# decoder's `re.sub(r"^[\x00-\x1f]+", "")` stripped — these rows passed by
# accident. Pinned so the fix does not regress the accidental-pass case.
TEXT_CONTROL_LEN = "You too"

# Byte length >= 128, exercising the 0x81 + uint16 LE long form.
TEXT_UINT16_LEN = (
    "Hopefully the move is going well, tennis is something to look forward "
    "to. Overall it sounds like things are starting to come together there!"
)

# Contains 0x86 inside a multi-byte character (f0 9f 98 86), so the old
# decoder's terminator search truncated mid-character and dropped the row.
TEXT_EMOJI_WITH_86 = "\U0001f606"

# Both defects at once: a printable length byte AND an embedded 0x86. The "↩"
# that opens every reply-quote is e2 86 a9, so this is the real-world shape.
TEXT_BOTH_DEFECTS = "↩ that was the plan, we can still make it work"


ALL_FIXTURES = [
    ("printable len (leak) A", _make_blob(TEXT_LEAK_A), TEXT_LEAK_A),
    ("printable len (leak) B", _make_blob(TEXT_LEAK_B), TEXT_LEAK_B),
    ("printable len (leak) C", _make_blob(TEXT_LEAK_C), TEXT_LEAK_C),
    ("control len (was OK)", _make_blob(TEXT_CONTROL_LEN), TEXT_CONTROL_LEN),
    ("uint16 len 0x81", _make_blob(TEXT_UINT16_LEN), TEXT_UINT16_LEN),
    ("embedded 0x86", _make_blob(TEXT_EMOJI_WITH_86), TEXT_EMOJI_WITH_86),
    ("printable len + 0x86", _make_blob(TEXT_BOTH_DEFECTS), TEXT_BOTH_DEFECTS),
]

# The conservative detector used to survey the corrupted corpus. It matches
# punctuation-then-word, digit-then-capital, and two-capitals-then-lower.
CORRUPT_PREFIX_RE = re.compile(r"^[^\w\s]\w|^\d[A-Z]|^[A-Z][A-Z][a-z]")


class TestFixtureSanity(unittest.TestCase):
    """The fixtures must actually exercise the branches they claim to."""

    def test_leak_fixtures_have_printable_length_bytes(self):
        """If these fall outside 0x20-0x7E they stop testing the leak."""
        for text in (TEXT_LEAK_A, TEXT_LEAK_B, TEXT_LEAK_C):
            with self.subTest(text=text[:24]):
                lb = _length_byte(_make_blob(text))
                self.assertTrue(
                    0x20 <= lb <= 0x7E,
                    "length byte 0x%02x is not printable — fixture no longer "
                    "reproduces the leak" % lb,
                )

    def test_control_fixture_has_control_length_byte(self):
        self.assertLess(_length_byte(_make_blob(TEXT_CONTROL_LEN)), 0x20)

    def test_uint16_fixture_uses_the_long_form(self):
        self.assertEqual(_length_byte(_make_blob(TEXT_UINT16_LEN)), 0x81)

    def test_0x86_fixtures_really_embed_that_byte(self):
        for text in (TEXT_EMOJI_WITH_86, TEXT_BOTH_DEFECTS):
            with self.subTest(text=repr(text[:12])):
                self.assertIn(b"\x86", text.encode("utf-8"))

    def test_plus_marker_sits_four_bytes_past_nsstring(self):
        """Matches all 2982 real chat.db rows surveyed."""
        blob = _make_blob(TEXT_LEAK_A)
        marker = blob.find(b"NSString")
        self.assertEqual(blob.find(b"+", marker) - (marker + len("NSString")), 4)


class TestAttributedBodyDecode(unittest.TestCase):
    """The decoded text must be exactly the message, with no length byte."""

    def test_printable_length_byte_is_not_emitted_as_text(self):
        for text in (TEXT_LEAK_A, TEXT_LEAK_B, TEXT_LEAK_C):
            with self.subTest(text=text[:24]):
                blob = _make_blob(text)
                leaked = chr(_length_byte(blob))
                got = extract_text_from_attributed_body(blob)
                self.assertEqual(got, text)
                self.assertFalse(
                    got.startswith(leaked),
                    "leaked the length byte %r" % leaked,
                )

    def test_control_length_byte_still_decodes(self):
        """Regression guard for rows that used to pass by accident."""
        self.assertEqual(
            extract_text_from_attributed_body(_make_blob(TEXT_CONTROL_LEN)),
            TEXT_CONTROL_LEN,
        )

    def test_uint16_length_prefix_decodes_full_message(self):
        self.assertEqual(
            extract_text_from_attributed_body(_make_blob(TEXT_UINT16_LEN)),
            TEXT_UINT16_LEN,
        )

    def test_embedded_0x86_is_not_truncated(self):
        """0x86 is a UTF-8 continuation byte, not a safe terminator."""
        self.assertEqual(
            extract_text_from_attributed_body(_make_blob(TEXT_EMOJI_WITH_86)),
            TEXT_EMOJI_WITH_86,
        )

    def test_both_defects_together_decode_cleanly(self):
        got = extract_text_from_attributed_body(_make_blob(TEXT_BOTH_DEFECTS))
        self.assertEqual(got, TEXT_BOTH_DEFECTS)
        self.assertTrue(got.startswith("↩"))

    def test_message_without_trailing_space_decodes(self):
        """Not every payload carries the trailing space."""
        self.assertEqual(
            extract_text_from_attributed_body(
                _make_blob(TEXT_LEAK_A, trailing_space=False)
            ),
            TEXT_LEAK_A,
        )

    def test_no_fixture_trips_the_corruption_detector(self):
        for label, blob, _ in ALL_FIXTURES:
            with self.subTest(fixture=label):
                got = extract_text_from_attributed_body(blob)
                self.assertIsNotNone(got, "%s decoded to None" % label)
                self.assertIsNone(
                    CORRUPT_PREFIX_RE.match(got),
                    "%s decoded with a stray prefix: %r" % (label, got),
                )

    def test_decoded_bytes_never_exceed_declared_length(self):
        """The invariant the bug violated: it returned length+1 bytes."""
        for label, blob, expected in ALL_FIXTURES:
            with self.subTest(fixture=label):
                lb = _length_byte(blob)
                plus = blob.find(b"+", blob.find(b"NSString"))
                declared = (
                    int.from_bytes(blob[plus + 2 : plus + 4], "little")
                    if lb == 0x81
                    else lb
                )
                got = extract_text_from_attributed_body(blob)
                self.assertLessEqual(len(got.encode("utf-8")), declared)
                self.assertEqual(got, expected)


class TestDecodeRobustness(unittest.TestCase):
    """Malformed input must return None rather than raise or emit garbage."""

    def test_none_blob_returns_none(self):
        self.assertIsNone(extract_text_from_attributed_body(None))

    def test_empty_blob_returns_none(self):
        self.assertIsNone(extract_text_from_attributed_body(b""))

    def test_blob_without_nsstring_marker_returns_none(self):
        self.assertIsNone(extract_text_from_attributed_body(b"\x04\x0bnope" * 4))

    def test_truncated_blob_returns_none_not_garbage(self):
        """A length prefix longer than the remaining bytes must fail safe."""
        blob = _make_blob(TEXT_UINT16_LEN)
        truncated = blob[: blob.find(b"+") + 8]
        self.assertIsNone(extract_text_from_attributed_body(truncated))


class TestDecodeFailureTripwire(unittest.TestCase):
    """The old decoder dropped 124/2982 rows silently. This must not recur.

    Assertions are on the return value, not just on printed output, so the
    tripwire is proven to actually gate the run.
    """

    def _run(self, recovered, failed):
        buf = io.StringIO()
        healthy = report_decode_failures(recovered, failed, out=buf)
        return healthy, buf.getvalue()

    def test_clean_run_is_healthy_and_reports_counts(self):
        healthy, out = self._run(2982, 0)
        self.assertTrue(healthy)
        self.assertIn("2982", out)
        self.assertIn("0.00%", out)

    def test_failures_within_budget_warn_but_do_not_fail(self):
        """0.1% failure: reported and warned, run still proceeds."""
        healthy, out = self._run(2979, 3)
        self.assertTrue(healthy)
        self.assertIn("WARNING", out)
        self.assertIn("3 attributedBody rows failed", out)

    def test_failure_rate_above_budget_fails_the_run(self):
        """The historical 124/2982 (4.16%) regression must now be caught."""
        healthy, out = self._run(2858, 124)
        self.assertFalse(healthy)
        self.assertIn("REGRESSION", out)
        self.assertIn("4.16%", out)

    def test_budget_boundary_is_not_itself_a_failure(self):
        """Exactly at budget is healthy; just above it is not."""
        total = 10000
        at = int(total * DECODE_FAILURE_BUDGET)
        healthy_at, _ = self._run(total - at, at)
        healthy_over, _ = self._run(total - at - 1, at + 1)
        self.assertTrue(healthy_at)
        self.assertFalse(healthy_over)

    def test_no_attributed_body_rows_is_healthy(self):
        healthy, _ = self._run(0, 0)
        self.assertTrue(healthy)


if __name__ == "__main__":
    unittest.main()
