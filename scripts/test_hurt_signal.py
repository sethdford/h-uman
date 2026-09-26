#!/usr/bin/env python3
"""Tests for hurt_signal.py — the Python twin of hu_hurt_signal_detect. The
cases mirror tests/test_daemon_hurt_handoff.c so the two cannot drift apart
silently; the phrase lists themselves are read from the C source."""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hurt_signal  # noqa: E402

D = hurt_signal.Detector()


class TestHurtSignal(unittest.TestCase):
    def test_phrase_lists_come_from_the_c_source(self):
        self.assertIn("u mad at me", D.phrases)
        self.assertIn("why ru texting", D.leads)
        self.assertIn("short", D.descriptors)

    def test_incident_messages(self):  # hurt_detect_fires_on_incident_messages
        for s in ("U mad at me?", "Why u being short?", "See this is why i get scared w u...",
                  "Why ru texting so weirddd now"):
            self.assertTrue(D(s), s)

    def test_common_variants(self):  # hurt_detect_fires_on_common_variants
        for s in ("are we ok??", "Did I do something wrong", "why are you ignoring me",
                  "you’re being so cold", "ARE YOU MAD", "why   are  you\tbeing   distant"):
            self.assertTrue(D(s), s)

    def test_third_party_and_lookalikes(self):  # hurt_detect_ignores_third_party_and_lookalikes
        for s in ("my boss is mad at me", "i'm mad at my sister lol", "why are you so sweet",
                  "what did you do today", "short on time today, call later?",
                  "it's cold outside", "Have to go to din for my dads bday", "Heyo",
                  "you made my day"):
            self.assertFalse(D(s), s)

    def test_empty_is_false(self):
        self.assertFalse(D(""))
        self.assertFalse(D(None))

    def test_missing_source_raises_rather_than_detecting_nothing(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(OSError):
                hurt_signal.Detector(os.path.join(d, "nope.c"))
            empty = os.path.join(d, "empty.c")
            with open(empty, "w") as f:
                f.write("/* no arrays */")
            with self.assertRaises(ValueError):
                hurt_signal.Detector(empty)


if __name__ == "__main__":
    unittest.main()
