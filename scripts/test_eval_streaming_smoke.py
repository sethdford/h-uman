#!/usr/bin/env python3
"""Unit tests for eval_streaming_smoke.py — the streaming-readiness tripwire.

Plain-runner pattern (no pytest). Run: python3 scripts/test_eval_streaming_smoke.py
All probe I/O is mocked; no live gemma-realtime server required.

The pure helpers (classify_delivery / find_harmony_leaks / streaming_verdict)
encode the decision "is token-by-token streaming actually working on :8741 yet?"
They are unit-tested here in isolation; the live probe is exercised separately
(and against the real server in the smoke run), per the predicate-extraction
discipline in .claude/rules/security-predicate-extraction.md.
"""
import json
import sys
import tempfile
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).parent))

import eval_streaming_smoke as ss


# ---------------------------------------------------------------------------
# classify_delivery — buffered vs incremental from chunk arrival times
# ---------------------------------------------------------------------------

def test_classify_delivery_single_chunk_is_buffered():
    # The :8741 server today emits ONE SSE chunk after generating the whole
    # reply (TTFT==total). One content chunk => buffered, never incremental.
    d = ss.classify_delivery([50000.0], content_len=120)
    assert d["chunk_count"] == 1
    assert d["incremental"] is False
    assert abs(d["ttft_ratio"] - 1.0) < 1e-9, "single chunk: first==last => ratio 1.0"
    print("✓ classify_delivery_single_chunk_is_buffered")


def test_classify_delivery_zero_chunks_is_buffered():
    # No content chunks at all (e.g. empty reply) is not incremental delivery.
    d = ss.classify_delivery([], content_len=0)
    assert d["chunk_count"] == 0
    assert d["incremental"] is False
    print("✓ classify_delivery_zero_chunks_is_buffered")


def test_classify_delivery_late_burst_is_buffered():
    # Several chunks all arriving in a tight burst at the very END of the
    # window (first/last ~= 1.0) is still buffering, not streaming.
    d = ss.classify_delivery([49998.0, 49999.0, 50000.0], content_len=120)
    assert d["chunk_count"] == 3
    assert d["incremental"] is False, "late burst (ratio ~1.0) is buffered"
    print("✓ classify_delivery_late_burst_is_buffered")


def test_classify_delivery_spread_chunks_is_incremental():
    # First token early, last token late => true token-by-token streaming.
    d = ss.classify_delivery([200.0, 1200.0, 8000.0, 30000.0, 50000.0],
                             content_len=120)
    assert d["chunk_count"] == 5
    assert d["incremental"] is True, "early first token + spread => incremental"
    assert d["ttft_ratio"] < 0.5
    print("✓ classify_delivery_spread_chunks_is_incremental")


# ---------------------------------------------------------------------------
# find_harmony_leaks — raw Harmony markers that escaped the filter
# ---------------------------------------------------------------------------

def test_find_harmony_leaks_clean_text_none():
    assert ss.find_harmony_leaks("hey, just chilling at home — what's up?") == []
    print("✓ find_harmony_leaks_clean_text_none")


def test_find_harmony_leaks_channel_marker_detected():
    leaks = ss.find_harmony_leaks("<|channel|>analysis<|message|>actual reply")
    assert any("channel" in m for m in leaks), f"channel marker missed: {leaks}"
    assert any("message" in m for m in leaks), f"message marker missed: {leaks}"
    print("✓ find_harmony_leaks_channel_marker_detected")


def test_find_harmony_leaks_bare_start_marker_detected():
    leaks = ss.find_harmony_leaks("text then <|start a leak")
    assert leaks, "a bare <| sequence must be flagged as a leak"
    print("✓ find_harmony_leaks_bare_start_marker_detected")


def test_find_harmony_leaks_empty_input_safe():
    assert ss.find_harmony_leaks("") == []
    assert ss.find_harmony_leaks(None) == []
    print("✓ find_harmony_leaks_empty_input_safe")


# ---------------------------------------------------------------------------
# streaming_verdict — combine delivery + leaks into a go/no-go
# ---------------------------------------------------------------------------

def test_streaming_verdict_incremental_clean_is_beneficial():
    d = {"chunk_count": 12, "incremental": True, "ttft_ratio": 0.01}
    v = ss.streaming_verdict(d, leaks=[])
    assert v["streaming_beneficial"] is True
    assert v["exit_code"] == 0
    print("✓ streaming_verdict_incremental_clean_is_beneficial")


def test_streaming_verdict_buffered_is_not_beneficial():
    d = {"chunk_count": 1, "incremental": False, "ttft_ratio": 1.0}
    v = ss.streaming_verdict(d, leaks=[])
    assert v["streaming_beneficial"] is False
    assert v["exit_code"] == 1
    assert "buffer" in v["reason"].lower(), f"reason should name buffering: {v['reason']}"
    print("✓ streaming_verdict_buffered_is_not_beneficial")


def test_streaming_verdict_incremental_but_leaking_is_not_beneficial():
    # Streaming works at the transport layer, but raw Harmony markers leak
    # through => flipping the flag would ship dirty tokens. Not ready.
    d = {"chunk_count": 12, "incremental": True, "ttft_ratio": 0.01}
    v = ss.streaming_verdict(d, leaks=["<|channel|>"])
    assert v["streaming_beneficial"] is False
    assert v["exit_code"] == 1
    assert "harmony" in v["reason"].lower() or "leak" in v["reason"].lower()
    print("✓ streaming_verdict_incremental_but_leaking_is_not_beneficial")


# ---------------------------------------------------------------------------
# probe_stream — live SSE probe (mocked transport)
# ---------------------------------------------------------------------------

class _FakeResp:
    """Minimal urlopen() context manager yielding SSE byte lines."""
    def __init__(self, lines):
        self._lines = lines

    def __enter__(self):
        return iter(self._lines)

    def __exit__(self, *a):
        return False


def _sse(content):
    payload = json.dumps({"choices": [{"delta": {"content": content}}]})
    return f"data: {payload}\n".encode()


def test_probe_stream_records_chunk_times_and_content():
    lines = [_sse("hey"), _sse(" there"), b"data: [DONE]\n"]
    with mock.patch.object(ss.urllib.request, "urlopen",
                           return_value=_FakeResp(lines)):
        stamps, content = ss.probe_stream("http://127.0.0.1:8741", "hi", timeout=5)
    assert content == "hey there", f"content reassembled wrong: {content!r}"
    assert len(stamps) == 2, "one timestamp per content chunk"
    assert all(isinstance(s, float) for s in stamps)
    print("✓ probe_stream_records_chunk_times_and_content")


def test_probe_stream_unreachable_raises_server_down():
    with mock.patch.object(ss.urllib.request, "urlopen",
                           side_effect=OSError("connection refused")):
        try:
            ss.probe_stream("http://127.0.0.1:8741", "hi", timeout=5)
        except ss.ServerDown:
            print("✓ probe_stream_unreachable_raises_server_down")
            return
    raise AssertionError("expected ServerDown on transport failure")


# ---------------------------------------------------------------------------
# main — wiring, verdict JSON, exit codes
# ---------------------------------------------------------------------------

def test_main_writes_verdict_and_exits_zero_when_incremental():
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / "stream_verdict.json"
        spread = ([200.0, 8000.0, 50000.0], "clean reply text")
        with mock.patch.object(ss, "probe_stream", return_value=spread):
            code = ss.main(["--server-url", "http://x",
                            "--output-json", str(out)])
        verdict = json.loads(out.read_text())
        assert code == 0, f"incremental+clean => exit 0, got {code}"
        assert verdict["streaming_beneficial"] is True
        assert verdict["delivery"]["incremental"] is True
    print("✓ main_writes_verdict_and_exits_zero_when_incremental")


def test_main_exits_one_when_buffered():
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / "stream_verdict.json"
        buffered = ([50000.0], "the whole reply at once")
        with mock.patch.object(ss, "probe_stream", return_value=buffered):
            code = ss.main(["--server-url", "http://x",
                            "--output-json", str(out)])
        verdict = json.loads(out.read_text())
        assert code == 1, f"buffered => exit 1, got {code}"
        assert verdict["streaming_beneficial"] is False
    print("✓ main_exits_one_when_buffered")


def test_main_exits_two_when_server_down():
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / "stream_verdict.json"
        with mock.patch.object(ss, "probe_stream",
                               side_effect=ss.ServerDown("refused")):
            code = ss.main(["--server-url", "http://x",
                            "--output-json", str(out)])
        verdict = json.loads(out.read_text())
        assert code == 2, f"server down => exit 2, got {code}"
        assert verdict["status"] == "SERVER_DOWN"
    print("✓ main_exits_two_when_server_down")


# ---------------------------------------------------------------------------
# classify_delivery — absolute TTFT (ms) reporting
# ---------------------------------------------------------------------------

def test_classify_delivery_reports_absolute_ttft_ms():
    # The first content chunk landed at 200 ms; that IS the absolute TTFT, the
    # number that maps to the SOTA bar (< ~300 ms feels instant).
    d = ss.classify_delivery([200.0, 1200.0, 50000.0], content_len=120)
    assert d["ttft_ms"] == 200.0, f"ttft_ms should be first-chunk ms: {d}"
    print("✓ classify_delivery_reports_absolute_ttft_ms")


def test_classify_delivery_zero_chunks_ttft_ms_is_zero():
    d = ss.classify_delivery([], content_len=0)
    assert d["ttft_ms"] == 0.0
    print("✓ classify_delivery_zero_chunks_ttft_ms_is_zero")


def test_classify_delivery_buffered_ttft_ms_equals_total():
    # Buffered: the single chunk lands at the very end, so absolute TTFT ~= total.
    d = ss.classify_delivery([50000.0], content_len=120)
    assert d["ttft_ms"] == 50000.0
    print("✓ classify_delivery_buffered_ttft_ms_equals_total")


# ---------------------------------------------------------------------------
# probe_regime — single-prompt bundle (delivery + leaks + verdict)
# ---------------------------------------------------------------------------

def test_probe_regime_bundles_delivery_and_verdict():
    spread = ([200.0, 8000.0, 50000.0], "clean incremental reply")
    with mock.patch.object(ss, "probe_stream", return_value=spread):
        r = ss.probe_regime("http://x", "hey", timeout=5)
    assert r["prompt"] == "hey"
    assert r["delivery"]["incremental"] is True
    assert r["streaming_beneficial"] is True
    assert r["exit_code"] == 0
    assert r["delivery"]["ttft_ms"] == 200.0
    print("✓ probe_regime_bundles_delivery_and_verdict")


def test_probe_regime_propagates_server_down():
    with mock.patch.object(ss, "probe_stream",
                           side_effect=ss.ServerDown("refused")):
        try:
            ss.probe_regime("http://x", "hey", timeout=5)
        except ss.ServerDown:
            print("✓ probe_regime_propagates_server_down")
            return
    raise AssertionError("probe_regime must propagate ServerDown")


# ---------------------------------------------------------------------------
# main — dual-regime (casual drives exit code; analytical informational)
# ---------------------------------------------------------------------------

def test_main_probes_both_regimes_and_records_each():
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / "verdict.json"
        spread = ([200.0, 8000.0, 50000.0], "clean reply")
        with mock.patch.object(ss, "probe_stream", return_value=spread):
            code = ss.main(["--server-url", "http://x", "--output-json", str(out)])
        verdict = json.loads(out.read_text())
        assert code == 0
        # Both regimes present; casual mirrors the top-level verdict fields.
        assert "casual" in verdict and "analytical" in verdict
        assert verdict["casual"]["delivery"]["incremental"] is True
        assert verdict["analytical"]["delivery"]["incremental"] is True
        assert verdict["delivery"] == verdict["casual"]["delivery"]
    print("✓ main_probes_both_regimes_and_records_each")


def test_main_skips_analytical_when_empty():
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / "verdict.json"
        buffered = ([50000.0], "whole reply at once")
        with mock.patch.object(ss, "probe_stream", return_value=buffered):
            code = ss.main(["--server-url", "http://x", "--analytical-prompt", "",
                            "--output-json", str(out)])
        verdict = json.loads(out.read_text())
        assert code == 1, "buffered casual => exit 1"
        assert "analytical" not in verdict, "empty analytical prompt must skip the probe"
        assert verdict["casual"]["delivery"]["ttft_ms"] == 50000.0
    print("✓ main_skips_analytical_when_empty")


def test_main_casual_server_down_exits_two_before_analytical():
    # If the casual probe can't reach the server, exit 2 immediately — the
    # analytical probe must not run (and not mask the down state).
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / "verdict.json"
        with mock.patch.object(ss, "probe_stream",
                               side_effect=ss.ServerDown("refused")):
            code = ss.main(["--server-url", "http://x", "--output-json", str(out)])
        verdict = json.loads(out.read_text())
        assert code == 2
        assert verdict["status"] == "SERVER_DOWN"
        assert "analytical" not in verdict
    print("✓ main_casual_server_down_exits_two_before_analytical")


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
