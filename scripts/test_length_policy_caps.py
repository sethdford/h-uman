#!/usr/bin/env python3
"""Tests for length_policy_caps.py — synthetic log lines only (stdlib unittest)."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import length_policy_caps as lpc  # noqa: E402

TAIL = "inbound_len=20 brief=0 stage=1"
LINES = [
    "t [HU_LENGTH_POLICY shadow] old_cap=50 new_cap=50 tight_old=1 tight_new=0 stats=1 "
    "p50=16 p50_derived=1 shape=0 " + TAIL,
    "t [HU_LENGTH_POLICY shadow] old_cap=160 new_cap=198 tight_old=0 tight_new=0 stats=1 "
    "p50=16 p50_derived=1 shape=1 " + TAIL,
    "t [HU_LENGTH_POLICY shadow] old_cap=15 new_cap=15 tight_old=1 tight_new=1 stats=0 "
    "p50=0 p50_derived=0 shape=0 " + TAIL,
    "an unrelated log line",
]
LOWERED = ("t [HU_LENGTH_POLICY shadow] old_cap=99 new_cap=50 tight_old=0 tight_new=0 stats=1 "
           "p50=16 p50_derived=1 shape=0 " + TAIL)


def run_on(lines):
    with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
        f.write("\n".join(lines) + "\n")
    try:
        return lpc.main(["--log", f.name])
    finally:
        os.unlink(f.name)


class SummarizeTest(unittest.TestCase):
    def test_counts_only_turns_with_stats(self):
        out = lpc.summarize(LINES)
        self.assertEqual(out["turns"], 3)
        self.assertEqual(out["turns_with_stats"], 2)
        self.assertEqual(out["tight_old_rate"], 0.5)
        self.assertEqual(out["tight_new_rate"], 0.0)
        self.assertEqual(out["new_gt_old"], 1)
        self.assertEqual(out["new_gt_old_shaped"], 1)
        self.assertEqual(out["new_lt_old"], 0)
        self.assertEqual(out["old_cap_median"], 105)

    def test_healthy_log_exits_zero(self):
        self.assertEqual(run_on(LINES), 0)

    def test_a_lowered_cap_is_a_bug(self):
        self.assertEqual(run_on(LINES + [LOWERED]), 1)

    def test_refuses_without_stats_turns(self):
        self.assertEqual(run_on(LINES[2:]), 2)

    def test_refuses_on_missing_log(self):
        self.assertEqual(lpc.main(["--log", "/nonexistent/hu-length-policy.log"]), 2)


if __name__ == "__main__":
    unittest.main()
