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


def test_latency_growth_robust_to_single_late_spike():
    # Flat ~15 s medians both thirds, with ONE 78 s reply-length spike landing
    # late. median-of-thirds must NOT read this as a cliff (mean-of-thirds would).
    series = [15000.0] * 12 + [15000.0] * 11 + [78000.0]
    g = mt.latency_growth(series)
    assert abs(g) < 0.05, f"single late spike should not move median growth, got {g}"
    print("✓ latency_growth_robust_to_single_late_spike")


def test_latency_ok_passes_real_generation_envelope():
    # Real on-device per-turn latencies (generation-bound, 5–58 s) must PASS the
    # recalibrated pathology gate: nothing exceeds the 90 s ceiling and the
    # median-of-thirds growth stays under the 1.0 tolerance.
    series = [10537.0, 11716.0, 7877.0, 7300.0, 16320.0, 12671.0, 11430.0,
              12220.0, 9317.0, 8479.0, 10072.0, 17043.0, 20906.0, 14966.0,
              19137.0, 10862.0, 12421.0, 18611.0, 22511.0, 11296.0, 15970.0,
              33471.0, 58322.0, 14681.0, 8483.0, 13106.0, 15164.0]
    ok, detail = mt.latency_ok(series, ceiling_ms=mt.LATENCY_CEILING_MS,
                               max_growth=mt.LATENCY_MAX_GROWTH)
    assert ok is True, f"real generation envelope should pass pathology gate: {detail}"
    assert detail["ceiling_violations"] == []
    print("✓ latency_ok_passes_real_generation_envelope")


def test_latency_ok_fails_on_hang_above_pathology_ceiling():
    # A genuine runaway/non-terminating stream (>90 s) must trip the ceiling.
    series = [15000.0] * 10 + [120000.0]
    ok, detail = mt.latency_ok(series, ceiling_ms=mt.LATENCY_CEILING_MS,
                               max_growth=mt.LATENCY_MAX_GROWTH)
    assert ok is False, "a >90 s hang must fail the pathology ceiling"
    assert 10 in detail["ceiling_violations"]
    print("✓ latency_ok_fails_on_hang_above_pathology_ceiling")


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


def test_count_empty_replies_counts_and_lists_turns():
    # turns 2 and 4 empty (one ""), turns 1/3/5 have content
    by_turn = {1: "hey", 2: "", 3: "what's up", 4: "   ", 5: "later"}
    res = mt.count_empty_replies(by_turn)
    assert res["count"] == 2, res
    assert res["turns"] == [2, 4], res  # 1-indexed, sorted
    assert abs(res["rate"] - 0.4) < 1e-9, res  # 2/5
    print("✓ count_empty_replies_counts_and_lists_turns")


def test_count_empty_replies_none_empty():
    by_turn = {1: "a", 2: "b", 3: "c"}
    res = mt.count_empty_replies(by_turn)
    assert res["count"] == 0 and res["turns"] == [] and res["rate"] == 0.0, res
    print("✓ count_empty_replies_none_empty")


def test_count_empty_replies_all_empty():
    by_turn = {1: "", 2: None, 3: "  "}
    res = mt.count_empty_replies(by_turn)
    assert res["count"] == 3 and res["turns"] == [1, 2, 3] and res["rate"] == 1.0, res
    print("✓ count_empty_replies_all_empty")


def test_count_empty_replies_empty_input_is_zero_rate():
    res = mt.count_empty_replies({})
    assert res["count"] == 0 and res["turns"] == [] and res["rate"] == 0.0, res
    print("✓ count_empty_replies_empty_input_is_zero_rate")


def test_scenario_verdict_records_empty_replies_diagnostic():
    empties = {"count": 2, "turns": [13, 14], "rate": 0.1}
    sv = mt.scenario_verdict(
        name="casual_catchup", retention=0.90, voice_pass=True, voice_detail={},
        latency_pass=True, latency_detail={}, empty_replies=empties,
    )
    assert sv["empty_replies"] == empties, sv
    assert sv["passed"] is True, sv  # diagnostic does NOT gate
    print("✓ scenario_verdict_records_empty_replies_diagnostic")


def test_scenario_verdict_empty_replies_defaults_when_omitted():
    sv = mt.scenario_verdict(
        name="s", retention=0.90, voice_pass=True, voice_detail={},
        latency_pass=True, latency_detail={},
    )
    assert sv["empty_replies"] == {"count": 0, "turns": [], "rate": 0.0}, sv
    print("✓ scenario_verdict_empty_replies_defaults_when_omitted")


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


class _FakeStreamResp:
    """Iterable SSE response. Splits `content` into two delta chunks (so there is
    a first-token event distinct from the rest) and terminates with [DONE].
    Reconstruction of the chunks is exact."""
    def __init__(self, content):
        mid = max(1, len(content) // 2)
        pieces = [p for p in ([content[:mid], content[mid:]] if content else []) if p]
        self._lines = [
            ("data: " + json.dumps({"choices": [{"delta": {"content": p}}]})).encode()
            for p in pieces
        ]
        self._lines.append(b"data: [DONE]")
    def __iter__(self):
        return iter(self._lines)
    def __enter__(self):
        return self
    def __exit__(self, *a):
        return False


def test_localbackend_chat_streams_content_and_times_first_token():
    backend = mt.LocalBackend("http://127.0.0.1:8741")
    with mock.patch.object(mt.urllib.request, "urlopen",
                           return_value=_FakeStreamResp("yo what's good")):
        content, first_token_ms, total_ms = backend.chat([{"role": "user", "content": "hey"}])
    assert content == "yo what's good", "streamed chunks must reconstruct exactly"
    assert first_token_ms >= 0.0
    assert total_ms >= first_token_ms, "total must be >= time-to-first-token"
    print("✓ localbackend_chat_streams_content_and_times_first_token")


def test_localbackend_sends_full_history_and_requests_streaming():
    backend = mt.LocalBackend("http://127.0.0.1:8741")
    captured = {}

    def fake_urlopen(req, timeout=0):
        captured["body"] = json.loads(req.data)
        return _FakeStreamResp("ok")

    history = [
        {"role": "user", "content": "turn1"},
        {"role": "assistant", "content": "reply1"},
        {"role": "user", "content": "turn2"},
    ]
    with mock.patch.object(mt.urllib.request, "urlopen", side_effect=fake_urlopen):
        backend.chat(history)
    assert captured["body"]["messages"] == history, "must send full accumulated history"
    assert captured["body"]["stream"] is True, "must request streaming to time first token"
    print("✓ localbackend_sends_full_history_and_requests_streaming")


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


def test_judge_anchor_retention_parses_true():
    with mock.patch.object(mt, "call_gemini", return_value='{"retained": true}'):
        out = mt.judge_anchor_retention("the dog is named Biscuit",
                                        "what about the dog",
                                        "Biscuit's doing great, total menace")
    assert out is True
    print("✓ judge_anchor_retention_parses_true")


def test_judge_anchor_retention_parses_false():
    with mock.patch.object(mt, "call_gemini", return_value='```json\n{"retained": false}\n```'):
        out = mt.judge_anchor_retention("flying to Denver Friday",
                                        "what should I pack",
                                        "pack for anything, who knows")
    assert out is False
    print("✓ judge_anchor_retention_parses_false")


def test_judge_available_true_when_token_present():
    with mock.patch.object(mt, "_get_adc_token", return_value="tok"):
        assert mt.judge_available() is True
    print("✓ judge_available_true_when_token_present")


def test_judge_available_false_when_no_token():
    with mock.patch.object(mt, "_get_adc_token", return_value=None):
        assert mt.judge_available() is False
    print("✓ judge_available_false_when_no_token")


def _tiny_scenario():
    return {
        "name": "tiny",
        "description": "tiny test scenario",
        "anchors": [{"turn": 1, "fact": "user likes tea", "probe_turn": 3}],
        "turns": ["hi", "how are you", "what do I like to drink", "cool", "bye", "later"],
    }


def test_run_scenario_collects_latency_and_transcript():
    backend = mock.Mock()
    backend.chat.side_effect = [("r%d" % i, 100.0 + i, 200.0 + i) for i in range(6)]
    sv = mt.run_scenario(_tiny_scenario(), backend, judge_on=False)
    assert len(sv["latency"]["series_ms"]) == 6, "one TTFT sample per turn"
    assert len(sv["latency"]["total_series_ms"]) == 6, "one total sample per turn"
    # judge_on=False → retention/voice marked skipped, latency still computed
    assert sv["voice"]["passed"] in (True, None)
    print("✓ run_scenario_collects_latency_and_transcript")


def test_run_scenario_gates_first_token_not_total():
    # CHIP CONTRACT: the latency gate is on TIME-TO-FIRST-TOKEN, not total turn
    # latency. A scenario with fast TTFT but slow total generation must PASS the
    # latency axis — total is diagnostics-only. (Before the chip, this would FAIL
    # because the 8 s ceiling gated total turn latency a 31B model can't meet.)
    backend = mock.Mock()
    backend.chat.side_effect = [("reply", 100.0, 50000.0) for _ in range(6)]
    sv = mt.run_scenario(_tiny_scenario(), backend, judge_on=False)
    assert sv["latency"]["passed"] is True, "fast TTFT must pass even with slow total"
    assert sv["latency"]["series_ms"] == [100.0] * 6, "gated series must be TTFT"
    assert sv["latency"]["total_series_ms"] == [50000.0] * 6, "total kept for diagnostics"
    print("✓ run_scenario_gates_first_token_not_total")


def test_run_scenario_max_turns_caps_depth():
    # FAST-DATA KNOB: --max-turns caps conversation depth without consuming the
    # whole scripted scenario, so the harness can be smoke-tested in minutes.
    backend = mock.Mock()
    backend.chat.side_effect = [("reply", 100.0, 200.0) for _ in range(10)]
    sv = mt.run_scenario(_tiny_scenario(), backend, judge_on=False, max_turns=2)
    assert backend.chat.call_count == 2, "only the first 2 turns should be driven"
    assert len(sv["latency"]["series_ms"]) == 2, "one TTFT sample per driven turn"
    print("✓ run_scenario_max_turns_caps_depth")


def test_run_scenario_max_turns_skips_truncated_anchor():
    # An anchor whose probe_turn (3) falls past the cap (2) must be SKIPPED, not
    # KeyError. With no scorable anchors, retention_rate degrades to the empty
    # default rather than crashing the run.
    backend = mock.Mock()
    backend.chat.side_effect = [("reply", 100.0, 200.0) for _ in range(10)]
    with mock.patch.object(mt, "judge_anchor_retention", return_value=True) as jar, \
         mock.patch.object(mt, "judge_voice_window", return_value=(8.0, "HUMAN")):
        sv = mt.run_scenario(_tiny_scenario(), backend, judge_on=True, max_turns=2)
    assert not jar.called, "anchor probed past the cap must not be judged"
    assert sv["latency"]["series_ms"] == [100.0, 100.0]
    print("✓ run_scenario_max_turns_skips_truncated_anchor")


def test_run_scenario_retention_uses_anchor_probe():
    backend = mock.Mock()
    backend.chat.side_effect = [("r%d" % i, 100.0, 200.0) for i in range(6)]
    with mock.patch.object(mt, "judge_anchor_retention", return_value=True) as jar, \
         mock.patch.object(mt, "judge_voice_window", return_value=(8.0, "HUMAN")):
        sv = mt.run_scenario(_tiny_scenario(), backend, judge_on=True)
    assert jar.called, "retention judge must be invoked at probe turns"
    assert sv["retention"]["rate"] == 1.0
    print("✓ run_scenario_retention_uses_anchor_probe")


def test_main_writes_verdict_and_returns_exit_code():
    backend = mock.Mock()
    backend.chat.side_effect = [("reply", 100.0, 200.0)] * 200  # plenty for all scenarios
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / "verdict.json"
        with mock.patch.object(mt, "LocalBackend", return_value=backend), \
             mock.patch.object(mt, "judge_available", return_value=True), \
             mock.patch.object(mt, "judge_anchor_retention", return_value=True), \
             mock.patch.object(mt, "judge_voice_window", return_value=(9.0, "HUMAN")):
            code = mt.main(["--output-json", str(out), "--server-url", "http://x"])
        verdict = json.loads(out.read_text())
        assert "run_passed" in verdict and "scenarios" in verdict
        assert code in (0, 1)
    print("✓ main_writes_verdict_and_returns_exit_code")


def test_main_deferred_when_backend_unreachable():
    backend = mock.Mock()
    backend.chat.side_effect = mt.BackendUnreachable("refused")
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / "verdict.json"
        with mock.patch.object(mt, "LocalBackend", return_value=backend), \
             mock.patch.object(mt, "judge_available", return_value=True):
            code = mt.main(["--output-json", str(out), "--server-url", "http://x"])
        assert code == 2, "unreachable backend → DEFERRED exit 2"
    print("✓ main_deferred_when_backend_unreachable")


def test_main_skipped_when_judge_unavailable():
    backend = mock.Mock()
    backend.chat.side_effect = [("reply", 100.0, 200.0)] * 200
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / "verdict.json"
        with mock.patch.object(mt, "LocalBackend", return_value=backend), \
             mock.patch.object(mt, "judge_available", return_value=False):
            code = mt.main(["--output-json", str(out), "--server-url", "http://x"])
        verdict = json.loads(out.read_text())
        assert code == 3, "judge unavailable → SKIPPED exit 3"
        assert verdict["judge"] == "SKIPPED"
    print("✓ main_skipped_when_judge_unavailable")


def test_judge_anchor_retention_raises_on_malformed_output():
    # A judge that returns garbage (not JSON) must raise JudgeUnavailable, not crash
    # with a bare ValueError nor be mistaken for a model failure.
    with mock.patch.object(mt, "call_gemini", return_value="sorry, I can't help with that"):
        try:
            mt.judge_anchor_retention("fact", "probe", "response")
        except mt.JudgeUnavailable:
            print("✓ judge_anchor_retention_raises_on_malformed_output")
            return
    raise AssertionError("expected JudgeUnavailable on unparseable judge output")


def test_judge_voice_window_raises_on_empty_result():
    # A falsy judge result must raise rather than silently scoring (0.0, 'AI'),
    # which would manufacture a spurious voice-drift FAIL.
    with mock.patch.object(mt, "evaluate_conversation", return_value=None):
        try:
            mt.judge_voice_window("casual_catchup", [("hi", "hey")])
        except mt.JudgeUnavailable:
            print("✓ judge_voice_window_raises_on_empty_result")
            return
    raise AssertionError("expected JudgeUnavailable when judge returns no result")


def test_main_fails_when_judge_off_but_latency_breaks():
    # CRITICAL contract: with the judge unavailable, a latency regression must
    # surface as FAIL (exit 1), NOT be masked as SKIPPED (exit 3).
    backend = mock.Mock()
    # Every turn's TTFT blows the ceiling (total mirrors it here).
    backend.chat.side_effect = [("reply", 99999.0, 99999.0)] * 400
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / "verdict.json"
        with mock.patch.object(mt, "LocalBackend", return_value=backend), \
             mock.patch.object(mt, "judge_available", return_value=False):
            code = mt.main(["--output-json", str(out), "--server-url", "http://x"])
        assert code == 1, f"judge off + latency fail → FAIL exit 1, got {code}"
    print("✓ main_fails_when_judge_off_but_latency_breaks")


def test_main_degrades_to_skipped_when_judge_dies_midrun():
    # Judge available at start, then raises JudgeUnavailable. main() must re-run
    # latency-only and degrade to SKIPPED (exit 3) when latency holds — a judge
    # outage must never masquerade as a model FAIL.
    backend = mock.Mock()
    backend.chat.side_effect = [("reply", 100.0, 200.0)] * 400
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / "verdict.json"
        with mock.patch.object(mt, "LocalBackend", return_value=backend), \
             mock.patch.object(mt, "judge_available", return_value=True), \
             mock.patch.object(mt, "judge_anchor_retention",
                               side_effect=mt.JudgeUnavailable("ADC revoked")):
            code = mt.main(["--output-json", str(out), "--server-url", "http://x"])
        verdict = json.loads(out.read_text())
        assert code == 3, f"judge dies mid-run + latency ok → SKIPPED exit 3, got {code}"
        assert verdict["judge"] == "SKIPPED"
    print("✓ main_degrades_to_skipped_when_judge_dies_midrun")


def test_load_persona_system_prompt_reads_persona_dir():
    # A persona JSON under the dir → a system prompt derived from its fields.
    persona = {
        "core_anchor": "I am Seth. Not an AI.",
        "core": {"identity": "Seth Ford, architect."},
        "style_rules": ["Keep it short.", "Use contractions."],
        "anti_patterns": ["Never say 'certainly'."],
    }
    with tempfile.TemporaryDirectory() as d:
        (Path(d) / "seth.json").write_text(json.dumps(persona))
        prompt = mt.load_persona_system_prompt(persona_dir=d)
    assert "I am Seth. Not an AI." in prompt, "anchor must seed the prompt"
    assert "Seth Ford, architect." in prompt, "identity must be included"
    assert "Use contractions." in prompt, "style rules must be included"
    assert "Never say 'certainly'." in prompt, "anti-patterns must be included"
    print("✓ load_persona_system_prompt_reads_persona_dir")


def test_load_persona_system_prompt_falls_back_when_no_file():
    # No persona file → the hardcoded Seth-voice fallback (never bare).
    with tempfile.TemporaryDirectory() as d:
        prompt = mt.load_persona_system_prompt(persona_dir=d)
    assert prompt == mt._FALLBACK_PERSONA_PROMPT
    assert "Seth" in prompt and "not an AI" in prompt
    print("✓ load_persona_system_prompt_falls_back_when_no_file")


def test_load_persona_system_prompt_falls_back_on_malformed_json():
    # A corrupt persona file must not crash the harness — fall back cleanly.
    with tempfile.TemporaryDirectory() as d:
        (Path(d) / "broken.json").write_text("{ not valid json ]")
        prompt = mt.load_persona_system_prompt(persona_dir=d)
    assert prompt == mt._FALLBACK_PERSONA_PROMPT
    print("✓ load_persona_system_prompt_falls_back_on_malformed_json")


def test_run_scenario_persists_persona_system_prompt():
    # The persona must be messages[0] (a system turn) on EVERY backend call,
    # mirroring production prompt assembly across the whole conversation.
    seen = []

    def capture(messages):
        # messages is mutated in place across turns; snapshot index 0 + length.
        seen.append((messages[0]["role"], messages[0]["content"], len(messages)))
        return ("reply", 100.0, 200.0)

    backend = mock.Mock()
    backend.chat.side_effect = capture
    mt.run_scenario(_tiny_scenario(), backend, judge_on=False,
                    persona_prompt="PERSONA-SENTINEL")
    assert seen, "backend.chat must be invoked"
    # First call: system + first user turn.
    assert seen[0] == ("system", "PERSONA-SENTINEL", 2)
    # Every call keeps the persona pinned at index 0 (it persists, never drifts).
    for role, content, _ in seen:
        assert role == "system" and content == "PERSONA-SENTINEL", \
            "persona system turn must stay at messages[0] every turn"
    # History still grows (last call has more messages than the first).
    assert seen[-1][2] > seen[0][2], "conversation history must accumulate"
    print("✓ run_scenario_persists_persona_system_prompt")


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
