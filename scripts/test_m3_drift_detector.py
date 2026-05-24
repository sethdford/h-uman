#!/usr/bin/env python3
"""
Phase E4 verifier — pins drift detection thresholds.

Tests:
  1. NEEDS_ROLLBACK fires when guard REJECT rate jumps >5 pts
  2. DEGRADING fires when guard REWRITE rate jumps >10 pts
  3. DEGRADING fires when avg completion tokens drops >40%
  4. OK with "insufficient data" when current window is empty
  5. windows_from_lineage chains timestamps correctly
  6. bucket_outcomes filters by window time correctly

Run: python3 scripts/test_m3_drift_detector.py
"""
from __future__ import annotations

import importlib.util
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

spec = importlib.util.spec_from_file_location(
    "m3_drift_detector", REPO_ROOT / "scripts" / "m3_drift_detector.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


_PASS = 0
_FAIL = 0


def _ok(name: str, cond: bool, detail: str = ""):
    global _PASS, _FAIL
    if cond:
        _PASS += 1
        print(f"  PASS  {name}")
    else:
        _FAIL += 1
        print(f"  FAIL  {name}  {detail}")


def _make_outcomes(start_ts: int, count: int, guard: int = 1, ct: int = 50):
    """Build a list of synthetic outcomes for a window."""
    return [{"t": start_ts + i, "g": guard, "ct": ct, "l": 200,
             "ch": (i % 3) + 1} for i in range(count)]


def test_needs_rollback_on_reject_spike():
    print("\n--- test_needs_rollback_on_reject_spike ---")
    # Prior adapter window: 100 outcomes, 5% reject
    # Current adapter window: 100 outcomes, 15% reject (+10 pts; threshold is 5)
    lineage = [
        {"adapter_path": "/a/prior.bin", "timestamp": 1000, "kind": "lora-persona"},
        {"adapter_path": "/a/current.bin", "timestamp": 2000, "kind": "lora-persona"},
    ]
    outcomes = (_make_outcomes(1000, 95, guard=1) + _make_outcomes(1095, 5, guard=3) +
                _make_outcomes(2000, 85, guard=1) + _make_outcomes(2085, 15, guard=3))
    windows = m.windows_from_lineage(lineage, end_ms=3000)
    windowed = m.bucket_outcomes(outcomes, windows)
    diag = m.diagnose_drift(windowed, lineage)
    _ok("status=NEEDS_ROLLBACK", diag["status"] == "NEEDS_ROLLBACK",
        f"got {diag['status']}: {diag['reasons']}")
    _ok("rollback hint points at prior",
        diag["rollback_hint"] == "/a/prior.bin")


def test_degrading_on_rewrite_drift():
    print("\n--- test_degrading_on_rewrite_drift ---")
    lineage = [
        {"adapter_path": "/a/prior.bin", "timestamp": 1000, "kind": "x"},
        {"adapter_path": "/a/current.bin", "timestamp": 2000, "kind": "x"},
    ]
    # Prior: 95% pass, 5% rewrite. Current: 80% pass, 20% rewrite (+15 pts).
    outcomes = (_make_outcomes(1000, 95, guard=1) + _make_outcomes(1095, 5, guard=2) +
                _make_outcomes(2000, 80, guard=1) + _make_outcomes(2080, 20, guard=2))
    windows = m.windows_from_lineage(lineage, end_ms=3000)
    windowed = m.bucket_outcomes(outcomes, windows)
    diag = m.diagnose_drift(windowed, lineage)
    _ok("status=DEGRADING", diag["status"] == "DEGRADING",
        f"got {diag['status']}: {diag['reasons']}")
    _ok("no rollback hint for DEGRADING (advisor mode)",
        diag.get("rollback_hint") is None)


def test_degrading_on_completion_token_drop():
    print("\n--- test_degrading_on_completion_token_drop ---")
    lineage = [
        {"adapter_path": "/a/prior.bin", "timestamp": 1000, "kind": "x"},
        {"adapter_path": "/a/current.bin", "timestamp": 2000, "kind": "x"},
    ]
    # Prior: avg 100 ct. Current: avg 30 ct (70% drop; threshold is 40%).
    outcomes = (_make_outcomes(1000, 50, ct=100) +
                _make_outcomes(2000, 50, ct=30))
    windows = m.windows_from_lineage(lineage, end_ms=3000)
    windowed = m.bucket_outcomes(outcomes, windows)
    diag = m.diagnose_drift(windowed, lineage)
    _ok("status=DEGRADING", diag["status"] == "DEGRADING",
        f"got {diag['status']}: {diag['reasons']}")
    _ok("reason mentions completion tokens",
        any("completion tokens" in r for r in diag["reasons"]))


def test_ok_when_current_window_empty():
    print("\n--- test_ok_when_current_window_empty ---")
    lineage = [
        {"adapter_path": "/a/prior.bin", "timestamp": 1000, "kind": "x"},
        {"adapter_path": "/a/current.bin", "timestamp": 2000, "kind": "x"},
    ]
    # Outcomes only for the PRIOR adapter. Current adapter has no traffic yet.
    outcomes = _make_outcomes(1000, 50)
    windows = m.windows_from_lineage(lineage, end_ms=3000)
    windowed = m.bucket_outcomes(outcomes, windows)
    diag = m.diagnose_drift(windowed, lineage)
    _ok("status=OK", diag["status"] == "OK")
    _ok("reason mentions newly-promoted",
        any("newly-promoted" in r for r in diag["reasons"]))


def test_ok_when_only_one_adapter():
    print("\n--- test_ok_when_only_one_adapter ---")
    lineage = [{"adapter_path": "/a/only.bin", "timestamp": 1000, "kind": "x"}]
    outcomes = _make_outcomes(1000, 50)
    windows = m.windows_from_lineage(lineage, end_ms=3000)
    windowed = m.bucket_outcomes(outcomes, windows)
    diag = m.diagnose_drift(windowed, lineage)
    _ok("status=OK with only-one-adapter note", diag["status"] == "OK")


def test_windows_from_lineage_chains_correctly():
    print("\n--- test_windows_from_lineage_chains_correctly ---")
    lineage = [
        {"adapter_path": "/a/v1", "timestamp": 100, "kind": "x"},
        {"adapter_path": "/a/v2", "timestamp": 200, "kind": "x"},
        {"adapter_path": "/a/v3", "timestamp": 300, "kind": "x"},
    ]
    windows = m.windows_from_lineage(lineage, end_ms=400)
    _ok("3 windows produced", len(windows) == 3)
    _ok("window 0 spans [100, 200)",
        windows[0]["start_ts_ms"] == 100 and windows[0]["end_ts_ms"] == 200)
    _ok("window 1 spans [200, 300)",
        windows[1]["start_ts_ms"] == 200 and windows[1]["end_ts_ms"] == 300)
    _ok("window 2 ends at end_ms (400)", windows[2]["end_ts_ms"] == 400)


def test_bucket_outcomes_filters_by_time():
    print("\n--- test_bucket_outcomes_filters_by_time ---")
    windows = [
        {"adapter_path": "/a/v1", "start_ts_ms": 100, "end_ts_ms": 200, "kind": "x"},
        {"adapter_path": "/a/v2", "start_ts_ms": 200, "end_ts_ms": 300, "kind": "x"},
    ]
    outcomes = ([{"t": 150, "g": 1, "ct": 10, "l": 100, "ch": 1}] +
                [{"t": 250, "g": 1, "ct": 20, "l": 200, "ch": 2}] +
                [{"t": 250, "g": 2, "ct": 20, "l": 200, "ch": 3}])
    bucketed = m.bucket_outcomes(outcomes, windows)
    _ok("v1 has 1 outcome", bucketed["/a/v1"]["count"] == 1)
    _ok("v2 has 2 outcomes", bucketed["/a/v2"]["count"] == 2)
    _ok("v2 rewrite_pct = 50%",
        abs(bucketed["/a/v2"]["guard_rewrite_pct"] - 0.5) < 0.01)


def main():
    print("M3 drift detector (E4) verifier")
    test_needs_rollback_on_reject_spike()
    test_degrading_on_rewrite_drift()
    test_degrading_on_completion_token_drop()
    test_ok_when_current_window_empty()
    test_ok_when_only_one_adapter()
    test_windows_from_lineage_chains_correctly()
    test_bucket_outcomes_filters_by_time()
    print(f"\n--- Results: {_PASS} passed, {_FAIL} failed ---")
    return 0 if _FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
