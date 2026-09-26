#!/usr/bin/env python3
"""Tests for eval_conversation_quality.py — hermetic: synthetic chat.db and
memory.db in a temp dir, no real messages (stdlib unittest)."""
import datetime as dt
import json
import os
import sqlite3
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import eval_conversation_quality as cq  # noqa: E402

T0 = dt.datetime(2026, 9, 1, 12, 0, tzinfo=dt.timezone.utc)
MIN = 60
HOUR = 3600


def apple_ns(t):
    return int((t - cq.APPLE_EPOCH).total_seconds() * 1e9)


class Fixture:
    """Builds chat.db (message/handle) + memory.db (messages) with the real
    column names eval_conversation_quality.py reads."""

    def __init__(self, d):
        self.chat_path = os.path.join(d, "chat.db")
        self.mem_path = os.path.join(d, "memory.db")
        self.chat = sqlite3.connect(self.chat_path)
        self.chat.executescript(
            "create table handle(ROWID integer primary key, id text);"
            "create table message(ROWID integer primary key, guid text, text text,"
            " attributedBody blob, handle_id integer, is_from_me integer, date integer,"
            " cache_roomnames text, item_type integer default 0,"
            " associated_message_guid text, associated_message_type integer default 0);")
        self.mem = sqlite3.connect(self.mem_path)
        self.mem.execute("create table messages(id integer primary key autoincrement,"
                         " session_id text, role text, content text, created_at text)")
        self.handles = {}
        self.n = 0

    def handle(self, contact):
        if contact not in self.handles:
            cur = self.chat.execute("insert into handle(id) values (?)", (contact,))
            self.handles[contact] = cur.lastrowid
        return self.handles[contact]

    def outbound(self, contact, secs, text, prior, kind="text", raw_ms=None):
        """A daemon send-provenance row (src/daemon/daemon_send_provenance.c).
        raw_ms stores a literal sent_at_ms, e.g. a pre-fix uptime stamp."""
        self.mem.execute("create table if not exists outbound_sends(id integer primary key,"
                         " sent_at_ms integer, channel text, contact text, kind text, text text,"
                         " prior_max_rowid integer)")
        self.mem.execute(
            "insert into outbound_sends(sent_at_ms,channel,contact,kind,text,prior_max_rowid)"
            " values (?,?,?,?,?,?)",
            (raw_ms if raw_ms is not None
             else int((T0 + dt.timedelta(seconds=secs)).timestamp() * 1000),
             "imessage", contact, kind, text, prior))

    def max_rowid(self):
        return self.chat.execute("select coalesce(max(ROWID),0) from message").fetchone()[0]

    def msg(self, contact, secs, text, from_me, huuman=False, room=None):
        self.n += 1
        guid = f"G{self.n}"
        self.chat.execute(
            "insert into message(guid,text,handle_id,is_from_me,date,cache_roomnames)"
            " values (?,?,?,?,?,?)",
            (guid, text, self.handle(contact), 1 if from_me else 0,
             apple_ns(T0 + dt.timedelta(seconds=secs)), room))
        if huuman:
            self.mem.execute(
                "insert into messages(session_id,role,content,created_at) values (?,?,?,?)",
                (contact, "assistant", text,
                 (T0 + dt.timedelta(seconds=secs)).strftime("%Y-%m-%d %H:%M:%S")))
        return guid

    def tapback(self, contact, secs, target_guid, kind=2000):
        self.n += 1
        self.chat.execute(
            "insert into message(guid,text,handle_id,is_from_me,date,"
            "associated_message_guid,associated_message_type) values (?,?,?,?,?,?,?)",
            (f"G{self.n}", None, self.handle(contact), 0,
             apple_ns(T0 + dt.timedelta(seconds=secs)), f"p:0/{target_guid}", kind))

    def close(self):
        self.chat.commit()
        self.mem.commit()
        self.chat.close()
        self.mem.close()


class TestTurnsAndOutcomes(unittest.TestCase):
    def build(self, fill):
        self.tmp = tempfile.TemporaryDirectory()
        fx = Fixture(self.tmp.name)
        fill(fx)
        fx.close()
        now = T0 + dt.timedelta(days=30)
        return cq.analyze(self.tmp.name + "/chat.db", self.tmp.name + "/memory.db",
                          since=T0 - dt.timedelta(days=1), now=now)

    def tearDown(self):
        self.tmp.cleanup()

    def test_consecutive_sends_form_one_turn(self):
        def fill(fx):
            fx.msg("+1", 0, "hey how was the trip", True)
            fx.msg("+1", 30, "also did you see the game", True)
            fx.msg("+1", 5 * MIN, "yes it was great", False)
        r = self.build(fill)
        self.assertEqual(r["turns"]["seth"], 1)
        self.assertEqual(r["per_turn"][0]["dead_end"], False)

    def test_no_reply_within_24h_is_dead_end(self):
        def fill(fx):
            fx.msg("+1", 0, "ok", True)
            fx.msg("+1", 26 * HOUR, "hi again", False)  # outside 24h
        r = self.build(fill)
        self.assertTrue(r["per_turn"][0]["dead_end"])

    def test_huuman_turn_attributed_by_memory_match(self):
        def fill(fx):
            fx.msg("+1", 0, "Why so down? How can I help?", True, huuman=True)
            fx.msg("+1", 2 * MIN, "just a long week", False)
        r = self.build(fill)
        self.assertEqual(r["turns"]["huuman"], 1)
        self.assertEqual(r["turns"]["seth"], 0)

    def test_burst_split_reply_is_all_huuman(self):
        # h-uman logs one reply; the egress splitter delivers it as two texts.
        # The second part must match the logged reply too, or the whole turn
        # falls into "ambiguous" and the h-uman arm keeps only unsplit replies.
        def fill(fx):
            fx.mem.execute(
                "insert into messages(session_id,role,content,created_at) values (?,?,?,?)",
                ("+1", "assistant", "that sounds rough. want to grab dinner tomorrow and talk it through",
                 T0.strftime("%Y-%m-%d %H:%M:%S")))
            fx.msg("+1", 5, "that sounds rough", True)
            fx.msg("+1", 20, "want to grab dinner tomorrow and talk it through", True)
            fx.msg("+1", 5 * MIN, "yes please", False)
        r = self.build(fill)
        self.assertEqual(r["turns"]["huuman"], 1)
        self.assertEqual(r["turns"]["ambiguous"], 0)

    def test_proactive_send_is_not_counted_as_seth(self):
        # Proactive check-ins are h-uman sends with no `assistant` row; they
        # must not leak into Seth's arm.
        def fill(fx):
            fx.mem.execute("create table if not exists proactive_sends(id integer primary key,"
                           " channel text, contact text, message_ref text, sent_timestamp integer,"
                           " outcome_type integer, outcome_timestamp integer, processed integer)")
            fx.mem.execute("insert into proactive_sends(channel,contact,sent_timestamp) values (?,?,?)",
                           ("imessage", "+1", int(T0.timestamp())))
            fx.msg("+1", 10, "thinking of you, how did the move go", True)
            fx.msg("+1", 5 * MIN, "so good!", False)
        r = self.build(fill)
        self.assertEqual(r["turns"]["seth"], 0)
        self.assertEqual(r["turns"]["ambiguous"], 1)

    def test_unmatched_send_near_huuman_activity_is_ambiguous(self):
        # h-uman logged a reply but the delivered text differs (rewritten):
        # it must not be counted as Seth's.
        def fill(fx):
            fx.mem.execute(
                "insert into messages(session_id,role,content,created_at) values (?,?,?,?)",
                ("+1", "assistant", "totally different logged text",
                 T0.strftime("%Y-%m-%d %H:%M:%S")))
            fx.msg("+1", 60, "something else was delivered", True)
            fx.msg("+1", 5 * MIN, "reply", False)
        r = self.build(fill)
        self.assertEqual(r["turns"]["seth"], 0)
        self.assertEqual(r["turns"]["huuman"], 0)
        self.assertEqual(r["turns"]["ambiguous"], 1)

    def test_turn_younger_than_24h_is_censored(self):
        def fill(fx):
            fx.msg("+1", 0, "hey", True)
        self.tmp = tempfile.TemporaryDirectory()
        fx = Fixture(self.tmp.name)
        fill(fx)
        fx.close()
        r = cq.analyze(self.tmp.name + "/chat.db", self.tmp.name + "/memory.db",
                       since=T0 - dt.timedelta(days=1), now=T0 + dt.timedelta(hours=3))
        self.assertEqual(r["turns"]["seth"], 0)
        self.assertEqual(r["turns"]["censored"], 1)

    def test_group_chats_excluded(self):
        def fill(fx):
            fx.msg("+1", 0, "group hello", True, room="chat123")
            fx.msg("+1", 60, "hi all", False, room="chat123")
        r = self.build(fill)
        self.assertEqual(sum(r["turns"][k] for k in ("seth", "huuman")), 0)

    def test_depth_counts_speaker_changes_until_hour_gap(self):
        def fill(fx):
            fx.msg("+1", 0, "you around?", True)
            fx.msg("+1", 1 * MIN, "yeah", False)
            fx.msg("+1", 2 * MIN, "want to grab lunch", True)
            fx.msg("+1", 3 * MIN, "sure where", False)
            fx.msg("+1", 3 * HOUR, "late msg", True)  # after a >60m gap
        r = self.build(fill)
        first = r["per_turn"][0]
        self.assertEqual(first["depth"], 3)  # them, me, them

    def test_positive_tapback_counts_and_is_not_a_reply(self):
        def fill(fx):
            g = fx.msg("+1", 0, "made it home", True)
            fx.tapback("+1", MIN, g, kind=2000)  # love
        r = self.build(fill)
        t = r["per_turn"][0]
        self.assertTrue(t["positive_tapback"])
        self.assertTrue(t["dead_end"])  # a tapback alone is not a reply


class TestExactProvenance(unittest.TestCase):
    """outbound_sends rows (daemon send provenance) make attribution exact from
    the first recorded send onward; earlier periods keep the text heuristic."""

    def run_fixture(self, fill):
        self.tmp = tempfile.TemporaryDirectory()
        fx = Fixture(self.tmp.name)
        fill(fx)
        fx.close()
        return cq.analyze(self.tmp.name + "/chat.db", self.tmp.name + "/memory.db",
                          since=T0 - dt.timedelta(days=1), now=T0 + dt.timedelta(days=30))

    def tearDown(self):
        self.tmp.cleanup()

    def test_rewritten_huuman_send_is_huuman_with_provenance(self):
        # memory.db logged different text (would be ambiguous by heuristic),
        # but outbound_sends recorded exactly what was delivered.
        def fill(fx):
            fx.mem.execute(
                "insert into messages(session_id,role,content,created_at) values (?,?,?,?)",
                ("+1", "assistant", "pre-egress draft text", T0.strftime("%Y-%m-%d %H:%M:%S")))
            prior = fx.max_rowid()
            fx.outbound("+1", 5, "what was actually delivered", prior)
            fx.msg("+1", 5, "what was actually delivered", True)
            fx.msg("+1", 3 * MIN, "nice", False)
        r = self.run_fixture(fill)
        self.assertEqual(r["turns"]["huuman"], 1)
        self.assertEqual(r["turns"]["ambiguous"], 0)
        self.assertEqual(r["attribution"]["exact_matched"], 1)

    def test_seth_send_after_provenance_start_is_seth_not_ambiguous(self):
        def fill(fx):
            fx.mem.execute(
                "insert into messages(session_id,role,content,created_at) values (?,?,?,?)",
                ("+1", "assistant", "hey how was the interview", T0.strftime("%Y-%m-%d %H:%M:%S")))
            prior = fx.max_rowid()
            fx.outbound("+1", 0, "hey how was the interview", prior)
            fx.msg("+1", 0, "hey how was the interview", True)
            fx.msg("+1", 2 * MIN, "it went great", False)
            fx.msg("+1", 5 * MIN, "so proud of you", True)  # Seth, typed by hand
            fx.msg("+1", 8 * MIN, "thanks!!", False)
        r = self.run_fixture(fill)
        self.assertEqual(r["turns"]["huuman"], 1)
        self.assertEqual(r["turns"]["seth"], 1)
        self.assertEqual(r["turns"]["ambiguous"], 0)

    def test_send_before_provenance_start_uses_text_heuristic(self):
        def fill(fx):
            # Day 0: no outbound_sends yet -> heuristic; rewritten reply is ambiguous.
            fx.mem.execute(
                "insert into messages(session_id,role,content,created_at) values (?,?,?,?)",
                ("+1", "assistant", "draft that differs", T0.strftime("%Y-%m-%d %H:%M:%S")))
            fx.msg("+1", 60, "delivered text", True)
            fx.msg("+1", 5 * MIN, "ok", False)
            # Day 3: provenance begins.
            prior = fx.max_rowid()
            fx.outbound("+1", 3 * 86400, "later exact send", prior)
            fx.msg("+1", 3 * 86400, "later exact send", True)
            fx.msg("+1", 3 * 86400 + 60, "cool", False)
        r = self.run_fixture(fill)
        self.assertEqual(r["turns"]["ambiguous"], 1)
        self.assertEqual(r["turns"]["huuman"], 1)
        self.assertIsNotNone(r["attribution"]["exact_from"])

    def test_record_without_delivered_row_is_counted_unmatched(self):
        def fill(fx):
            fx.outbound("+1", 0, "never landed in chat.db", fx.max_rowid())
            fx.msg("+1", 10 * MIN, "unrelated thing seth typed", True)
            fx.msg("+1", 12 * MIN, "k", False)
        r = self.run_fixture(fill)
        self.assertEqual(r["attribution"]["exact_unmatched_records"], 1)
        self.assertEqual(r["turns"]["seth"], 1)

    # Deploys of 2026-09-25 stamped sent_at_ms with CLOCK_MONOTONIC (uptime),
    # e.g. 1831484402. Those rows still carry a correct prior_max_rowid.
    UPTIME_MS = 1_831_484_402

    def test_uptime_stamped_row_resolves_by_rowid_and_text(self):
        def fill(fx):
            fx.outbound("+1", 0, "Morning", fx.max_rowid(), raw_ms=self.UPTIME_MS)
            fx.msg("+1", 0, "Morning", True)
            fx.msg("+1", 2 * MIN, "Tired boo?", False)
        r = self.run_fixture(fill)
        self.assertEqual(r["turns"]["huuman"], 1)
        self.assertEqual(r["attribution"]["exact_matched"], 1)
        self.assertEqual(r["attribution"]["uptime_stamped_records"], 1)

    def test_uptime_stamp_does_not_label_earlier_sends_as_seth(self):
        # An uptime stamp read as epoch ms is 1970: provenance must not be
        # treated as complete from 1970, or every unclaimed send becomes Seth's.
        def fill(fx):
            fx.mem.execute(
                "insert into messages(session_id,role,content,created_at) values (?,?,?,?)",
                ("+1", "assistant", "draft that differs", T0.strftime("%Y-%m-%d %H:%M:%S")))
            fx.msg("+1", 60, "delivered text", True)
            fx.msg("+1", 5 * MIN, "ok", False)
            fx.outbound("+1", 3 * 86400, "later exact send", fx.max_rowid(),
                        raw_ms=self.UPTIME_MS)
            fx.msg("+1", 3 * 86400, "later exact send", True)
            fx.msg("+1", 3 * 86400 + 60, "cool", False)
        r = self.run_fixture(fill)
        self.assertEqual(r["turns"]["ambiguous"], 1)
        self.assertEqual(r["turns"]["seth"], 0)
        self.assertEqual(r["turns"]["huuman"], 1)
        # Completeness starts at the first resolved send, not 1970.
        self.assertTrue(r["attribution"]["exact_from"].startswith("2026-09-04"))

    def test_uptime_stamped_row_without_rowid_boundary_is_unmatched(self):
        def fill(fx):
            fx.outbound("+1", 0, "hey", -1, raw_ms=self.UPTIME_MS)
            fx.msg("+1", 0, "hey", True)
            fx.msg("+1", 2 * MIN, "hi", False)
        r = self.run_fixture(fill)
        self.assertEqual(r["attribution"]["exact_matched"], 0)
        self.assertEqual(r["attribution"]["exact_unmatched_records"], 1)

    def test_invalid_utf8_in_memory_db_does_not_abort(self):
        # A real memory.db row held non-UTF-8 bytes; the default text factory
        # raised OperationalError and the whole metric aborted.
        def fill(fx):
            fx.mem.execute(
                "insert into messages(session_id,role,content,created_at) "
                "values (?,?,CAST(X'68656c6c6fff' AS TEXT),?)",
                ("+1", "assistant", T0.strftime("%Y-%m-%d %H:%M:%S")))
            fx.msg("+1", 60, "typed by hand", True)
            fx.msg("+1", 5 * MIN, "ok", False)
        r = self.run_fixture(fill)  # raised OperationalError before the fix
        t = r["turns"]
        self.assertEqual(t["seth"] + t["huuman"] + t["ambiguous"], 1)


class TestHurtAfter(unittest.TestCase):
    """hurt_after: did the contact's next reply say they feel hurt or worried
    about us? The dead-end rate scored a hurt thread 0.000 because she kept
    replying (2026-09-26)."""

    def run_fixture(self, fill, detector=True):
        self.tmp = tempfile.TemporaryDirectory()
        fx = Fixture(self.tmp.name)
        fill(fx)
        fx.close()
        saved = cq._hurt_detector
        if not detector:
            cq._hurt_detector = lambda: None
        try:
            return cq.analyze(self.tmp.name + "/chat.db", self.tmp.name + "/memory.db",
                              since=T0 - dt.timedelta(days=1), now=T0 + dt.timedelta(days=30))
        finally:
            cq._hurt_detector = saved

    def tearDown(self):
        self.tmp.cleanup()

    @staticmethod
    def huuman(fx, secs, text):
        fx.outbound("+1", secs, text, fx.max_rowid())
        fx.msg("+1", secs, text, True)

    def test_hurt_reply_after_huuman_turn_is_counted(self):
        def fill(fx):
            fx.msg("+1", 0, "Have to go to din for my dads bday", False)
            self.huuman(fx, 60, "Enjoy")
            fx.msg("+1", 120, "U mad at me?", False)
            fx.msg("+1", 10 * MIN, "i'm fine, was just driving. have fun at dinner", True)
            fx.msg("+1", 12 * MIN, "ok thank u", False)
        r = self.run_fixture(fill)
        rows = {row["arm"]: row for row in r["per_turn"]}
        self.assertTrue(rows["huuman"]["hurt_after"])
        self.assertFalse(rows["seth"]["hurt_after"])
        self.assertEqual(cq.hurt_after_counts(r["per_turn"]),
                         {"seth": {"hurt": 0, "replies": 1}, "huuman": {"hurt": 1, "replies": 1}})

    def test_no_reply_is_not_scored(self):
        def fill(fx):
            fx.msg("+1", 0, "Heyo", False)
            self.huuman(fx, 60, "Hey")
        r = self.run_fixture(fill)
        self.assertIsNone(r["per_turn"][0]["hurt_after"])
        self.assertEqual(cq.hurt_after_counts(r["per_turn"])["huuman"], {"hurt": 0, "replies": 0})

    def test_only_the_first_reply_burst_counts(self):
        # The hurt message answers Seth's later send, not h-uman's earlier one.
        def fill(fx):
            fx.msg("+1", 0, "Heyo", False)
            self.huuman(fx, 60, "Hey")
            fx.msg("+1", 120, "whats up", False)
            fx.msg("+1", 10 * MIN, "nm", True)
            fx.msg("+1", 11 * MIN, "why are you being short", False)
        r = self.run_fixture(fill)
        rows = {row["arm"]: row for row in r["per_turn"]}
        self.assertFalse(rows["huuman"]["hurt_after"])
        self.assertTrue(rows["seth"]["hurt_after"])

    def test_unavailable_detector_reports_absent_not_zero(self):
        def fill(fx):
            fx.msg("+1", 0, "Heyo", False)
            self.huuman(fx, 60, "Hey")
            fx.msg("+1", 120, "U mad at me?", False)
        r = self.run_fixture(fill, detector=False)
        self.assertFalse(r["hurt_detector"])
        self.assertIsNone(r["per_turn"][0]["hurt_after"])


class TestSummary(unittest.TestCase):
    def test_refuses_verdict_when_arm_too_small(self):
        rows = [{"arm": "seth", "contact": "+1", "dead_end": False}] * 50 + \
               [{"arm": "huuman", "contact": "+1", "dead_end": True}] * 3
        s = cq.summarize(rows, min_turns=30)
        self.assertEqual(s["verdict"], "INSUFFICIENT")
        self.assertIsNone(s["dead_end_rate"]["huuman"])

    def test_paired_contacts_only_and_rates(self):
        rows = ([{"arm": "seth", "contact": "+1", "dead_end": d} for d in [False] * 30 + [True] * 10]
                + [{"arm": "huuman", "contact": "+1", "dead_end": d} for d in [False] * 20 + [True] * 20]
                + [{"arm": "seth", "contact": "+2", "dead_end": False}] * 40)  # +2 has no h-uman turns
        s = cq.summarize(rows, min_turns=30, min_contacts=1, seed=1)
        self.assertEqual(s["contacts_paired"], 1)
        self.assertAlmostEqual(s["dead_end_rate"]["seth"], 0.25)
        self.assertAlmostEqual(s["dead_end_rate"]["huuman"], 0.5)
        lo, hi = s["delta_ci95"]
        self.assertLessEqual(lo, 0.25)
        self.assertGreaterEqual(hi, 0.25)


class TestCli(unittest.TestCase):
    def test_insufficient_data_exits_2_and_writes_no_verdict(self):
        with tempfile.TemporaryDirectory() as d:
            fx = Fixture(d)
            fx.msg("+1", 0, "hey", True)
            fx.close()
            out = os.path.join(d, "out.json")
            rc = subprocess.run([sys.executable, os.path.join(HERE, "eval_conversation_quality.py"),
                                 "--chat-db", fx.chat_path, "--memory-db", fx.mem_path,
                                 "--since", "2026-08-01", "--out", out]).returncode
            self.assertEqual(rc, 2)
            self.assertFalse(os.path.exists(out))


if __name__ == "__main__":
    unittest.main()
