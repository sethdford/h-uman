#!/usr/bin/env python3
"""Tests for check_contact_profiles.py — hermetic (stdlib unittest): the pure
per-contact checks, plus one end-to-end run on synthetic chat.db/memory.db."""
import datetime as dt
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_contact_profiles as cp  # noqa: E402
from test_eval_conversation_quality import Fixture, MIN, T0  # noqa: E402


def checks(findings):
    return {f["check"] for f in (findings or [])}


SETH = ["sounds good see you then"] * 20  # 24 bytes, no emoji, 0% one-word


class TestCheckContact(unittest.TestCase):
    def test_too_few_texts_is_unmeasured_not_ok(self):
        self.assertIsNone(cp.check_contact({}, SETH[:19], [], 0, min_n=20))

    def test_lexi_incident_profile_is_flagged(self):
        # The 2026-09-26 profile: built from 53 messages, no emoji, short texts.
        old = {"total_messages": 53, "uses_emoji": False, "prefers_short_texts": True}
        seth = ["love the curls 😘"] * 2 + ["see you at the park later tonight ok"] * 18
        huuman = ["Yeah", "Nah", "Aight", "Hey", "Good", "Tired", "hope your day is good"]
        found = checks(cp.check_contact(old, seth, huuman, 1157, min_n=20))
        self.assertIn("sample_outgrown", found)
        self.assertIn("emoji_mismatch", found)
        self.assertIn("huuman_terse", found)

    def test_sample_outgrown_needs_a_real_gap(self):
        self.assertIn("sample_outgrown",
                      checks(cp.check_contact({"total_messages": 53}, SETH, [], 1157, 20)))
        self.assertNotIn("sample_outgrown",
                         checks(cp.check_contact({"total_messages": 900}, SETH, [], 1000, 20)))
        # 3x but only 60 messages behind: too small to call.
        self.assertNotIn("sample_outgrown",
                         checks(cp.check_contact({"total_messages": 30}, SETH, [], 90, 20)))

    def test_emoji_mismatch_both_directions(self):
        some = ["ok 😘"] * 2 + SETH[:18]  # 10%
        self.assertIn("emoji_mismatch",
                      checks(cp.check_contact({"uses_emoji": False}, some, [], 0, 20)))
        rare = ["ok 😘"] + SETH * 2  # 1 of 41
        self.assertNotIn("emoji_mismatch",
                         checks(cp.check_contact({"uses_emoji": False}, rare, [], 0, 20)))
        none50 = SETH * 3  # 60 texts, 0 emoji
        self.assertIn("emoji_mismatch",
                      checks(cp.check_contact({"uses_emoji": True}, none50, [], 0, 20)))

    def test_short_rule_tight_only_when_seth_writes_longer(self):
        long_ = ["x" * 100] * 20
        self.assertIn("short_rule_tight",
                      checks(cp.check_contact({"prefers_short_texts": True}, long_, [], 0, 20)))
        self.assertNotIn("short_rule_tight",
                         checks(cp.check_contact({"prefers_short_texts": True}, SETH, [], 0, 20)))
        self.assertNotIn("short_rule_tight",
                         checks(cp.check_contact({"prefers_short_texts": False}, long_, [], 0, 20)))

    def test_reply_floor_missing_or_drifted(self):
        self.assertIn("reply_floor", checks(cp.check_contact({}, SETH, [], 0, 20)))
        self.assertNotIn("reply_floor",
                         checks(cp.check_contact({cp.REPLY_FIELD: 26}, SETH, [], 0, 20)))
        self.assertIn("reply_floor",
                      checks(cp.check_contact({cp.REPLY_FIELD: 60}, SETH, [], 0, 20)))

    def test_huuman_terse_needs_share_ratio_and_n(self):
        terse = ["Yeah"] * 5 + ["sounds good to me"] * 5  # 50%
        self.assertIn("huuman_terse", checks(cp.check_contact({}, SETH, terse, 0, 20)))
        mild = ["Yeah"] * 3 + ["sounds good to me"] * 7  # 30%
        self.assertNotIn("huuman_terse", checks(cp.check_contact({}, SETH, mild, 0, 20)))
        few = ["Yeah"] * 4  # n=4 < 5
        self.assertNotIn("huuman_terse", checks(cp.check_contact({}, SETH, few, 0, 20)))
        seth_terse = ["ok"] * 8 + SETH[:12]  # Seth 40% one-word; h-uman 50% is not 2x
        self.assertNotIn("huuman_terse",
                         checks(cp.check_contact({}, seth_terse, terse, 0, 20)))


class TestEndToEnd(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.fx = Fixture(self.tmp.name)
        self.persona = os.path.join(self.tmp.name, "seth.json")

    def tearDown(self):
        self.tmp.cleanup()

    def build(self, profile):
        with open(self.persona, "w") as f:
            json.dump({"name": "seth", "contacts": {"+1": profile}}, f)
        for i in range(20):
            self.fx.msg("+1", i * 10 * MIN, "sounds good see you then", True)
            self.fx.msg("+1", i * 10 * MIN + MIN, "ok cool", False)

    def huuman_send(self, secs, text):
        self.fx.outbound("+1", secs, text, self.fx.max_rowid())
        self.fx.msg("+1", secs, text, True)

    def run_main(self, *extra, now=None):
        self.fx.close()
        since = T0 - dt.timedelta(days=30)
        report = cp.run(json.load(open(self.persona)), self.fx.chat_path, self.fx.mem_path,
                        since, 20, now=now)
        return report

    def test_only_recent_huuman_sends_count_for_terseness(self):
        self.build({"name": "L", cp.REPLY_FIELD: 26})
        for i in range(6):  # one-word sends 10+ days before "now"
            self.huuman_send(400 * MIN + i * 10 * MIN, "Yeah")
        now = T0 + dt.timedelta(days=12)
        rep = self.run_main(now=now)
        self.assertEqual(rep["stale"], [])
        self.assertEqual([c["name"] for c in rep["ok"]], ["L"])

    def test_recent_terse_huuman_flags_and_report_has_no_text(self):
        self.build({"name": "L", cp.REPLY_FIELD: 26})
        for i in range(6):
            self.huuman_send(400 * MIN + i * 10 * MIN, "Yeah")
        now = T0 + dt.timedelta(days=1)
        rep = self.run_main(now=now)
        self.assertEqual([c["name"] for c in rep["stale"]], ["L"])
        self.assertIn("huuman_terse", checks(rep["stale"][0]["findings"]))
        self.assertNotIn("sounds good", json.dumps(rep))
        self.assertNotIn("Yeah", json.dumps(rep))

    def test_main_exit_codes_and_refusal(self):
        self.build({"name": "L"})  # reply floor missing -> stale
        self.fx.close()
        out = os.path.join(self.tmp.name, "report.json")
        args = ["--persona", self.persona, "--chat-db", self.fx.chat_path,
                "--memory-db", self.fx.mem_path, "--days", "36500", "--out", out]
        self.assertEqual(cp.main(args), 1)
        self.assertTrue(os.path.exists(out))
        missing = ["--persona", os.path.join(self.tmp.name, "nope.json"), "--out",
                   os.path.join(self.tmp.name, "never.json")]
        self.assertEqual(cp.main(missing), 2)
        self.assertFalse(os.path.exists(os.path.join(self.tmp.name, "never.json")))


if __name__ == "__main__":
    unittest.main()
