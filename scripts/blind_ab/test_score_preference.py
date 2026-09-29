#!/usr/bin/env python3
"""Tests for score_preference.py (US-6 preference measurement).

Hermetic: no network, no real AddressBook, no ~/.human writes -- everything
here operates on tempdir fixtures and score_preference.py's own functions.

Covers:
  - load_preference_key()'s "_mode": "preference" marker enforcement
  - F3: --min-n is operator-overridable for a scoring-only look, but a
    WRITTEN evidence file is hard-floored at max(--min-n, MIN_N=20) --
    `--min-n 1 --evidence-out ...` must still refuse at n=15.
  - the happy path: n >= 20 writes a well-formed evidence file.
"""
import csv
import json
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(__file__))
from score_preference import MIN_N, load_preference_key

SCRIPT = os.path.join(os.path.dirname(__file__), "score_preference.py")
FIELDNAMES = ["id", "context", "option_A", "option_B", "choice", "confidence"]


def _write_sheet(path, n, rater_correct=True):
    """Write a rating_sheet.csv with n answered rows plus a matching
    preference-mode answer_key.json alongside it. Every row's `choice`
    matches (or, if rater_correct=False, mismatches) the key so scoring
    yields a deterministic, well-formed n."""
    key = {}
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDNAMES)
        w.writeheader()
        for i in range(n):
            model_side = "A" if i % 2 == 0 else "B"
            key[f"t{i}"] = model_side
            choice = model_side if rater_correct else ("B" if model_side == "A" else "A")
            w.writerow({"id": f"t{i}", "context": f"ctx {i}",
                        "option_A": f"a{i}", "option_B": f"b{i}",
                        "choice": choice, "confidence": 3})
    key["_mode"] = "preference"
    keyf = os.path.join(os.path.dirname(path), "answer_key.json")
    with open(keyf, "w") as f:
        json.dump(key, f)
    return keyf


class TestLoadPreferenceKey(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def _write_key(self, obj):
        path = os.path.join(self.tmp.name, "answer_key.json")
        with open(path, "w") as f:
            json.dump(obj, f)
        return path

    def test_missing_mode_marker_raises(self):
        path = self._write_key({"t1": "A"})
        with self.assertRaises(ValueError):
            load_preference_key(path)

    def test_detection_mode_marker_raises(self):
        """A detection-mode key (no _mode, or a different _mode) must never
        be silently scored as preference -- designs/US-6.md's conflation
        risk."""
        path = self._write_key({"t1": "A", "_mode": "detection"})
        with self.assertRaises(ValueError):
            load_preference_key(path)

    def test_preference_mode_marker_strips_marker_key(self):
        path = self._write_key({"t1": "A", "t2": "B", "_mode": "preference"})
        key = load_preference_key(path)
        self.assertNotIn("_mode", key)
        self.assertEqual(key, {"t1": "A", "t2": "B"})


class TestCLIEvidenceFloor(unittest.TestCase):
    """subprocess against the real script, mirroring
    test_make_rating_sheet.py's established hermetic CLI pattern."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def _run(self, sheet, keyf, extra_args, evidence_out):
        return subprocess.run(
            [sys.executable, SCRIPT, sheet, "--key", keyf, "--rater", "human",
             "--evidence-out", evidence_out] + extra_args,
            capture_output=True, text=True, timeout=60)

    def test_min_n_1_with_evidence_out_still_refuses_at_n15(self):
        """F3: an operator passing --min-n 1 must NOT be able to make a
        15-scored-pair run commit an evidence file -- the AC-6.5 floor
        (MIN_N=20) is hard for WRITES regardless of --min-n."""
        sheet = os.path.join(self.tmp.name, "rating_sheet_alice.csv")
        keyf = _write_sheet(sheet, n=15)
        evidence_out = os.path.join(self.tmp.name, "evidence.json")
        r = self._run(sheet, keyf, ["--min-n", "1"], evidence_out)
        self.assertNotEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("evidence floor", r.stdout + r.stderr)
        self.assertFalse(os.path.exists(evidence_out),
                          "evidence file must not be written below the floor")

    def test_min_n_1_without_evidence_out_scores_at_n15(self):
        """--min-n still works for its original purpose: a scoring-only
        (no --evidence-out) look at a small sample must succeed."""
        sheet = os.path.join(self.tmp.name, "rating_sheet_alice.csv")
        keyf = _write_sheet(sheet, n=15)
        r = subprocess.run(
            [sys.executable, SCRIPT, sheet, "--key", keyf, "--rater", "human",
             "--min-n", "1"],
            capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("RESULT_blind_ab_preference=SCORED n=15", r.stdout)

    def test_n_at_or_above_floor_writes_evidence(self):
        sheet = os.path.join(self.tmp.name, "rating_sheet_alice.csv")
        keyf = _write_sheet(sheet, n=MIN_N)
        evidence_out = os.path.join(self.tmp.name, "evidence.json")
        r = self._run(sheet, keyf, [], evidence_out)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue(os.path.exists(evidence_out))
        with open(evidence_out) as f:
            evidence = json.load(f)
        self.assertEqual(evidence["n"], MIN_N)
        self.assertEqual(evidence["rater"], "human")
        self.assertIn("win_rate", evidence)

    def test_higher_min_n_raises_the_floor(self):
        """--min-n 25 (above MIN_N) must raise the evidence floor, not
        lower it -- max(min_n, MIN_N)."""
        sheet = os.path.join(self.tmp.name, "rating_sheet_alice.csv")
        keyf = _write_sheet(sheet, n=MIN_N)
        evidence_out = os.path.join(self.tmp.name, "evidence.json")
        r = self._run(sheet, keyf, ["--min-n", "25"], evidence_out)
        self.assertNotEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertFalse(os.path.exists(evidence_out))


class TestSelftestEntryPoint(unittest.TestCase):
    def test_selftest_cli_exits_zero(self):
        r = subprocess.run([sys.executable, SCRIPT, "--selftest"],
                            capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("selftest OK", r.stdout)


if __name__ == "__main__":
    unittest.main()
