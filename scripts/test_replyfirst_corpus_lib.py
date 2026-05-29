#!/usr/bin/env python3
"""Unit tests for replyfirst_corpus_lib. Plain-runner, stdlib only, no model.
Run: python3 scripts/test_replyfirst_corpus_lib.py
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import replyfirst_corpus_lib as lib


def test_split_marker_present():
    # deliberation, then a marker, then the reply (one v4-repair shape)
    raw = "Let me think about this.\n<|channel|>final\nYeah, I'm around — what's up?"
    delib, reply = lib.split_deliberation_reply(raw, marker="<|channel|>")
    assert delib == "Let me think about this.", repr(delib)
    assert reply == "Yeah, I'm around — what's up?", repr(reply)
    print("✓ split_marker_present")


def test_split_markerless_fallback_last_paragraph_is_reply():
    raw = "I should keep this casual and short.\n\nHaha yeah totally, let's do it."
    delib, reply = lib.split_deliberation_reply(raw, marker="<|channel|>")
    assert reply == "Haha yeah totally, let's do it.", repr(reply)
    assert delib == "I should keep this casual and short.", repr(delib)
    print("✓ split_markerless_fallback_last_paragraph_is_reply")


def test_split_pure_reply_no_deliberation():
    raw = "yeah what's up"
    delib, reply = lib.split_deliberation_reply(raw, marker="<|channel|>")
    assert delib == "", repr(delib)
    assert reply == "yeah what's up", repr(reply)
    print("✓ split_pure_reply_no_deliberation")


def test_reorder_puts_reply_first_then_sentinel_then_delib():
    out = lib.reorder_to_replyfirst("the deliberation", "the reply", sentinel="<|channel|>thought")
    assert out == "the reply<|channel|>thought\nthe deliberation", repr(out)
    print("✓ reorder_puts_reply_first_then_sentinel_then_delib")


def test_reorder_empty_deliberation_keeps_boundary():
    out = lib.reorder_to_replyfirst("", "just the reply", sentinel="<|channel|>thought")
    assert out == "just the reply<|channel|>thought\n", repr(out)
    print("✓ reorder_empty_deliberation_keeps_boundary")


def test_build_target_returns_none_on_empty_reply():
    # marker at very start → no reply text → parse failure
    raw = "<|channel|>final\n"
    assert lib.build_target(raw, marker="<|channel|>", sentinel="<|channel|>thought") is None
    print("✓ build_target_returns_none_on_empty_reply")


def test_format_sft_example_text_schema():
    ex = lib.format_sft_example("USER: hi", "hey<|channel|>thought\n")
    assert ex == {"text": "USER: hi\nhey<|channel|>thought\n"}, repr(ex)
    print("✓ format_sft_example_text_schema")


def run():
    test_split_marker_present()
    test_split_markerless_fallback_last_paragraph_is_reply()
    test_split_pure_reply_no_deliberation()
    test_reorder_puts_reply_first_then_sentinel_then_delib()
    test_reorder_empty_deliberation_keeps_boundary()
    test_build_target_returns_none_on_empty_reply()
    test_format_sft_example_text_schema()
    print("\nALL replyfirst_corpus_lib TESTS PASSED")


if __name__ == "__main__":
    run()
