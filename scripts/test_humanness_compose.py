#!/usr/bin/env python3
"""Tests for humanness_compose.py — the composite + hybrid gate (AC-3, AC-8, AC-9).

Pins the contract that a single-axis collapse FAILs even when the composite is
high, plus weight redistribution, composite floor, and the bootstrap SKIP.

Run: python3 scripts/test_humanness_compose.py
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import humanness_compose as hc  # noqa: E402


def _axes(fid, anti, judge, rel, judge_avail=True, rel_avail=True):
    return {
        "fidelity": {"mean": fid, "n": 20},
        "anti_ai": {"mean": anti, "n": 20},
        "judge": {"mean": judge, "n": 20, "available": judge_avail},
        "relationship": {"mean": rel, "n": 12, "available": rel_avail},
    }


def test_all_green_passes():
    axes = _axes(0.85, 0.88, 0.80, 0.78)
    baseline = {"fidelity": 0.82, "anti_ai": 0.86, "judge": 0.77,
                "relationship": 0.75, "composite": 0.80}
    r = hc.compose(axes, baseline)
    assert r["humanness_verdict"] == "PASS", r["gate"]["reason"]


def test_one_axis_collapse_fails_even_with_high_composite():
    """The headline AC-3 contract: relationship collapses but the other three
    are so strong the composite stays above the floor — must still FAIL."""
    axes = _axes(0.97, 0.98, 0.96, 0.40)  # relationship tanks
    baseline = {"fidelity": 0.82, "anti_ai": 0.86, "judge": 0.77,
                "relationship": 0.75, "composite": 0.80}
    composite, _ = hc.compute_composite(axes)
    assert composite >= hc.DEFAULT_COMPOSITE_FLOOR, "composite should stay high (masking risk)"
    r = hc.compose(axes, baseline)
    assert r["humanness_verdict"] == "FAIL", "one-axis collapse must FAIL"
    assert "relationship regressed" in r["gate"]["reason"]


def test_composite_floor_breach_fails():
    axes = _axes(0.55, 0.55, 0.55, 0.55)  # all uniformly low, none "regressed" much
    baseline = {"fidelity": 0.54, "anti_ai": 0.54, "judge": 0.54,
                "relationship": 0.54, "composite": 0.54}
    r = hc.compose(axes, baseline)
    assert r["humanness_verdict"] == "FAIL"
    assert "composite" in r["gate"]["reason"]


def test_no_baseline_skips():
    axes = _axes(0.85, 0.88, 0.80, 0.78)
    r = hc.compose(axes, baseline=None)
    assert r["humanness_verdict"] == "SKIP"


def test_judge_unavailable_redistributes_weight():
    axes = _axes(0.80, 0.80, 0.0, 0.80, judge_avail=False)
    composite, used = hc.compute_composite(axes)
    assert "judge" not in used, "unavailable judge must be dropped from weights"
    assert abs(sum(used.values()) - 1.0) < 1e-9, "weights must renormalize to 1.0"
    # With judge gone and the other three all 0.80, composite is exactly 0.80.
    assert abs(composite - 0.80) < 1e-9, composite


def test_baseline_window_bootstrap(tmp_path_str):
    """Fewer than `window` history rows -> None (bootstrap)."""
    p = Path(tmp_path_str) / "baseline.jsonl"
    p.write_text('{"axes":{"fidelity":0.8},"composite":0.8}\n' * 3)
    assert hc.load_baseline(str(p), window=7) is None
    # Enough rows -> a baseline dict.
    p.write_text('{"axes":{"fidelity":0.8,"anti_ai":0.8,"judge":0.8,"relationship":0.8},'
                 '"composite":0.8}\n' * 7)
    b = hc.load_baseline(str(p), window=7)
    assert b is not None and abs(b["composite"] - 0.8) < 1e-9


def main():
    import tempfile
    failures = 0
    tests = [
        test_all_green_passes,
        test_one_axis_collapse_fails_even_with_high_composite,
        test_composite_floor_breach_fails,
        test_no_baseline_skips,
        test_judge_unavailable_redistributes_weight,
    ]
    for t in tests:
        try:
            t()
            print(f"  PASS  {t.__name__}")
        except AssertionError as e:
            failures += 1
            print(f"  FAIL  {t.__name__}: {e}")
    # baseline test needs a temp dir
    with tempfile.TemporaryDirectory() as d:
        try:
            test_baseline_window_bootstrap(d)
            print("  PASS  test_baseline_window_bootstrap")
        except AssertionError as e:
            failures += 1
            print(f"  FAIL  test_baseline_window_bootstrap: {e}")

    total = len(tests) + 1
    print(f"--- humanness_compose: {total - failures}/{total} passed ---")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
