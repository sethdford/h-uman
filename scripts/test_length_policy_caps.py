#!/usr/bin/env python3
"""Tests for length_policy_caps.py — synthetic log lines only (stdlib unittest)."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import length_policy_caps as lpc  # noqa: E402

LINES = [
    "t [HU_LENGTH_POLICY shadow] old_cap=50 new_cap=50 tight_old=1 tight_new=0 stats=1 p50=16",
    "t [HU_LENGTH_POLICY shadow] old_cap=99 new_cap=50 tight_old=0 tight_new=0 stats=1 p50=16",
    "t [HU_LENGTH_POLICY shadow] old_cap=15 new_cap=15 tight_old=1 tight_new=1 stats=0 p50=0",
    "an unrelated log line",
]


class SummarizeTest(unittest.TestCase):
    def test_counts_only_turns_with_stats(self):
        out = lpc.summarize(LINES)
        self.assertEqual(out["turns"], 3)
        self.assertEqual(out["turns_with_stats"], 2)
        self.assertEqual(out["tight_old_rate"], 0.5)
        self.assertEqual(out["tight_new_rate"], 0.0)
        self.assertEqual(out["new_lt_old"], 1)
        self.assertEqual(out["old_cap_median"], 74.5)

    def test_refuses_without_stats_turns(self):
        with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
            f.write(LINES[2] + "\n")
        try:
            self.assertEqual(lpc.main(["--log", f.name]), 2)
        finally:
            os.unlink(f.name)


if __name__ == "__main__":
    unittest.main()
