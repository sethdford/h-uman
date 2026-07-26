#!/usr/bin/env python3
"""Pins for synthetic_judge.parse() against the gemma thought-channel traps.

The 2026-07-25 GLM-vs-v5 judging run produced 320/320 silent parse failures
from two compounding issues this file pins:
  1. The judge prompt's answer schema ('{"real":"A"|"B",...}') is echoed by
     the model's reasoning channel; a first-match JSON scan (or a bare
     '"real":"A"' regex) harvests the ECHO, not the verdict.
  2. The stock mlx_lm server splits thinking into message.reasoning and can
     omit "content" entirely on a length cut, so call() falls back to
     scanning reasoning text — where the echo always precedes any verdict.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from synthetic_judge import parse


def test_schema_echo_alone_yields_no_verdict():
    echo = 'I must reply with {"real":"A"|"B","opinion":1-5} strict JSON...'
    assert parse(echo) is None, f"schema echo harvested as verdict: {parse(echo)}"
    print("ok schema_echo_alone_yields_no_verdict")


def test_verdict_after_echo_wins():
    txt = ('The format is {"real":"A"|"B","opinion":1-5}. Considering tone...\n'
           '{"real":"B","opinion":4,"memory":3,"reasoning":4,'
           '"lexical":5,"tone":4,"syntax":5}')
    assert parse(txt)["real"] == "B", parse(txt)
    print("ok verdict_after_echo_wins")


def test_clean_answer_parses():
    txt = ('{"real":"A","opinion":5,"memory":5,"reasoning":5,'
           '"lexical":5,"tone":5,"syntax":5}')
    j = parse(txt)
    assert j["real"] == "A" and j["opinion"] == 5
    print("ok clean_answer_parses")


def test_fenced_answer_parses():
    txt = ('```json\n{"real":"A","opinion":5,"memory":5,"reasoning":5,'
           '"lexical":5,"tone":5,"syntax":5}\n```')
    assert parse(txt)["real"] == "A"
    print("ok fenced_answer_parses")


def test_prose_fallback_takes_last_real_not_echo():
    txt = 'Schema says "real":"A"|"B". After comparing, real=B is my judgment.'
    assert parse(txt)["real"] == "B", parse(txt)
    print("ok prose_fallback_takes_last_real_not_echo")


def test_no_verdict_returns_none():
    assert parse("I could not decide between these options.") is None
    print("ok no_verdict_returns_none")


if __name__ == "__main__":
    fails = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            try:
                fn()
            except AssertionError as e:
                print(f"FAIL {name}: {e}")
                fails += 1
    print(f"{'FAILED' if fails else 'PASSED'} ({fails} failures)")
    sys.exit(1 if fails else 0)
