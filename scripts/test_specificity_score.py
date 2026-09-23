#!/usr/bin/env python3
"""Tests for the length-matched specificity gate (2026-09-22).

The claim under test is NOT "the scorer runs" — it is that length matching
actually removes the length confound that made the 2026-09-20 reading
(daemon 0.973 vs human 2.136 per reply) point at the memory pipeline when
74% of that gap was reply brevity. Each test therefore pins a contract that
FAILS if the fix is reverted.
"""
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from specificity_score import (  # noqa: E402
    bootstrap_delta_ci,
    length_matched_human,
    score,
    summarize,
)

VOCAB = {"tampa", "vanguard", "prussia"}


def test_length_matching_neutralizes_a_pure_length_confound():
    """Two corpora with IDENTICAL density but 2x length differ on the raw
    per-reply metric and agree once length-matched. If length matching is
    removed, the matched means diverge and this fails."""
    # density: exactly one insider token per 20 chars of text
    short = ["tampa " + "x" * 14] * 50                    # 20 chars, 1 token
    long_ = ["tampa " + "y" * 14 + " vanguard " + "z" * 10] * 50  # 40 chars, 2 tokens

    raw_short = summarize("s", short, VOCAB)
    raw_long = summarize("l", long_, VOCAB)
    # Raw per-reply says the short corpus is half as specific...
    assert raw_short["specific_per_reply"] == pytest.approx(1.0)
    assert raw_long["specific_per_reply"] == pytest.approx(2.0)

    # ...but length-matching the "human" (long) pool against the short corpus
    # must select only comparable-length replies. With no reply within
    # max_delta, the cohort is empty rather than silently wrong.
    matched, unmatched = length_matched_human(long_, short, max_delta=5)
    assert matched == [] and unmatched == 50, "must refuse, not fabricate, a cohort"


def test_length_matching_selects_comparable_lengths_without_replacement():
    human = [f"tampa {'x' * n}" for n in range(1, 61)]  # lengths 7..66, unique
    daemon = ["tampa " + "z" * 4] * 3                   # length 10
    matched, unmatched = length_matched_human(human, daemon, max_delta=10)
    assert unmatched == 0
    assert len(matched) == 3
    assert len(set(matched)) == 3, "sampling must be without replacement"
    for m in matched:
        assert abs(len(m) - 10) <= 10


def test_length_matching_is_deterministic():
    human = [f"tampa {'x' * n}" for n in range(1, 40)] * 2
    daemon = ["tampa " + "z" * 4, "vanguard " + "q" * 6]
    a, ua = length_matched_human(human, daemon, max_delta=8)
    b, ub = length_matched_human(human, daemon, max_delta=8)
    assert a == b and ua == ub


def test_bootstrap_ci_brackets_a_known_zero_delta_and_excludes_a_real_one():
    same = [1.0] * 200
    ci = bootstrap_delta_ci(same, same, iters=400, seed=0)
    assert ci == [0.0, 0.0], "identical corpora must yield a zero-width CI at 0"

    lo_ci = bootstrap_delta_ci([0.0] * 200, [2.0] * 200, iters=400, seed=0)
    assert lo_ci[1] < 0, "a real deficit must produce a CI strictly below zero"


def test_proper_class_requires_non_sentence_initial_capital():
    """The 8.6x casing finding depends on `proper` counting mid-sentence caps
    only. A reply naming an entity in lowercase must score 0 proper."""
    lower = score("heading to tampa later", VOCAB)
    upper = score("heading to Tampa later", VOCAB)
    assert lower["proper"] == 0
    assert upper["proper"] == 1
    # Both still count the entity as insider — content parity is case-blind.
    assert lower["insider"] == upper["insider"] == 1


def test_per_class_densities_are_reported():
    s = summarize("x", ["heading to Tampa with Vanguard folks"], VOCAB)
    for key in ("insider_per_100_chars", "concrete_per_100_chars", "proper_per_100_chars"):
        assert key in s, f"decomposition key {key} missing"
