#!/usr/bin/env python3
"""Unit tests for eval_multiturn_local.py and multiturn_scenarios_deep.py.

Plain-runner pattern (no pytest). Run: python3 scripts/test_eval_multiturn_local.py
All model/judge I/O is mocked; no live mlx-server or ADC required.
"""
import json
import sys
import tempfile
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).parent))

import multiturn_scenarios_deep as deep


def test_six_deep_scenarios_present():
    names = [s["name"] for s in deep.DEEP_SCENARIOS]
    assert len(deep.DEEP_SCENARIOS) == 6, f"expected 6 scenarios, got {len(names)}"
    expected = {"casual_catchup", "emotional_escalation", "debate_opinions",
                "banter_humor", "news_reaction_chain", "advice_seeking"}
    assert set(names) == expected, f"name mismatch: {set(names) ^ expected}"
    print("✓ six_deep_scenarios_present")


def test_each_scenario_has_20_to_30_turns():
    for s in deep.DEEP_SCENARIOS:
        n = len(s["turns"])
        assert 20 <= n <= 30, f"{s['name']}: {n} turns (want 20–30)"
    print("✓ each_scenario_has_20_to_30_turns")


def test_anchors_reference_valid_turn_indices():
    for s in deep.DEEP_SCENARIOS:
        n = len(s["turns"])
        assert 3 <= len(s["anchors"]) <= 5, f"{s['name']}: {len(s['anchors'])} anchors (want 3–5)"
        for a in s["anchors"]:
            assert 1 <= a["turn"] <= n, f"{s['name']}: anchor turn {a['turn']} out of range"
            assert 1 <= a["probe_turn"] <= n, f"{s['name']}: probe_turn {a['probe_turn']} out of range"
            assert a["probe_turn"] > a["turn"], (
                f"{s['name']}: probe_turn {a['probe_turn']} must come after fact turn {a['turn']}")
            assert a["fact"].strip(), f"{s['name']}: empty anchor fact"
    print("✓ anchors_reference_valid_turn_indices")


def main():
    tests = [v for k, v in sorted(globals().items())
             if k.startswith("test_") and callable(v)]
    passed = failed = 0
    for t in tests:
        try:
            t(); passed += 1
        except AssertionError as e:
            print(f"✗ {t.__name__}: {e}"); failed += 1
        except Exception as e:
            print(f"✗ {t.__name__}: {type(e).__name__}: {e}"); failed += 1
    print("=" * 60)
    print(f"Results: {passed} passed, {failed} failed")
    print("=" * 60)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
