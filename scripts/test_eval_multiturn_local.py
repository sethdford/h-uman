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
import eval_multiturn_local as mt


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


def test_latency_ceiling_violations_flags_over_ceiling():
    series = [100.0, 200.0, 9000.0, 300.0, 12000.0]
    over = mt.latency_ceiling_violations(series, ceiling_ms=8000.0)
    assert over == [2, 4], f"expected indices [2,4], got {over}"
    print("✓ latency_ceiling_violations_flags_over_ceiling")


def test_latency_growth_flat_series_near_zero():
    series = [500.0] * 30
    g = mt.latency_growth(series)
    assert abs(g) < 1e-6, f"flat series should have ~0 growth, got {g}"
    print("✓ latency_growth_flat_series_near_zero")


def test_latency_growth_climbing_series_positive():
    # first third ~100, last third ~200 → growth ~1.0 (100%)
    series = [100.0] * 10 + [150.0] * 10 + [200.0] * 10
    g = mt.latency_growth(series)
    assert 0.9 < g < 1.1, f"expected ~1.0 growth, got {g}"
    print("✓ latency_growth_climbing_series_positive")


def test_latency_ok_passes_flat_under_ceiling():
    series = [400.0] * 24
    ok, detail = mt.latency_ok(series, ceiling_ms=8000.0, max_growth=0.20)
    assert ok is True, f"flat under-ceiling series should pass: {detail}"
    assert detail["ceiling_violations"] == []
    print("✓ latency_ok_passes_flat_under_ceiling")


def test_latency_ok_fails_on_growth():
    series = [100.0] * 12 + [500.0] * 12  # 400% growth
    ok, detail = mt.latency_ok(series, ceiling_ms=8000.0, max_growth=0.20)
    assert ok is False, "steeply climbing series should fail growth gate"
    assert detail["growth"] > 0.20
    print("✓ latency_ok_fails_on_growth")


def test_retention_rate_all_retained():
    assert mt.retention_rate([True, True, True, True]) == 1.0
    print("✓ retention_rate_all_retained")


def test_retention_rate_none_retained():
    assert mt.retention_rate([False, False, False]) == 0.0
    print("✓ retention_rate_none_retained")


def test_retention_rate_partial():
    rate = mt.retention_rate([True, True, False, True])  # 3/4
    assert abs(rate - 0.75) < 1e-9, f"expected 0.75, got {rate}"
    print("✓ retention_rate_partial")


def test_retention_rate_empty_is_zero():
    assert mt.retention_rate([]) == 0.0
    print("✓ retention_rate_empty_is_zero")


def test_voice_normalize_divides_by_ten():
    assert mt.voice_normalize(8.0) == 0.8
    assert mt.voice_normalize(10) == 1.0
    print("✓ voice_normalize_divides_by_ten")


def test_voice_drift_ok_stable_passes():
    # first-third 0.8, last-third 0.78 → drop 0.02 ≤ tol 0.10
    ok = mt.voice_drift_ok(0.8, 0.78, tol=0.10, any_hard_ai=False)
    assert ok is True
    print("✓ voice_drift_ok_stable_passes")


def test_voice_drift_ok_big_drop_fails():
    # drop 0.30 > tol 0.10
    ok = mt.voice_drift_ok(0.8, 0.5, tol=0.10, any_hard_ai=False)
    assert ok is False
    print("✓ voice_drift_ok_big_drop_fails")


def test_voice_drift_ok_hard_ai_late_fails():
    # small drop, but a late turn flipped to hard AI verdict
    ok = mt.voice_drift_ok(0.8, 0.79, tol=0.10, any_hard_ai=True)
    assert ok is False
    print("✓ voice_drift_ok_hard_ai_late_fails")


def test_scenario_verdict_all_axes_pass():
    sv = mt.scenario_verdict(
        name="casual_catchup",
        retention=0.90, voice_pass=True, voice_detail={"first": 0.8, "last": 0.79},
        latency_pass=True, latency_detail={"growth": 0.05, "ceiling_violations": []},
    )
    assert sv["passed"] is True
    assert sv["retention"]["passed"] is True
    assert sv["scenario"] == "casual_catchup"
    print("✓ scenario_verdict_all_axes_pass")


def test_scenario_verdict_retention_below_min_fails_axis():
    sv = mt.scenario_verdict(
        name="debate_opinions",
        retention=0.80, voice_pass=True, voice_detail={},
        latency_pass=True, latency_detail={},
    )
    assert sv["retention"]["passed"] is False  # 0.80 < RETENTION_RATE_MIN 0.85
    assert sv["passed"] is False
    print("✓ scenario_verdict_retention_below_min_fails_axis")


def test_run_verdict_five_of_six_passes():
    svs = [{"scenario": f"s{i}", "passed": (i != 5),
            "retention": {"rate": 0.9}} for i in range(6)]  # 5 pass, 1 fail, none below floor
    rv = mt.run_verdict(svs)
    assert rv["scenarios_passed"] == 5
    assert rv["run_passed"] is True
    assert rv["hard_floor_veto"] is False
    print("✓ run_verdict_five_of_six_passes")


def test_run_verdict_hard_floor_veto():
    svs = [{"scenario": f"s{i}", "passed": True,
            "retention": {"rate": 0.9}} for i in range(6)]
    svs[0]["retention"]["rate"] = 0.60  # below RETENTION_HARD_FLOOR 0.70
    rv = mt.run_verdict(svs)
    assert rv["hard_floor_veto"] is True
    assert rv["run_passed"] is False, "hard floor must veto even when 5/6 pass"
    print("✓ run_verdict_hard_floor_veto")


def test_write_verdict_roundtrip():
    verdict = {"run_passed": True, "scenarios": []}
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "verdict.json"
        mt.write_verdict(verdict, p)
        loaded = json.loads(p.read_text())
        assert loaded["run_passed"] is True
        assert "generated_at" in loaded
    print("✓ write_verdict_roundtrip")


class _FakeResp:
    def __init__(self, payload):
        self._b = json.dumps(payload).encode()
    def read(self):
        return self._b
    def __enter__(self):
        return self
    def __exit__(self, *a):
        return False


def test_localbackend_chat_returns_content_and_latency():
    backend = mt.LocalBackend("http://127.0.0.1:8741")
    payload = {"choices": [{"message": {"content": "yo what's good"}}]}
    with mock.patch.object(mt.urllib.request, "urlopen", return_value=_FakeResp(payload)):
        content, latency_ms = backend.chat([{"role": "user", "content": "hey"}])
    assert content == "yo what's good"
    assert latency_ms >= 0.0
    print("✓ localbackend_chat_returns_content_and_latency")


def test_localbackend_sends_full_history():
    backend = mt.LocalBackend("http://127.0.0.1:8741")
    captured = {}

    def fake_urlopen(req, timeout=0):
        captured["body"] = json.loads(req.data)
        return _FakeResp({"choices": [{"message": {"content": "ok"}}]})

    history = [
        {"role": "user", "content": "turn1"},
        {"role": "assistant", "content": "reply1"},
        {"role": "user", "content": "turn2"},
    ]
    with mock.patch.object(mt.urllib.request, "urlopen", side_effect=fake_urlopen):
        backend.chat(history)
    assert captured["body"]["messages"] == history, "must send full accumulated history"
    print("✓ localbackend_sends_full_history")


def test_localbackend_unreachable_raises_backend_unreachable():
    backend = mt.LocalBackend("http://127.0.0.1:8741")
    with mock.patch.object(mt.urllib.request, "urlopen",
                           side_effect=OSError("connection refused")):
        try:
            backend.chat([{"role": "user", "content": "hey"}])
            assert False, "expected BackendUnreachable"
        except mt.BackendUnreachable:
            pass
    print("✓ localbackend_unreachable_raises_backend_unreachable")


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
