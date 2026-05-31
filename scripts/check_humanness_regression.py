#!/usr/bin/env python3
"""
Humanness regression checker for CI.

Reads a humanness scores JSON file and compares against the baseline,
reporting pass/fail and which metrics regressed.

Regression = current < expected - threshold for any metric.

Used by both the deterministic PR gate (required) and the scheduled
secrets-gated live-judge job (advisory).
"""

import json
import sys
from pathlib import Path


def get_project_root():
    """Return the absolute path to the project root."""
    script_dir = Path(__file__).resolve().parent
    return script_dir.parent


def load_baseline(baseline_path):
    """Load the humanness baseline JSON."""
    with open(baseline_path, "r") as f:
        return json.load(f)


def load_scores(scores_path):
    """Load the scores JSON emitted by the runner."""
    with open(scores_path, "r") as f:
        return json.load(f)


def check_regressions(baseline, scores):
    """
    Check if any metrics have regressed.

    Returns (pass, regressions) where:
      - pass: bool indicating overall pass/fail
      - regressions: list of {"metric": name, "expected": val, "threshold": val, "current": val, "delta": val}
    """
    regressions = []

    metrics_baseline = baseline.get("metrics", {})
    metrics_scores = scores.get("metrics", {})

    for metric_name, baseline_config in metrics_baseline.items():
        if metric_name not in metrics_scores:
            # Metric missing from current scores (could be advisory scorer unavailable)
            # Don't fail on missing metrics from the scheduled tier
            continue

        expected = baseline_config.get("expected", 0.0)
        threshold = baseline_config.get("threshold", 0.05)
        current = metrics_scores[metric_name]

        # Regression = current < expected - threshold
        regression_threshold = expected - threshold
        if current < regression_threshold:
            regressions.append({
                "metric": metric_name,
                "expected": expected,
                "threshold": threshold,
                "current": current,
                "delta": current - expected,
                "regression_threshold": regression_threshold,
            })

    pass_all = len(regressions) == 0
    return pass_all, regressions


def print_results(pass_all, regressions):
    """Print human-readable results."""
    if pass_all:
        print("✓ humanness baseline check PASSED")
        return 0
    else:
        print("✗ humanness baseline check FAILED")
        for r in regressions:
            print(f"  {r['metric']}: {r['current']:.4f} (expected {r['expected']:.4f} - {r['threshold']:.4f} = {r['regression_threshold']:.4f})")
            print(f"    delta: {r['delta']:+.4f}")
        return 1


def selftest_mode():
    """
    Run self-tests covering pass / single-metric-regress / missing-metric.

    Exits 0 if all tests pass, 1 if any test fails.
    """
    print("[selftest] running humanness regression checker self-tests...", file=sys.stderr)

    all_pass = True

    # Test 1: Pass case (all metrics meet threshold)
    baseline_pass = {
        "metrics": {
            "metric_a": {"expected": 0.8, "threshold": 0.05},
            "metric_b": {"expected": 0.7, "threshold": 0.1},
        }
    }
    scores_pass = {
        "metrics": {
            "metric_a": 0.8,
            "metric_b": 0.7,
        }
    }
    pass_result, regressions = check_regressions(baseline_pass, scores_pass)
    if not pass_result or regressions:
        print("[selftest] FAIL: pass case should have no regressions", file=sys.stderr)
        all_pass = False
    else:
        print("[selftest] PASS: all-metrics-ok case", file=sys.stderr)

    # Test 2: Single metric regression
    baseline_regress = {
        "metrics": {
            "metric_a": {"expected": 0.8, "threshold": 0.05},
            "metric_b": {"expected": 0.7, "threshold": 0.1},
        }
    }
    scores_regress = {
        "metrics": {
            "metric_a": 0.74,  # Below 0.8 - 0.05 = 0.75
            "metric_b": 0.7,   # OK
        }
    }
    pass_result, regressions = check_regressions(baseline_regress, scores_regress)
    if pass_result or len(regressions) != 1:
        print("[selftest] FAIL: single-metric-regression case should detect metric_a", file=sys.stderr)
        all_pass = False
    elif regressions[0]["metric"] != "metric_a":
        print(f"[selftest] FAIL: expected metric_a regression, got {regressions[0]['metric']}", file=sys.stderr)
        all_pass = False
    else:
        print("[selftest] PASS: single-metric-regression case detected", file=sys.stderr)

    # Test 3: Missing metric (from advisory scorer)
    baseline_with_fidelity = {
        "metrics": {
            "metric_a": {"expected": 0.8, "threshold": 0.05},
            "optional_fidelity": {"expected": 0.75, "threshold": 0.1},
        }
    }
    scores_missing = {
        "metrics": {
            "metric_a": 0.8,
            # optional_fidelity is missing (wasn't computed)
        }
    }
    pass_result, regressions = check_regressions(baseline_with_fidelity, scores_missing)
    if not pass_result:
        print("[selftest] FAIL: missing-metric case should not fail (advisory scorer)", file=sys.stderr)
        all_pass = False
    else:
        print("[selftest] PASS: missing-metric case (advisory scorer unavailable)", file=sys.stderr)

    if all_pass:
        print("[selftest] all self-tests passed ✓", file=sys.stderr)
        return 0
    else:
        print("[selftest] some self-tests failed ✗", file=sys.stderr)
        return 1


def main():
    """Main entry point."""
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        return selftest_mode()

    if len(sys.argv) < 2:
        print("Usage: check_humanness_regression.py <scores.json> [--baseline <baseline.json>]", file=sys.stderr)
        print("", file=sys.stderr)
        print("Compares current humanness scores against baseline, reports regressions.", file=sys.stderr)
        print("Exits 0 if no regressions, 1 if any metric regressed.", file=sys.stderr)
        return 1

    scores_path = Path(sys.argv[1])
    if not scores_path.exists():
        print(f"[check] scores file not found: {scores_path}", file=sys.stderr)
        return 1

    project_root = get_project_root()
    baseline_path = project_root / "docs" / "evaluation" / "humanness-baseline.json"

    # Allow override via --baseline flag
    for i, arg in enumerate(sys.argv[2:]):
        if arg == "--baseline" and i + 2 < len(sys.argv):
            baseline_path = Path(sys.argv[i + 3])

    if not baseline_path.exists():
        print(f"[check] baseline file not found: {baseline_path}", file=sys.stderr)
        return 1

    try:
        baseline = load_baseline(baseline_path)
        scores = load_scores(scores_path)
    except json.JSONDecodeError as e:
        print(f"[check] JSON parse error: {e}", file=sys.stderr)
        return 1
    except Exception as e:
        print(f"[check] error: {e}", file=sys.stderr)
        return 1

    pass_all, regressions = check_regressions(baseline, scores)
    return print_results(pass_all, regressions)


if __name__ == "__main__":
    sys.exit(main())
