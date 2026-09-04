#!/usr/bin/env python3
"""Tests for scripts/eval_reply_delay_model.py — the time-split held-out
evaluation follow-up to Contract C5, Part C. Synthetic data only; no
chat.db touched."""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from eval_reply_delay_model import (  # noqa: E402
    abs_log_error,
    bootstrap_ci_paired_diff,
    compute_metrics,
    predict_point,
    temporal_split,
)
from fit_reply_delay_model import fit  # noqa: E402


def make_sample(ts, hour, len_chars, contact, delay_secs):
    return {"ts": ts, "hour": hour, "len_chars": len_chars, "contact": contact, "delay_secs": delay_secs}


def test_temporal_split_is_time_ordered_and_respects_frac():
    # Deliberately built out of order to prove the split SORTS first.
    samples = [make_sample(ts, 10, 10, "c1", 30) for ts in [50, 10, 40, 20, 30]]
    train, test = temporal_split(samples, train_frac=0.6)
    assert [s["ts"] for s in train] == [10, 20, 30]
    assert [s["ts"] for s in test] == [40, 50]
    print("✓ temporal_split sorts by ts and splits at the right fraction")


def test_temporal_split_train_always_precedes_test():
    samples = [make_sample(ts, 10, 10, "c1", 30) for ts in range(100)]
    train, test = temporal_split(samples, train_frac=0.8)
    assert len(train) == 80 and len(test) == 20
    assert max(s["ts"] for s in train) < min(s["ts"] for s in test)
    print("✓ every train ts precedes every test ts (no leakage across the boundary)")


def _toy_model():
    """A hand-built model dict shaped like fit()'s output, small enough to
    reason about by hand."""
    return {
        "length_bucket_thresholds": {"lo_chars": 40, "hi_chars": 160},
        "freq_tercile_boundaries": {"lo_count": 5, "hi_count": 20},
        "cells": {
            "h10_lshort_flow": {"n": 10, "quantiles": {"p10": 10, "p25": 20, "p50": 30, "p75": 60, "p90": 120}},
        },
        "hour_len_marginals": {
            "h10_lshort": {"n": 20, "quantiles": {"p10": 15, "p25": 25, "p50": 45, "p75": 70, "p90": 130}},
        },
        "hour_marginals": {
            "h10": {"n": 50, "quantiles": {"p10": 20, "p25": 40, "p50": 90, "p75": 200, "p90": 500}},
        },
        "global": {"n": 500, "quantiles": {"p10": 30, "p25": 60, "p50": 150, "p75": 400, "p90": 900}},
    }


def test_predict_point_exact_cell():
    model = _toy_model()
    pred, n = predict_point(model, hour=10, len_chars=5, freq=2.0)  # short, low
    assert pred == 30 and n == 10
    print("✓ predict_point hits the exact cell when available")


def test_predict_point_falls_back_hour_len_then_hour_then_global():
    model = _toy_model()
    # freq=50 -> "high", no h10_lshort_fhigh cell -> falls back to h10_lshort
    pred, n = predict_point(model, hour=10, len_chars=5, freq=50.0)
    assert pred == 45 and n == 20
    # len=500 -> "long", no long cell/marginal at hour 10 -> falls back to h10
    pred, n = predict_point(model, hour=10, len_chars=500, freq=2.0)
    assert pred == 90 and n == 50
    # hour=5 has nothing at all -> global
    pred, n = predict_point(model, hour=5, len_chars=5, freq=2.0)
    assert pred == 150 and n == 500
    print("✓ predict_point falls back hour_len -> hour -> global correctly")


def test_abs_log_error_zero_when_pred_equals_actual():
    assert abs_log_error(100.0, 100.0) == 0.0
    print("✓ abs_log_error is 0 for an exact prediction")


def test_abs_log_error_floors_at_one_second():
    # Neither pred nor actual may be 0 inside the log — both floor to 1.
    e1 = abs_log_error(0.0, 0.0)
    assert e1 == 0.0  # both floor to 1 -> ln(1/1) = 0
    e2 = abs_log_error(0.0, 100.0)
    import math
    assert abs(e2 - math.log(100.0)) < 1e-9
    print("✓ abs_log_error floors both sides at 1s (no log(0), no divide-by-zero)")


def test_compute_metrics_recovers_known_errors():
    model = _toy_model()
    # h10_lshort_flow predicts 30 (exact cell); global predicts 150.
    test_samples = [
        make_sample(1, 10, 5, "c1", 30),   # exact match for model
        make_sample(2, 10, 5, "c1", 130),  # model off by 100, global off by 20
    ]
    contact_counts = {"c1": 2}  # <=5 -> "low" tercile
    m = compute_metrics(test_samples, model, contact_counts)
    assert m["ae_model"] == [0, 100]
    assert m["ae_global"] == [120, 20]
    assert abs(m["mae_model"] - 50.0) < 1e-9
    assert abs(m["mae_global"] - 70.0) < 1e-9
    print("✓ compute_metrics produces the hand-computed MAE for a 2-sample case")


def test_bootstrap_ci_paired_diff_deterministic_for_seed():
    a = [1.0, 2.0, 3.0, 4.0, 5.0]
    b = [1.0, 1.0, 1.0, 1.0, 1.0]
    m1, lo1, hi1 = bootstrap_ci_paired_diff(a, b, n_bootstrap=500, seed=42)
    m2, lo2, hi2 = bootstrap_ci_paired_diff(a, b, n_bootstrap=500, seed=42)
    assert (m1, lo1, hi1) == (m2, lo2, hi2)
    assert abs(m1 - 2.0) < 1e-9  # mean(a-b) = mean([0,1,2,3,4]) = 2.0
    assert lo1 <= m1 <= hi1
    print("✓ bootstrap_ci_paired_diff is deterministic per seed and centers near the true mean diff")


def test_bootstrap_ci_paired_diff_narrows_around_zero_when_a_equals_b():
    a = [5.0] * 50
    b = [5.0] * 50
    m, lo, hi = bootstrap_ci_paired_diff(a, b, n_bootstrap=1000, seed=1)
    assert m == 0.0 and lo == 0.0 and hi == 0.0
    print("✓ identical paired series give a degenerate CI of exactly 0")


def test_fit_on_train_then_predict_on_test_end_to_end():
    """Small end-to-end sanity check: fit() on a train split, then
    predict_point() on held-out samples from the SAME hour/length/freq
    cell should land close to the true generating value."""
    samples = []
    ts = 1000
    for i in range(30):
        samples.append(make_sample(ts, 14, 10, "c1", 50 + (i % 5)))  # tight cluster near 50-54
        ts += 10
    train, test = temporal_split(samples, train_frac=0.8)
    assert len(train) == 24 and len(test) == 6

    contact_counts = {"c1": 30}
    model = fit(train, contact_counts, min_cell_n=5)
    assert model is not None

    pred, n = predict_point(model, hour=14, len_chars=10, freq=30)
    assert 45 <= pred <= 60, f"prediction {pred} should track the tight 50-54 cluster"
    assert n >= 5
    print(f"✓ fit(train) -> predict_point(test) tracks the generating distribution (pred={pred})")


def main():
    tests = [
        test_temporal_split_is_time_ordered_and_respects_frac,
        test_temporal_split_train_always_precedes_test,
        test_predict_point_exact_cell,
        test_predict_point_falls_back_hour_len_then_hour_then_global,
        test_abs_log_error_zero_when_pred_equals_actual,
        test_abs_log_error_floors_at_one_second,
        test_compute_metrics_recovers_known_errors,
        test_bootstrap_ci_paired_diff_deterministic_for_seed,
        test_bootstrap_ci_paired_diff_narrows_around_zero_when_a_equals_b,
        test_fit_on_train_then_predict_on_test_end_to_end,
    ]
    print("=" * 60)
    print("Testing eval_reply_delay_model.py")
    print("=" * 60)
    passed = failed = 0
    for t in tests:
        try:
            t()
            passed += 1
        except AssertionError as e:
            print(f"✗ {t.__name__}: {e}")
            failed += 1
        except Exception as e:
            print(f"✗ {t.__name__}: {type(e).__name__}: {e}")
            failed += 1
    print("=" * 60)
    print(f"Results: {passed} passed, {failed} failed")
    print("=" * 60)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
