#!/usr/bin/env python3
"""Unit tests for eval_ordering metrics + verdict. Stdlib only, no model.
Run: python3 scripts/test_eval_ordering.py
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import eval_ordering as eo

S = "<|channel|>thought"


def test_reply_first_true_when_reply_precedes_sentinel():
    assert eo.is_reply_first("yeah what's up" + S + "\nthinking", sentinel=S) is True
    print("✓ reply_first_true_when_reply_precedes_sentinel")


def test_reply_first_true_when_no_sentinel():
    assert eo.is_reply_first("yeah what's up", sentinel=S) is True
    print("✓ reply_first_true_when_no_sentinel")


def test_reply_first_false_when_sentinel_at_start():
    assert eo.is_reply_first(S + "\ndeliberating first", sentinel=S) is False
    print("✓ reply_first_false_when_sentinel_at_start")


def test_first_reply_token_index_zero_for_reply_first():
    assert eo.first_reply_token_index("hey there" + S + "\nx", sentinel=S) == 0
    print("✓ first_reply_token_index_zero_for_reply_first")


def test_first_reply_token_index_penalty_for_delib_first():
    g = S + "\none two three four five"
    assert eo.first_reply_token_index(g, sentinel=S) >= 5
    print("✓ first_reply_token_index_penalty_for_delib_first")


def test_build_verdict_pass_when_thresholds_met():
    gens = ["hi" + S + "\nx", "yo" + S + "\ny", "sup"]  # 3/3 reply-first
    v = eo.build_verdict(gens, sentinel=S, adapter_path="/x", floor=0.90, max_idx=8)
    assert v["pct_reply_first"] == 1.0, v
    assert v["gate"]["ordering_pass"] is True, v
    assert v["verdict"] == "PASS" and v["exit_code"] == 0, v
    print("✓ build_verdict_pass_when_thresholds_met")


def test_build_verdict_fail_when_below_floor():
    gens = [S + "\nbad", S + "\nbad", "good" + S + "\nx"]  # 1/3 reply-first
    v = eo.build_verdict(gens, sentinel=S, adapter_path="/x", floor=0.90, max_idx=8)
    assert v["gate"]["ordering_pass"] is False, v
    assert v["verdict"] == "FAIL" and v["exit_code"] == 1, v
    print("✓ build_verdict_fail_when_below_floor")


def run():
    test_reply_first_true_when_reply_precedes_sentinel()
    test_reply_first_true_when_no_sentinel()
    test_reply_first_false_when_sentinel_at_start()
    test_first_reply_token_index_zero_for_reply_first()
    test_first_reply_token_index_penalty_for_delib_first()
    test_build_verdict_pass_when_thresholds_met()
    test_build_verdict_fail_when_below_floor()
    print("\nALL eval_ordering TESTS PASSED")


if __name__ == "__main__":
    run()
