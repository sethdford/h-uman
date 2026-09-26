#!/usr/bin/env python3
"""Tests for measure_contact_reply_lengths.py — hermetic: synthetic chat.db,
memory.db and persona JSON in a temp dir (stdlib unittest)."""
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import measure_contact_reply_lengths as mr  # noqa: E402
from test_eval_conversation_quality import Fixture, MIN  # noqa: E402

DAYS = ["--days", "36500"]  # fixture messages are dated 2026-09-01


class TestMeasure(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.fx = Fixture(self.tmp.name)
        self.persona = os.path.join(self.tmp.name, "seth.json")
        self.write_persona({"+1": {"name": "Lexi"}, "+2": {"name": "Sparse", FIELD: 99}})

    def tearDown(self):
        self.tmp.cleanup()

    def write_persona(self, contacts):
        with open(self.persona, "w") as f:
            json.dump({"name": "seth", "contacts": contacts}, f)

    def contacts(self):
        with open(self.persona) as f:
            return json.load(f)["contacts"]

    def run_main(self, *extra):
        self.fx.close()
        return mr.main(["--persona", self.persona, "--chat-db", self.fx.chat_path,
                        "--memory-db", self.fx.mem_path, *DAYS, *extra])

    def seth_texts(self, contact, texts, start=0):
        for i, t in enumerate(texts):
            self.fx.msg(contact, start + i * 10 * MIN, t, True)
            self.fx.msg(contact, start + i * 10 * MIN + MIN, "ok", False)

    def test_only_seths_own_texts_are_measured(self):
        # 20 of Seth's 10-byte texts, plus h-uman sends that are much longer.
        self.seth_texts("+1", ["abcdefghij"] * 20)
        for i in range(10):
            secs = 400 * MIN + i * 10 * MIN
            prior = self.fx.max_rowid()
            text = "h-uman wrote this long reply number %02d" % i
            self.fx.outbound("+1", secs, text, prior)
            self.fx.msg("+1", secs, text, True)
        self.assertEqual(self.run_main("--write"), 0)
        self.assertEqual(self.contacts()["+1"][FIELD], 10)

    def test_length_is_utf8_bytes(self):
        self.seth_texts("+1", ["gn 😘"] * 20)  # 3 ASCII + 4-byte emoji
        self.run_main("--write")
        self.assertEqual(self.contacts()["+1"][FIELD], 7)

    def test_too_few_texts_writes_nothing_and_keeps_existing(self):
        self.seth_texts("+1", ["abcdefghij"] * 20)
        self.seth_texts("+2", ["x" * 50] * 5, start=100000)
        self.run_main("--write", "--min-n", "20")
        c = self.contacts()
        self.assertEqual(c["+2"][FIELD], 99)  # n=5 < 20: untouched
        self.assertEqual(c["+1"][FIELD], 10)

    def test_dry_run_does_not_modify_persona(self):
        self.seth_texts("+1", ["abcdefghij"] * 20)
        before = open(self.persona).read()
        self.assertEqual(self.run_main(), 0)
        self.assertEqual(open(self.persona).read(), before)

    def test_write_leaves_a_backup(self):
        self.seth_texts("+1", ["abcdefghij"] * 20)
        self.run_main("--write")
        backups = [f for f in os.listdir(self.tmp.name) if ".bak-replylen-" in f]
        self.assertEqual(len(backups), 1)

    def test_percentile_nearest_rank(self):
        self.assertEqual(mr.percentile(list(range(1, 11)), 90), 9)
        self.assertEqual(mr.percentile([5], 90), 5)


FIELD = mr.FIELD

if __name__ == "__main__":
    unittest.main()
