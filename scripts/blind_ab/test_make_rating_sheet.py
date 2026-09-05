#!/usr/bin/env python3
"""Hermetic tests for make_rating_sheet.py's redaction and --mode preference
behavior (US-6, sprints/sprint-better-than-human-2026-09-05/designs/US-6.md).

No network, no real AddressBook read (HU_BLIND_AB_SKIP_ADDRESSBOOK=1 is set
for every subprocess invocation), no ~/.human writes -- redact()/build() unit
tests inject a fixture name list directly rather than resolving contacts.
"""
import csv
import json
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(__file__))
from make_rating_sheet import build, build_name_tokens, redact, write_outputs

SCRIPT = os.path.join(os.path.dirname(__file__), "make_rating_sheet.py")


class TestRedactPhone(unittest.TestCase):
    def test_us_format_dashes_is_redacted(self):
        out = redact("call me 555-123-4567 later")
        self.assertNotIn("555-123-4567", out)
        self.assertIn("[phone]", out)

    def test_us_format_dots_is_redacted(self):
        out = redact("reach me at 555.123.4567")
        self.assertNotIn("555.123.4567", out)
        self.assertIn("[phone]", out)

    def test_parens_format_is_redacted(self):
        out = redact("(555) 123-4567 works")
        self.assertNotIn("123-4567", out)
        self.assertIn("[phone]", out)

    def test_international_format_is_redacted(self):
        out = redact("text +14155551234 tonight")
        self.assertNotIn("+14155551234", out)
        self.assertIn("[phone]", out)

    def test_no_phone_shaped_text_is_unchanged(self):
        out = redact("just a normal sentence with no numbers")
        self.assertEqual(out, "just a normal sentence with no numbers")

    def test_empty_and_none_are_passthrough(self):
        self.assertEqual(redact(""), "")
        self.assertIsNone(redact(None))


class TestRedactName(unittest.TestCase):
    """Fixture name lists only -- never resolve_contact_name_tokens()/the
    real AddressBook, per designs/US-6.md's "keeping the test hermetic"."""

    def test_fixture_name_is_redacted(self):
        out = redact("tell Sarah hi", name_tokens=["Sarah", "Jake"])
        self.assertNotIn("Sarah", out)
        self.assertIn("[name]", out)

    def test_case_insensitive_match(self):
        out = redact("tell sarah hi", name_tokens=["Sarah"])
        self.assertNotIn("sarah", out)
        self.assertIn("[name]", out)

    def test_non_matching_name_untouched(self):
        out = redact("tell Friday hi", name_tokens=["Sarah", "Jake"])
        self.assertEqual(out, "tell Friday hi")

    def test_no_name_tokens_leaves_text_unchanged_besides_phone(self):
        out = redact("tell Sarah hi", name_tokens=None)
        self.assertEqual(out, "tell Sarah hi")

    def test_phone_and_name_redacted_together(self):
        out = redact("Sarah's number is 555-123-4567", name_tokens=["Sarah"])
        self.assertNotIn("Sarah", out)
        self.assertNotIn("555-123-4567", out)
        self.assertIn("[name]", out)
        self.assertIn("[phone]", out)

    def test_build_name_tokens_splits_full_names(self):
        tokens = build_name_tokens(["Sarah Jones", "Jake Q. Smith"])
        self.assertIn("sarah", tokens)
        self.assertIn("jones", tokens)
        self.assertIn("jake", tokens)
        self.assertIn("smith", tokens)

    def test_build_name_tokens_handles_empty_and_none(self):
        self.assertEqual(build_name_tokens(None), set())
        self.assertEqual(build_name_tokens([]), set())
        self.assertEqual(build_name_tokens([""]), set())


class TestBuildDetectionMode(unittest.TestCase):
    """Default mode must be backward-compatible: key[id] holds which side is
    the REAL Seth reply, unchanged from pre-US-6 behavior."""

    def test_default_mode_is_detection(self):
        triples = [{"id": "t1", "context": "c", "seth_reply": "seth",
                    "huuman_reply": "model"}]
        rows, key, skipped = build(triples, seed=1)
        self.assertEqual(skipped, 0)
        self.assertEqual(len(rows), 1)
        row = rows[0]
        seth_side = key["t1"]
        other_side = "B" if seth_side == "A" else "A"
        self.assertEqual(row[f"option_{seth_side}"], "seth")
        self.assertEqual(row[f"option_{other_side}"], "model")

    def test_detection_mode_does_not_skip_identical_replies(self):
        """Duplicate real/model replies are only excluded in preference
        mode -- detection's existing behavior for this edge case is
        untouched (scope: designs/US-6.md 'Approach')."""
        triples = [{"id": "t1", "context": "c", "seth_reply": "same",
                    "huuman_reply": "same"}]
        rows, key, skipped = build(triples, seed=1, mode="detection")
        self.assertEqual(skipped, 0)
        self.assertEqual(len(rows), 1)

    def test_detection_mode_applies_redaction(self):
        triples = [{"id": "t1", "context": "call 555-123-4567", "seth_reply": "seth",
                    "huuman_reply": "model"}]
        rows, key, skipped = build(triples, seed=1, mode="detection")
        self.assertNotIn("555-123-4567", rows[0]["context"])
        self.assertIn("[phone]", rows[0]["context"])


class TestBuildPreferenceMode(unittest.TestCase):
    def test_key_value_is_the_model_side(self):
        triples = [{"id": "t1", "context": "c", "seth_reply": "seth reply",
                    "huuman_reply": "model reply"}]
        rows, key, skipped = build(triples, seed=1, mode="preference")
        self.assertEqual(skipped, 0)
        self.assertEqual(len(rows), 1)
        row = rows[0]
        model_side = key["t1"]
        seth_side = "B" if model_side == "A" else "A"
        self.assertEqual(row[f"option_{model_side}"], "model reply")
        self.assertEqual(row[f"option_{seth_side}"], "seth reply")
        self.assertIn(model_side, ("A", "B"))

    def test_real_real_duplicate_is_skipped_and_counted(self):
        triples = [
            {"id": "t1", "context": "c1", "seth_reply": "unique reply",
             "huuman_reply": "different model reply"},
            {"id": "t2", "context": "c2", "seth_reply": "identical text",
             "huuman_reply": "identical text"},
        ]
        rows, key, skipped = build(triples, seed=1, mode="preference")
        self.assertEqual(skipped, 1)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["id"], "t1")
        self.assertNotIn("t2", key)

    def test_duplicate_detected_after_redaction_not_before(self):
        """Two replies that differ only in a phone number must still be
        treated as a real/model duplicate once redaction makes them
        identical -- redaction happens before the comparison."""
        triples = [{"id": "t1", "context": "c",
                    "seth_reply": "call 555-123-4567",
                    "huuman_reply": "call 555-999-8888"}]
        rows, key, skipped = build(triples, seed=1, mode="preference")
        self.assertEqual(skipped, 1)
        self.assertEqual(len(rows), 0)

    def test_all_duplicates_yields_zero_rows(self):
        triples = [{"id": f"t{i}", "context": "c", "seth_reply": "same",
                    "huuman_reply": "same"} for i in range(5)]
        rows, key, skipped = build(triples, seed=1, mode="preference")
        self.assertEqual(skipped, 5)
        self.assertEqual(rows, [])
        self.assertEqual(key, {})

    def test_invalid_mode_raises(self):
        with self.assertRaises(ValueError):
            build([{"id": "t1", "context": "c", "seth_reply": "a", "huuman_reply": "b"}],
                  seed=1, mode="bogus")


class TestWriteOutputsModeMarker(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def test_preference_key_carries_mode_marker(self):
        triples = [{"id": "t1", "context": "c", "seth_reply": "seth",
                    "huuman_reply": "model"}]
        rows, key, _ = build(triples, seed=1, mode="preference")
        sheet, keyf = write_outputs(rows, key, "preference", self.tmp.name)
        with open(keyf) as f:
            written = json.load(f)
        self.assertEqual(written["_mode"], "preference")
        self.assertIn(written["t1"], ("A", "B"))

    def test_detection_key_has_no_mode_marker(self):
        triples = [{"id": "t1", "context": "c", "seth_reply": "seth",
                    "huuman_reply": "model"}]
        rows, key, _ = build(triples, seed=1, mode="detection")
        sheet, keyf = write_outputs(rows, key, "detection", self.tmp.name)
        with open(keyf) as f:
            written = json.load(f)
        self.assertNotIn("_mode", written)
        self.assertIn(written["t1"], ("A", "B"))

    def test_csv_has_no_raw_phone_or_name(self):
        triples = [{"id": "t1", "context": "call 555-123-4567",
                    "seth_reply": "tell Sarah I said hi",
                    "huuman_reply": "model reply, no PII here"}]
        rows, key, _ = build(triples, seed=1, mode="preference",
                             name_tokens=["Sarah"])
        sheet, keyf = write_outputs(rows, key, "preference", self.tmp.name)
        with open(sheet) as f:
            content = f.read()
        self.assertNotIn("555-123-4567", content)
        self.assertNotIn("Sarah", content)


class TestCLIEndToEnd(unittest.TestCase):
    """subprocess against the real script, mirroring test_score.py's
    established hermetic pattern. HU_BLIND_AB_SKIP_ADDRESSBOOK=1 keeps this
    from ever touching the real macOS AddressBook."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.env = dict(os.environ, HU_BLIND_AB_SKIP_ADDRESSBOOK="1")

    def _write_triples(self, triples):
        path = os.path.join(self.tmp.name, "triples.json")
        with open(path, "w") as f:
            json.dump(triples, f)
        return path

    def test_mode_preference_cli_writes_marked_key(self):
        triples = [{"id": f"t{i}", "context": f"ctx {i}",
                    "seth_reply": f"seth {i}", "huuman_reply": f"model {i}"}
                   for i in range(5)]
        tpath = self._write_triples(triples)
        out_dir = os.path.join(self.tmp.name, "out")
        r = subprocess.run(
            [sys.executable, SCRIPT, tpath, "--mode", "preference", "--out-dir", out_dir],
            capture_output=True, text=True, env=self.env, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        keyf = os.path.join(out_dir, "answer_key.json")
        self.assertTrue(os.path.exists(keyf))
        with open(keyf) as f:
            key = json.load(f)
        self.assertEqual(key["_mode"], "preference")
        for i in range(5):
            self.assertIn(key[f"t{i}"], ("A", "B"))

    def test_all_duplicate_pairs_refuses_and_writes_nothing(self):
        triples = [{"id": f"t{i}", "context": "c", "seth_reply": "same",
                    "huuman_reply": "same"} for i in range(3)]
        tpath = self._write_triples(triples)
        out_dir = os.path.join(self.tmp.name, "out2")
        r = subprocess.run(
            [sys.executable, SCRIPT, tpath, "--mode", "preference", "--out-dir", out_dir],
            capture_output=True, text=True, env=self.env, timeout=60)
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertFalse(os.path.exists(os.path.join(out_dir, "rating_sheet.csv")))
        self.assertFalse(os.path.exists(os.path.join(out_dir, "answer_key.json")))

    def test_detection_mode_default_still_works(self):
        """Backward-compat smoke test: no --mode flag behaves as before."""
        triples = [{"id": "t1", "context": "c", "seth_reply": "seth",
                    "huuman_reply": "model"}]
        tpath = self._write_triples(triples)
        out_dir = os.path.join(self.tmp.name, "out3")
        r = subprocess.run(
            [sys.executable, SCRIPT, tpath, "--out-dir", out_dir],
            capture_output=True, text=True, env=self.env, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        with open(os.path.join(out_dir, "answer_key.json")) as f:
            key = json.load(f)
        self.assertNotIn("_mode", key)
        self.assertIn(key["t1"], ("A", "B"))


if __name__ == "__main__":
    unittest.main()
