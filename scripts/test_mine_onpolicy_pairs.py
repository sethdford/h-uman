#!/usr/bin/env python3
"""Tests for mine_onpolicy_pairs.py — hermetic: synthetic chat.db/memory.db
(stdlib unittest). A pair may only be minted when h-uman and Seth answered
the SAME inbound message."""
import json
import os
import sys
import tempfile
import unittest
import datetime as dt

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mine_onpolicy_pairs as mp  # noqa: E402
from test_eval_conversation_quality import Fixture, MIN, T0  # noqa: E402

SINCE = T0 - dt.timedelta(days=1)


class TestMine(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.fx = Fixture(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def huuman(self, secs, text, contact="+1"):
        self.fx.outbound(contact, secs, text, self.fx.max_rowid())
        self.fx.msg(contact, secs, text, True)

    def run_mine(self):
        self.fx.close()
        return mp.mine(self.fx.chat_path, self.fx.mem_path, SINCE, {"+1": "Lexi"})

    def test_same_inbound_answered_by_both_is_a_pair(self):
        self.fx.msg("+1", 0, "Why u being short?", False)
        self.huuman(60, "Nah")
        self.fx.msg("+1", 120, "not short, just wiped. how was dinner with your dad?", True)
        self.fx.msg("+1", 10 * MIN, "it was nice", False)
        pairs, per, _ = self.run_mine()
        self.assertEqual(len(pairs), 1)
        p = pairs[0]
        self.assertEqual(p["rejected"], "Nah")
        self.assertIn("how was dinner", p["chosen"])
        self.assertEqual(set(p), {"prompt", "chosen", "rejected"})
        self.assertEqual(per["Lexi"]["seth_after_huuman"], 1)

    def test_replies_to_different_inbounds_are_not_paired(self):
        self.fx.msg("+1", 0, "Heyo", False)
        self.huuman(60, "Hey")
        self.fx.msg("+1", 5 * MIN, "how was your day?", False)
        self.fx.msg("+1", 6 * MIN, "long one, you?", True)  # Seth, different inbound
        pairs, per, _ = self.run_mine()
        self.assertEqual(pairs, [])
        self.assertEqual(per["Lexi"]["gold_moments"], 1)
        self.assertEqual(per["Lexi"]["huuman_only"], 1)

    def test_run_with_an_ambiguous_send_is_skipped(self):
        # Before any provenance: a logged draft that differs from what was
        # delivered makes that send ambiguous, so its run can't be used.
        self.fx.mem.execute(
            "insert into messages(session_id,role,content,created_at) values (?,?,?,?)",
            ("+1", "assistant", "a draft that was rewritten", T0.strftime("%Y-%m-%d %H:%M:%S")))
        self.fx.msg("+1", 0, "you around?", False)
        self.fx.msg("+1", 30, "yep what's up", True)
        pairs, per, _ = self.run_mine()
        self.assertEqual(pairs, [])
        self.assertEqual(per["Lexi"]["skipped_ambiguous"], 1)

    def test_context_is_rendered_like_the_corpus_and_stops_at_a_gap(self):
        self.fx.msg("+1", 0, "old thread from hours ago", False)
        base = 3 * 3600
        self.fx.msg("+1", base, "Heyo", False)
        self.fx.msg("+1", base + 30, "you up?", False)
        self.huuman(base + 60, "Hey")
        self.fx.msg("+1", base + 90, "hey you! just got home", True)
        pairs, _, _ = self.run_mine()
        self.assertEqual(pairs[0]["prompt"], "Them: Heyo\nThem: you up?")
        self.assertNotIn("old thread", pairs[0]["prompt"])

    def test_write_refuses_with_no_pairs_and_writes_no_text_manifest(self):
        self.fx.msg("+1", 0, "Heyo", False)
        self.fx.msg("+1", 60, "hey!", True)
        self.fx.close()
        out = os.path.join(self.tmp.name, "pairs.jsonl")
        args = ["--chat-db", self.fx.chat_path, "--memory-db", self.fx.mem_path,
                "--persona", os.path.join(self.tmp.name, "none.json"),
                "--days", "36500", "--write", out]
        self.assertEqual(mp.main(args), 2)
        self.assertFalse(os.path.exists(out))

    def test_write_emits_trainer_shape_and_text_free_manifest(self):
        self.fx.msg("+1", 0, "Why u being short?", False)
        self.huuman(60, "Nah")
        self.fx.msg("+1", 120, "sorry, long day. dinner good?", True)
        self.fx.close()
        out = os.path.join(self.tmp.name, "pairs.jsonl")
        args = ["--chat-db", self.fx.chat_path, "--memory-db", self.fx.mem_path,
                "--persona", os.path.join(self.tmp.name, "none.json"),
                "--days", "36500", "--write", out]
        self.assertEqual(mp.main(args), 0)
        rows = [json.loads(line) for line in open(out)]
        self.assertEqual(len(rows), 1)
        self.assertEqual(set(rows[0]), {"prompt", "chosen", "rejected"})
        manifest = open(out + ".manifest.json").read()
        self.assertEqual(json.loads(manifest)["pairs"], 1)
        for text in ("Nah", "dinner", "short"):
            self.assertNotIn(text, manifest)


if __name__ == "__main__":
    unittest.main()
