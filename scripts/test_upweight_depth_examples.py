#!/usr/bin/env python3
"""Tests for upweight_depth_examples.py (stdlib unittest)."""
import json
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(__file__))
from upweight_depth_examples import MAX_WEIGHT, reply_of, upweight, weight_for  # noqa: E402


class TestWeightFor(unittest.TestCase):
    def test_typical_short_reply_gets_one_copy(self):
        self.assertEqual(weight_for("Them: you around?", "yeah", long_threshold=77), 1)

    def test_long_reply_gets_extra_copy(self):
        self.assertEqual(weight_for("Them: how was it", "x" * 80, long_threshold=77), 2)

    def test_question_back_gets_extra_copy(self):
        self.assertEqual(weight_for("Them: rough day", "How's your mood today?", long_threshold=77) >= 2,
                         True)

    def test_emotional_prompt_gets_extra_copy(self):
        self.assertEqual(weight_for("Them: I'm so stressed about tomorrow", "you'll crush it",
                                    long_threshold=77), 2)

    def test_emotion_match_is_word_bounded(self):
        # "missing" is not "miss"-the-feeling in "dismissed"; word-boundary match only.
        self.assertEqual(weight_for("Them: the case got dismissed", "nice", long_threshold=77), 1)

    def test_weight_is_capped(self):
        w = weight_for("Them: I miss you, feeling sad", "x" * 200 + "?", long_threshold=77)
        self.assertEqual(w, MAX_WEIGHT)

    def test_question_in_prompt_alone_does_not_count(self):
        # Only a question the REPLY asks back is follow-through.
        self.assertEqual(weight_for("Them: you coming?", "yep", long_threshold=77), 1)


class TestReplyOf(unittest.TestCase):
    def test_orpo_uses_chosen(self):
        self.assertEqual(reply_of({"prompt": "p", "chosen": "c", "rejected": "r"}), "c")

    def test_sft_uses_completion(self):
        self.assertEqual(reply_of({"prompt": "p", "completion": "c"}), "c")

    def test_unknown_shape_raises(self):
        with self.assertRaises(ValueError):
            reply_of({"prompt": "p", "text": "t"})


class TestUpweight(unittest.TestCase):
    def rows(self):
        return ([{"prompt": "Them: hey", "completion": "yo"}] * 8
                + [{"prompt": "Them: I'm sad today", "completion": "Why so down? How can I help?"}]
                + [{"prompt": "Them: ok", "completion": "x" * 100}])

    def test_depth_rows_duplicated_typical_rows_kept_once(self):
        out, stats = upweight(self.rows())
        self.assertEqual(sum(r["completion"] == "yo" for r in out), 8)
        self.assertEqual(sum(r["completion"].startswith("Why so down") for r in out), 3)
        self.assertEqual(stats["rows_in"], 10)
        self.assertEqual(stats["rows_out"], len(out))
        self.assertGreater(stats["rows_out"], stats["rows_in"])

    def test_output_is_deterministic_for_a_seed(self):
        a, _ = upweight(self.rows(), seed=7)
        b, _ = upweight(self.rows(), seed=7)
        self.assertEqual(a, b)

    def test_empty_input_refuses(self):
        with self.assertRaises(ValueError):
            upweight([])


class TestCli(unittest.TestCase):
    def test_cli_writes_output_and_sidecar(self):
        here = os.path.dirname(os.path.abspath(__file__))
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "in.jsonl")
            with open(src, "w") as f:
                for r in [{"prompt": "Them: hi", "completion": "hey"},
                          {"prompt": "Them: I miss you", "completion": "miss you too, when are you back?"}]:
                    f.write(json.dumps(r) + "\n")
            dst = os.path.join(d, "out.jsonl")
            side = os.path.join(d, "stats.json")
            subprocess.run([sys.executable, os.path.join(here, "upweight_depth_examples.py"),
                            "--input", src, "--output", dst, "--sidecar", side], check=True)
            out = [json.loads(l) for l in open(dst)]
            self.assertEqual(len(out), 1 + 3)
            self.assertEqual(json.load(open(side))["rows_out"], 4)

    def test_cli_refuses_empty_input(self):
        here = os.path.dirname(os.path.abspath(__file__))
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "in.jsonl")
            open(src, "w").close()
            dst = os.path.join(d, "out.jsonl")
            rc = subprocess.run([sys.executable, os.path.join(here, "upweight_depth_examples.py"),
                                 "--input", src, "--output", dst,
                                 "--sidecar", os.path.join(d, "s.json")]).returncode
            self.assertNotEqual(rc, 0)
            self.assertFalse(os.path.exists(dst))


if __name__ == "__main__":
    unittest.main()
