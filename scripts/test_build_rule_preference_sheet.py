"""Hermetic tests for build_rule_preference_sheet: the pure row builder and the
outputs' shapes. No chat.db, no :8741, no `human` binary."""
import csv
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_rule_preference_sheet as brs  # noqa: E402


def _items():
    return [
        {"id": "r14-01", "rule": "r14", "context": "ugh worst day", "off": "damn that's rough",
         "live": "what happened"},
        {"id": "r15-01", "rule": "r15", "context": "should I take the job? " * 4,
         "off": "yeah — sounds like a good move", "live": "take it"},
        {"id": "r15-02", "rule": "r15", "context": "x" * 160, "off": "same", "live": "same"},
        {"id": "r15-03", "rule": "r15", "context": "y" * 160, "off": "", "live": "nope"},
    ]


class BuildRows(unittest.TestCase):
    def test_identical_or_empty_pairs_are_skipped_and_counted(self):
        rows, key, skipped = brs.build_rows(_items(), seed=1)
        self.assertEqual([r["id"] for r in rows], ["r14-01", "r15-01"])
        self.assertEqual(skipped, 2)
        self.assertEqual(key["_mode"], "preference")
        self.assertEqual(set(key) - {"_mode"}, {"r14-01", "r15-01"})

    def test_key_names_the_side_holding_the_live_reply(self):
        rows, key, _ = brs.build_rows(_items(), seed=7)
        for r in rows:
            live = [it for it in _items() if it["id"] == r["id"]][0]["live"]
            self.assertEqual(r["option_" + key[r["id"]]], live)
            self.assertIn(key[r["id"]], "AB")
            self.assertEqual(r["choice"], "")
            self.assertEqual(r["confidence"], "")

    def test_side_assignment_is_seeded_and_varies(self):
        a = brs.build_rows(_items(), seed=3)[1]
        b = brs.build_rows(_items(), seed=3)[1]
        self.assertEqual(a, b)
        sides = {brs.build_rows(_items(), seed=s)[1]["r15-01"] for s in range(20)}
        self.assertEqual(sides, {"A", "B"})

    def test_phone_numbers_are_redacted_in_every_text_column(self):
        items = [{"id": "r15-01", "rule": "r15", "context": "call me 727-555-0142 about the lease",
                  "off": "text 727-555-0142", "live": "will do"}]
        rows, _, _ = brs.build_rows(items)
        for col in ("context", "option_A", "option_B"):
            self.assertNotIn("555-0142", rows[0][col])


class WriteOutputs(unittest.TestCase):
    def test_writes_sheet_key_arms_and_readme(self):
        rows, key, skipped = brs.build_rows(_items(), seed=1)
        with tempfile.TemporaryDirectory() as d:
            brs.write_outputs(d, rows, key, [{"id": "x"}], skipped,
                              {"date": "2026-09-19", "model": "m", "human_bin": "h",
                               "prompt_bytes": {}, "temperature": 0.7, "max_tokens": 80, "days": 365})
            with open(os.path.join(d, "rating_sheet.csv"), newline="") as f:
                got = list(csv.DictReader(f))
            self.assertEqual(list(got[0].keys()), brs.SHEET_COLUMNS)
            self.assertEqual(len(got), 2)
            k = json.load(open(os.path.join(d, "answer_key.json")))
            self.assertEqual(k["_mode"], "preference")
            arms = json.load(open(os.path.join(d, "arms.json")))
            self.assertEqual(arms["skipped_identical"], 2)
            readme = open(os.path.join(d, "README.md")).read()
            self.assertIn("2 rows (1 rule 14", readme)
            self.assertIn("score_preference.py", readme)
            self.assertIn("LIVE arm", readme)


if __name__ == "__main__":
    unittest.main()
