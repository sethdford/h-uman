#!/usr/bin/env python3
"""
Tests for trajectory_gate.py — the pure curve-gate at the heart of the
longitudinal personalization SOTA claim.

Every branch is exercised with synthetic generation lists (no model). The
load-bearing test is scale20_replay_fails_on_base_capability: a generation
whose fidelity RISES but whose base-capability COLLAPSES must FAIL the gate on
the base_capability axis — that is the gate proving it is honest, not just a
fidelity cheerleader (lora-scale-default-or-die.md).
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from trajectory_gate import (
    TrajectoryGateConfig,
    evaluate_trajectory_gate,
)


def _gen(i, fidelity, base_capability, ci_half=0.03):
    return {
        "gen": i,
        "fidelity_mean": fidelity,
        "base_capability": base_capability,
        "fidelity_ci": [fidelity - ci_half, fidelity + ci_half],
    }


def test_rising_curve_base_preserved_passes():
    """Classic happy path: fidelity rises, base-capability holds → PASS."""
    gens = [
        _gen(0, 0.586, 0.94),   # base
        _gen(1, 0.856, 0.93),   # v4-repair: big fidelity gain, base intact
    ]
    res = evaluate_trajectory_gate(gens)
    assert res.verdict == "PASS", f"expected PASS, got {res.verdict}: {res.details}"
    assert res.failing_axis is None
    print(f"✓ rising curve + base preserved → PASS")


def test_scale20_replay_fails_on_base_capability():
    """THE honesty test. gen1 lifts fidelity (0.586→0.90) but tanks base
    capability (0.94→0.55) — the scale=20 incident shape. The gate MUST fail
    on the base_capability axis, naming gen 1, NOT be fooled by the fidelity
    rise."""
    gens = [
        _gen(0, 0.586, 0.94),
        _gen(1, 0.90, 0.55),   # voice up, instruction-following destroyed
    ]
    res = evaluate_trajectory_gate(gens)
    assert res.verdict == "FAIL", f"scale=20 replay must FAIL, got {res.verdict}"
    assert res.failing_axis == "base_capability", (
        f"must fail on base_capability axis, got {res.failing_axis}"
    )
    assert res.failing_gen == 1, f"must name gen 1, got {res.failing_gen}"
    print(f"✓ scale=20 replay (fidelity up, base down) → FAIL on base_capability")


def test_fidelity_regression_beyond_stderr_fails_curve():
    """A later generation that drops fidelity more than 1 stderr below the
    running max fails on the curve axis."""
    # ci_half 0.03 → stderr ≈ 0.06/(2*1.96) ≈ 0.0153; running max 0.86.
    # gen2 fidelity 0.70 is ~10 stderr below → curve FAIL.
    gens = [
        _gen(0, 0.586, 0.94),
        _gen(1, 0.86, 0.93),
        _gen(2, 0.70, 0.93),
    ]
    res = evaluate_trajectory_gate(gens)
    assert res.verdict == "FAIL", f"expected FAIL, got {res.verdict}"
    assert res.failing_axis == "curve", f"expected curve axis, got {res.failing_axis}"
    assert res.failing_gen == 2, f"expected gen 2, got {res.failing_gen}"
    print(f"✓ fidelity regression > 1 stderr below running max → FAIL on curve")


def test_small_dip_within_stderr_still_passes():
    """A tiny dip within CI tolerance does NOT fail — adaptation is noisy and
    strict monotonicity would false-fail. Wide CI (ci_half 0.10 → stderr
    ≈ 0.051) absorbs a 0.02 dip below the running max."""
    gens = [
        _gen(0, 0.70, 0.94, ci_half=0.10),
        _gen(1, 0.86, 0.93, ci_half=0.10),
        _gen(2, 0.85, 0.93, ci_half=0.10),  # 0.01 below max, within 1 stderr
    ]
    res = evaluate_trajectory_gate(gens)
    assert res.verdict == "PASS", f"within-CI dip must PASS, got {res.verdict}: {res.details}"
    print(f"✓ small dip within 1 stderr → still PASS (noise-tolerant)")


def test_final_below_floor_fails():
    """Even a monotone-rising, base-preserving curve fails if the newest
    generation is below the published fidelity floor (default 0.80)."""
    gens = [
        _gen(0, 0.50, 0.94),
        _gen(1, 0.70, 0.93),  # rising, base ok, but 0.70 < 0.80 floor
    ]
    res = evaluate_trajectory_gate(gens)
    assert res.verdict == "FAIL", f"expected FAIL, got {res.verdict}"
    assert res.failing_axis == "final_floor", f"expected final_floor, got {res.failing_axis}"
    assert res.failing_gen == 1
    print(f"✓ final gen below 0.80 floor → FAIL on final_floor")


def test_single_generation_skips():
    """A single generation has no curve to judge → SKIP, not PASS/FAIL."""
    res = evaluate_trajectory_gate([_gen(0, 0.586, 0.94)])
    assert res.verdict == "SKIP", f"expected SKIP, got {res.verdict}"
    print(f"✓ single generation → SKIP")


def test_empty_skips():
    """No generations → SKIP."""
    res = evaluate_trajectory_gate([])
    assert res.verdict == "SKIP", f"expected SKIP, got {res.verdict}"
    print(f"✓ empty trajectory → SKIP")


def test_out_of_order_input_is_sorted():
    """Generations supplied out of order are evaluated in gen-index order so a
    caller can't perturb the gate by appending out of sequence."""
    gens = [
        _gen(1, 0.856, 0.93),
        _gen(0, 0.586, 0.94),  # supplied second but is the base
    ]
    res = evaluate_trajectory_gate(gens)
    assert res.verdict == "PASS", f"expected PASS after sort, got {res.verdict}: {res.details}"
    print(f"✓ out-of-order generations sorted before evaluation")


def test_epsilon_boundary_base_capability():
    """Base-capability exactly at gen0 - epsilon passes; just below fails.
    gen0 base 0.94, eps 0.05 → floor 0.89."""
    cfg = TrajectoryGateConfig()
    at_floor = evaluate_trajectory_gate(
        [_gen(0, 0.586, 0.94), _gen(1, 0.86, 0.89)], cfg
    )
    assert at_floor.verdict == "PASS", f"at floor must PASS, got {at_floor.verdict}"
    below = evaluate_trajectory_gate(
        [_gen(0, 0.586, 0.94), _gen(1, 0.86, 0.889)], cfg
    )
    assert below.verdict == "FAIL" and below.failing_axis == "base_capability"
    print(f"✓ base-capability epsilon boundary: 0.89 PASS, 0.889 FAIL")


def test_stderr_from_explicit_field_overrides_ci():
    """An explicit fidelity_stderr is honored over a derived CI."""
    gens = [
        {"gen": 0, "fidelity_mean": 0.586, "base_capability": 0.94},
        # large explicit stderr makes a 0.10 dip tolerable
        {"gen": 1, "fidelity_mean": 0.86, "base_capability": 0.93, "fidelity_stderr": 0.20},
        {"gen": 2, "fidelity_mean": 0.80, "base_capability": 0.93, "fidelity_stderr": 0.20},
    ]
    res = evaluate_trajectory_gate(gens)
    assert res.verdict == "PASS", f"explicit stderr should absorb dip, got {res.verdict}: {res.details}"
    print(f"✓ explicit fidelity_stderr honored over derived CI")


def main():
    tests = [
        test_rising_curve_base_preserved_passes,
        test_scale20_replay_fails_on_base_capability,
        test_fidelity_regression_beyond_stderr_fails_curve,
        test_small_dip_within_stderr_still_passes,
        test_final_below_floor_fails,
        test_single_generation_skips,
        test_empty_skips,
        test_out_of_order_input_is_sorted,
        test_epsilon_boundary_base_capability,
        test_stderr_from_explicit_field_overrides_ci,
    ]
    print("=" * 60)
    print("Testing trajectory_gate.py")
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
