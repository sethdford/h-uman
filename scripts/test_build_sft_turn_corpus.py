#!/usr/bin/env python3
"""Tests for build_sft_turn_corpus.py — the pure formatting and selection."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))

from build_sft_turn_corpus import (  # noqa: E402
    format_prompt, is_valid_row, keep_chat, merge_followups, turn_rows, union_rows)


def _m(text, from_me, ts, seth=None):
    m = {"text": text, "is_from_me": from_me, "timestamp": ts, "chat_id": "+15550001111",
         "contact": "+15550001111"}
    if seth is not None:
        m["is_seth"] = seth
    return m


class TestTurnRows(unittest.TestCase):
    def test_target_is_the_whole_multi_bubble_reply(self):
        window = [_m("got the job!!", False, 1000), _m("Excellent!", True, 1010),
                  _m("We gonna hang out soon?", True, 1025)]
        self.assertEqual(turn_rows(window),
                         [{"prompt": "got the job!!",
                           "completion": "Excellent!\nWe gonna hang out soon?"}])

    def test_context_is_labeled_once_it_has_more_than_one_turn(self):
        window = [_m("hey", False, 1000), _m("hi", True, 1010),
                  _m("dinner?", False, 1100), _m("Yes", True, 1120), _m("7?", True, 1130)]
        rows = turn_rows(window)
        self.assertEqual(rows[1], {"prompt": "Them: hey\nSeth: hi\nThem: dinner?",
                                   "completion": "Yes\n7?"})

    def test_daemon_turns_are_context_never_targets(self):
        window = [_m("hey", False, 1000), _m("How can I help?", True, 1005, seth=False),
                  _m("lol?", False, 1100), _m("ignore that", True, 1110, seth=True)]
        rows = turn_rows(window)
        self.assertEqual([r["completion"] for r in rows], ["ignore that"])
        self.assertIn("Seth: How can I help?", rows[0]["prompt"])

    def test_a_reply_needs_an_incoming_turn_right_before_it(self):
        # Seth texting first (no incoming turn to answer) is not a reply.
        window = [_m("you up?", True, 1000), _m("yeah", False, 1010), _m("cool", True, 1020)]
        self.assertEqual([r["completion"] for r in turn_rows(window)], ["cool"])

    def test_context_is_capped_at_five_turns(self):
        window = []
        for i in range(8):
            window.append(_m(f"them {i}", False, 1000 + i * 400))
            window.append(_m(f"seth {i}", True, 1010 + i * 400))
        last = turn_rows(window)[-1]
        self.assertEqual(last["prompt"].count("\n"), 4)

    def test_overlong_and_empty_replies_are_dropped(self):
        window = [_m("tell me", False, 1000), _m("x" * 801, True, 1010)]
        self.assertEqual(turn_rows(window), [])


class TestSelection(unittest.TestCase):
    def test_group_chats_owner_and_excluded_handles_are_dropped(self):
        skip = {"+18015550000", "jordan@example.com"}
        self.assertTrue(keep_chat("+15550001111", skip))
        self.assertFalse(keep_chat("chat123456789", skip))
        self.assertFalse(keep_chat("+18015550000", skip))
        self.assertFalse(keep_chat("jordan@example.com", skip))
        self.assertFalse(keep_chat("", skip))

    def test_valid_split_is_frozen_by_content(self):
        row = {"prompt": "hey", "completion": "hi"}
        self.assertEqual(is_valid_row(row), is_valid_row(dict(row)))
        rows = [{"prompt": f"p{i}", "completion": "c"} for i in range(2000)]
        share = sum(is_valid_row(r) for r in rows) / len(rows)
        self.assertTrue(0.03 < share < 0.08, share)


class TestFormatPrompt(unittest.TestCase):
    def test_single_incoming_turn_is_bare_text(self):
        self.assertEqual(format_prompt([{"speaker": "them", "text": "yo"}]), "yo")


class TestMergeFollowups(unittest.TestCase):
    """seth-sft-20260919 stored a second bubble as its own row whose prompt
    ends with the first bubble; 461 of its 1164 rows look like that."""

    def test_second_and_third_bubbles_fold_into_the_first(self):
        rows = [{"prompt": "got the job!!", "completion": "Excellent!"},
                {"prompt": "Seth: x\nThem: got the job!!\nSeth: Excellent!\nSeth: Proud of you",
                 "completion": "We gonna hang out soon?"},
                {"prompt": "Them: got the job!!\nSeth: Excellent!", "completion": "Proud of you"}]
        merged, dropped = merge_followups(rows)
        self.assertEqual(merged, [{"prompt": "got the job!!",
                                   "completion": "Excellent!\nProud of you\nWe gonna hang out soon?"}])
        self.assertEqual(dropped, 0)

    def test_a_followup_with_no_first_bubble_row_is_dropped_and_counted(self):
        rows = [{"prompt": "Them: hey\nSeth: hi", "completion": "what's up"},
                {"prompt": "dinner?", "completion": "Yes"}]
        merged, dropped = merge_followups(rows)
        self.assertEqual(merged, [{"prompt": "dinner?", "completion": "Yes"}])
        self.assertEqual(dropped, 1)

    def test_labeled_incoming_line_matches_bare_head(self):
        rows = [{"prompt": "Seth: earlier\nThem: dinner?", "completion": "Yes"},
                {"prompt": "Seth: earlier\nThem: dinner?\nSeth: Yes", "completion": "7?"}]
        merged, _ = merge_followups(rows)
        self.assertEqual([r["completion"] for r in merged], ["Yes\n7?"])


class TestUnion(unittest.TestCase):
    def test_fresh_rows_win_over_the_same_reply_in_the_old_corpus(self):
        fresh = [{"prompt": "Them: a\nSeth: b\nThem: dinner?", "completion": "Yes\n7?"}]
        old = [{"prompt": "dinner?", "completion": "Yes\n7?"},
               {"prompt": "movie?", "completion": "sure"}]
        self.assertEqual(union_rows(fresh, old), fresh + [old[1]])


if __name__ == "__main__":
    unittest.main()
