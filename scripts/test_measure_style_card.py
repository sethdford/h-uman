#!/usr/bin/env python3
"""Hermetic tests for scripts/measure_style_card.py.

No chat.db, no ~/.human, no network: every test feeds synthetic
(timestamp, text) messages straight into the measurement and checks the
card that comes out. The corpus below is built so each axis has an exact
known rate, which is what lets these tests catch a wrong denominator,
a wrong window filter, or a card written on refusal.

Run: python3 scripts/test_measure_style_card.py
"""
import datetime
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure_style_card as msc  # noqa: E402

T0 = datetime.datetime(2026, 8, 1, 12, 0, 0)


def synthetic_corpus(n=400):
    """n messages, timestamps spaced one minute apart inside the window.

    Composition (exact, by index):
      i % 5 == 0   -> starts lowercase  (20%)
      i % 4 == 0   -> ends with "."     (25%)   [period]
      i % 10 == 1  -> ends with "?"     (10%)   [question]
      i % 20 == 2  -> ends with "!"     ( 5%)   [exclaim]
      everything else has no terminal punctuation -> 60%
      i % 8 == 3   -> contains an emoji (12.5%)
    """
    out = []
    for i in range(n):
        head = "yeah ok" if i % 5 == 0 else "Yeah ok"
        body = head + " sounds good"
        if i % 8 == 3:
            body += " 😂"
        if i % 4 == 0:
            body += "."
        elif i % 10 == 1:
            body += "?"
        elif i % 20 == 2:
            body += "!"
        out.append((T0 + datetime.timedelta(minutes=i), body))
    return out


class BuildCard(unittest.TestCase):
    def test_axes_match_known_synthetic_rates(self):
        msgs = synthetic_corpus(400)
        card = msc.build_card(
            msgs, persona="test", window_start=T0 - datetime.timedelta(days=1),
            window_end=T0 + datetime.timedelta(days=1), min_n=300, n_resamples=200,
        )
        self.assertEqual(card["schema"], msc.SCHEMA)
        self.assertEqual(card["persona"], "test")
        self.assertEqual(card["n"], 400)
        ax = card["axes"]
        self.assertAlmostEqual(ax["lowercase_start_rate"]["value"], 0.20, places=9)
        self.assertAlmostEqual(ax["question_rate"]["value"], 0.10, places=9)
        self.assertAlmostEqual(ax["exclamation_rate"]["value"], 0.05, places=9)
        self.assertAlmostEqual(ax["no_terminal_punct_rate"]["value"], 0.60, places=9)
        self.assertAlmostEqual(ax["emoji_rate"]["value"], 0.125, places=9)
        for name in msc.CARD_AXES:
            entry = ax[name]
            self.assertLessEqual(entry["ci_lo"], entry["value"], name)
            self.assertGreaterEqual(entry["ci_hi"], entry["value"], name)
            self.assertEqual(entry["n"], 400, name)

    def test_window_excludes_messages_outside_it(self):
        msgs = synthetic_corpus(400)
        # 100 extra messages a year earlier must not be counted.
        stale = [(T0 - datetime.timedelta(days=365, minutes=i), "old text") for i in range(100)]
        card = msc.build_card(
            msgs + stale, persona="test", window_start=T0 - datetime.timedelta(days=1),
            window_end=T0 + datetime.timedelta(days=1), min_n=300, n_resamples=50,
        )
        self.assertEqual(card["n"], 400)
        self.assertEqual(card["window"]["start"], (T0 - datetime.timedelta(days=1)).date().isoformat())

    def test_refuses_below_min_n(self):
        msgs = synthetic_corpus(299)
        with self.assertRaises(msc.InsufficientData):
            msc.build_card(
                msgs, persona="test", window_start=T0 - datetime.timedelta(days=1),
                window_end=T0 + datetime.timedelta(days=1), min_n=300, n_resamples=50,
            )


class RunCli(unittest.TestCase):
    def _args(self, out):
        return msc.parse_args([
            "--persona", "test", "--out", out, "--min-n", "300", "--n-resamples", "50",
            "--end", (T0 + datetime.timedelta(days=1)).date().isoformat(), "--days", "2",
        ])

    def test_refusal_writes_nothing_and_exits_nonzero(self):
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "test.style-card.json")
            rc = msc.run(self._args(out), messages=synthetic_corpus(10))
            self.assertNotEqual(rc, 0)
            self.assertFalse(os.path.exists(out))

    def test_success_writes_card_with_provenance(self):
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "test.style-card.json")
            rc = msc.run(self._args(out), messages=synthetic_corpus(400))
            self.assertEqual(rc, 0)
            with open(out) as f:
                card = json.load(f)
            self.assertEqual(card["schema"], msc.SCHEMA)
            self.assertEqual(card["window"]["days"], 2)
            self.assertEqual(card["min_n"], 300)
            self.assertIn("generated_at", card)
            self.assertEqual(set(card["axes"]), set(msc.CARD_AXES))

    def test_card_never_contains_message_text(self):
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "test.style-card.json")
            msgs = [(ts, t + " SECRETMARKER") for ts, t in synthetic_corpus(400)]
            self.assertEqual(msc.run(self._args(out), messages=msgs), 0)
            with open(out) as f:
                self.assertNotIn("SECRETMARKER", f.read())


if __name__ == "__main__":
    unittest.main()
