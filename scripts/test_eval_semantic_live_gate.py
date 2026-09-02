#!/usr/bin/env python3
"""pytest suite for eval_semantic_live_gate.py (Contract C1).

No network: the Gemini judge is short-circuited via HU_GATE_FAKE=1 (the
module's own fake path — see call_gemini()), and a tiny stdlib HTTP server
stands in for :8741 (chat completions) and the embedder. The `human` CLI is
never invoked directly here — semantic_search()/score_arm() are monkeypatched
so the suite doesn't depend on a compiled binary.

Two families:
  1. Pure-function tests (decide_verdict, select_contexts, parsing helpers) —
     no I/O at all.
  2. End-to-end main() tests against the fake server, proving both the happy
     path (writes a verdict) and every REFUSE path (writes nothing, exit 2).
"""
from __future__ import annotations

import json
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eval_semantic_live_gate as G  # noqa: E402


# ---------------------------------------------------------------------------
# Fake server: stands in for :8741 (chat completions) and the embedder.
# ---------------------------------------------------------------------------
class _FakeHandler(BaseHTTPRequestHandler):
    def log_message(self, *a):  # silence
        pass

    def _read_body(self):
        length = int(self.headers.get("Content-Length", 0))
        return json.loads(self.rfile.read(length) or b"{}")

    def _send_json(self, obj, code=200):
        payload = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_POST(self):  # noqa: N802
        if self.path == "/v1/embeddings":
            self._read_body()
            self._send_json({"data": [{"embedding": [0.0] * 8}]})
            return
        if self.path == "/v1/chat/completions":
            body = self._read_body()
            sys_msg = next((m["content"] for m in body.get("messages", [])
                            if m.get("role") == "system"), "")
            has_memories = "Relevant memories:" in sys_msg
            reply = "with-memories reply" if has_memories else "plain reply"
            self._send_json({"choices": [{"message": {"content": reply}}]})
            return
        self._send_json({"error": "unknown path"}, code=404)


@pytest.fixture()
def fake_server():
    server = HTTPServer(("127.0.0.1", 0), _FakeHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}"
    yield base
    server.shutdown()
    thread.join(timeout=5)


@pytest.fixture(autouse=True)
def fake_judge(monkeypatch):
    """Every test gets the network-free judge unless it overrides this."""
    monkeypatch.setenv("HU_GATE_FAKE", "1")
    yield


@pytest.fixture()
def contexts_file(tmp_path):
    p = tmp_path / "contexts.jsonl"
    rows = [{"incoming": f"real inbound message number {i} about something"} for i in range(40)]
    p.write_text("\n".join(json.dumps(r) for r in rows) + "\n")
    return str(p)


def _base_args(fake_server, contexts_file, out_path, extra=None):
    args = [
        "--contexts", contexts_file,
        "--n", "32",
        "--min-n", "30",
        "--server", fake_server,
        "--embed-url", fake_server,
        "--memory-db", "/dev/null",  # copy_memory_db is monkeypatched in most tests
        "--system-prompt-file", "-",  # overridden per-test via a real file
        "--out", out_path,
    ]
    return args + (extra or [])


# ---------------------------------------------------------------------------
# 1. Pure verdict logic — no I/O
# ---------------------------------------------------------------------------
def _summary(composite=0.8, ei=4.0, reality=4.0):
    return {"composite": composite, "ei_mean": ei, "reality_mean": reality}


def test_promote_when_live_matches_shadow():
    verdict, reasons = G.decide_verdict(_summary(), _summary())
    assert verdict == "PROMOTE"
    assert reasons == []


def test_promote_when_live_strictly_better():
    verdict, reasons = G.decide_verdict(_summary(0.7, 3.0, 3.0), _summary(0.9, 4.5, 4.5))
    assert verdict == "PROMOTE"


def test_hold_when_composite_drops_beyond_tolerance():
    shadow = _summary(composite=0.80)
    live = _summary(composite=0.70)
    verdict, reasons = G.decide_verdict(shadow, live, composite_tolerance=0.02)
    assert verdict == "HOLD"
    assert any("composite dropped" in r for r in reasons)


def test_composite_drop_within_tolerance_still_promotes():
    shadow = _summary(composite=0.80)
    live = _summary(composite=0.79)
    verdict, _ = G.decide_verdict(shadow, live, composite_tolerance=0.02)
    assert verdict == "PROMOTE"


def test_hold_when_ei_drops_beyond_tolerance():
    """The AlpsBench finding this gate exists to catch: memory retrieval can
    degrade emotional intelligence even while other axes look fine."""
    shadow = _summary(ei=4.0)
    live = _summary(ei=3.0)
    verdict, reasons = G.decide_verdict(shadow, live, ei_tolerance=0.15)
    assert verdict == "HOLD"
    assert any("emotional_intelligence dropped" in r for r in reasons)


def test_hold_when_reality_awareness_drops_beyond_tolerance():
    shadow = _summary(reality=4.0)
    live = _summary(reality=3.2)
    verdict, reasons = G.decide_verdict(shadow, live, reality_tolerance=0.15)
    assert verdict == "HOLD"
    assert any("reality_awareness dropped" in r for r in reasons)


def test_hold_reasons_accumulate_for_multiple_regressions():
    shadow = _summary(composite=0.8, ei=4.0, reality=4.0)
    live = _summary(composite=0.5, ei=2.0, reality=2.0)
    verdict, reasons = G.decide_verdict(shadow, live)
    assert verdict == "HOLD"
    assert len(reasons) == 3


def test_ei_improving_does_not_mask_a_composite_drop():
    """Classifier-score-plus-flag-gate style: no single strong axis should be
    able to buy back a regression on another required axis."""
    shadow = _summary(composite=0.80, ei=3.0)
    live = _summary(composite=0.60, ei=5.0)
    verdict, reasons = G.decide_verdict(shadow, live)
    assert verdict == "HOLD"
    assert any("composite dropped" in r for r in reasons)


# ---------------------------------------------------------------------------
# 2. select_contexts — deterministic, fixed subset
# ---------------------------------------------------------------------------
def test_select_contexts_missing_file_returns_empty():
    assert G.select_contexts("/no/such/file.jsonl", 10) == []


def test_select_contexts_deterministic_across_calls(contexts_file):
    a = G.select_contexts(contexts_file, 10)
    b = G.select_contexts(contexts_file, 10)
    assert a == b
    assert len(a) == 10


def test_select_contexts_dedupes_and_filters_short(tmp_path):
    p = tmp_path / "c.jsonl"
    rows = [
        {"incoming": "hi"},                       # too short (< 4 chars)
        {"incoming": "a real message here"},
        {"incoming": "a real message here"},      # duplicate
        {"prompt": "a prompt-keyed message too"},
        {"incoming": ""},                          # blank
        "not json",
    ]
    lines = []
    for r in rows:
        lines.append(r if isinstance(r, str) else json.dumps(r))
    p.write_text("\n".join(lines) + "\n")
    out = G.select_contexts(str(p), 10)
    assert len(out) == len(set(out))  # dedup means no repeats
    assert "hi" not in out
    assert len(out) == 2


def test_select_contexts_respects_n_cap(contexts_file):
    out = G.select_contexts(contexts_file, 5)
    assert len(out) == 5


# ---------------------------------------------------------------------------
# 3. semantic-results parsing
# ---------------------------------------------------------------------------
def test_parse_semantic_results_no_results():
    assert G._parse_semantic_results("No results for: foo\n") == []


def test_parse_semantic_results_basic():
    stdout = (
        "  [1] fact:123 (0.912): likes hiking on weekends\n"
        "  [2] fact:456 (0.803): works at a hospital\n"
    )
    out = G._parse_semantic_results(stdout)
    assert out == ["likes hiking on weekends", "works at a hospital"]


def test_build_memories_block_empty_is_none():
    assert G.build_memories_block([]) is None
    assert G.build_memories_block(None) is None


def test_build_memories_block_formats_bullets():
    block = G.build_memories_block(["a", "b"])
    assert block.startswith("Relevant memories:\n")
    assert "- a" in block and "- b" in block
    assert block.endswith("\n\n")


# ---------------------------------------------------------------------------
# 4. judge (fake mode) — deterministic, in-range, network-free
# ---------------------------------------------------------------------------
def test_fake_judge_returns_in_range_scores():
    r = G.judge_ei_reality("hey you ok?", "yeah I'm fine thanks")
    assert r is not None
    assert 1 <= r["ei"] <= 5
    assert 1 <= r["reality"] <= 5


def test_fake_judge_is_deterministic_for_same_input():
    a = G.judge_ei_reality("same incoming", "same reply")
    b = G.judge_ei_reality("same incoming", "same reply")
    assert a == b


def test_preflight_judge_passes_under_fake_mode():
    assert G.preflight_judge() is True


def test_judge_returns_none_when_call_gemini_raises(monkeypatch):
    def boom(*a, **k):
        raise RuntimeError("network is down")
    monkeypatch.setattr(G, "call_gemini", boom)
    assert G.judge_ei_reality("x", "y") is None


def test_preflight_judge_fails_when_judge_unreachable(monkeypatch):
    def boom(*a, **k):
        raise RuntimeError("no ADC credentials found")
    monkeypatch.setattr(G, "call_gemini", boom)
    assert G.preflight_judge() is False


# ---------------------------------------------------------------------------
# 5. embedder preflight against the fake server
# ---------------------------------------------------------------------------
def test_preflight_embedder_true_against_fake_server(fake_server):
    assert G.preflight_embedder(fake_server) is True


def test_preflight_embedder_false_when_unreachable():
    assert G.preflight_embedder("http://127.0.0.1:1") is False


# ---------------------------------------------------------------------------
# 6. generate() against the fake server, incl. the memories-block distinction
# ---------------------------------------------------------------------------
def test_generate_plain_reply(fake_server):
    reply = G.generate(fake_server, "m", "system prompt, no memories", "hey", 50, 0.7)
    assert reply == "plain reply"


def test_generate_with_memories_block_changes_response(fake_server):
    sp = "Relevant memories:\n- likes hiking\n\nsystem prompt"
    reply = G.generate(fake_server, "m", sp, "hey", 50, 0.7)
    assert reply == "with-memories reply"


# ---------------------------------------------------------------------------
# 7. end-to-end main() — happy path + every REFUSE path
# ---------------------------------------------------------------------------
def _patch_common(monkeypatch, human_bin_ok=True, memory_db_ok=True):
    """Common monkeypatches so main() doesn't need a real `human` binary or a
    real memory.db — those are exercised by the (documented, manual) live run
    against production, not by this offline suite."""
    monkeypatch.setattr(G, "copy_memory_db",
                        lambda src, dst: (str(Path(dst) / "memory.db") if memory_db_ok else None))

    def fake_semantic_search(human_bin, memory_db, embed_url, query, k, timeout=90):
        return ["a relevant memory about " + query[:10]]
    monkeypatch.setattr(G, "semantic_search", fake_semantic_search)

    def fake_score_arm(human_bin, rows, timeout=90):
        if not human_bin_ok or not rows:
            return None
        return {
            "n": len(rows),
            "axes": {
                "anti_ai": {"mean": 0.8, "stderr": 0.01, "n": len(rows)},
                "relationship": {"mean": 0.0, "stderr": 0.0, "n": 0},
                "fidelity": {"mean": 0.0, "stderr": 0.0, "n": 0, "available": False},
            },
        }
    monkeypatch.setattr(G, "score_arm", fake_score_arm)
    monkeypatch.setattr(G, "build_system_prompt", lambda args: "FAKE SYSTEM PROMPT")


def test_main_happy_path_writes_promote_or_hold(monkeypatch, fake_server, contexts_file, tmp_path):
    _patch_common(monkeypatch)
    out = str(tmp_path / "gate.json")
    rc = G.main(_base_args(fake_server, contexts_file, out))
    assert rc in (0, 1)
    doc = json.loads(Path(out).read_text())
    assert doc["verdict"] in ("PROMOTE", "HOLD")
    assert doc["shadow"]["n_replies"] >= 30
    assert doc["live"]["n_replies"] >= 30
    assert doc["shadow"]["n_ei"] >= 30
    assert doc["live"]["n_ei"] >= 30


def test_main_refuses_when_embedder_unreachable(monkeypatch, fake_server, contexts_file, tmp_path):
    _patch_common(monkeypatch)
    out = str(tmp_path / "gate.json")
    args = _base_args(fake_server, contexts_file, out, extra=["--embed-url", "http://127.0.0.1:1"])
    rc = G.main(args)
    assert rc == 2
    assert not Path(out).exists()


def test_main_refuses_when_judge_unreachable(monkeypatch, fake_server, contexts_file, tmp_path):
    _patch_common(monkeypatch)

    def boom(*a, **k):
        raise RuntimeError("no ADC credentials found")
    monkeypatch.setattr(G, "call_gemini", boom)
    out = str(tmp_path / "gate.json")
    rc = G.main(_base_args(fake_server, contexts_file, out))
    assert rc == 2
    assert not Path(out).exists()


def test_main_refuses_when_too_few_contexts(monkeypatch, fake_server, tmp_path):
    _patch_common(monkeypatch)
    thin = tmp_path / "thin.jsonl"
    thin.write_text("\n".join(json.dumps({"incoming": f"only a few {i}"}) for i in range(5)) + "\n")
    out = str(tmp_path / "gate.json")
    rc = G.main(_base_args(fake_server, str(thin), out))
    assert rc == 2
    assert not Path(out).exists()


def test_main_refuses_when_memory_db_copy_fails(monkeypatch, fake_server, contexts_file, tmp_path):
    _patch_common(monkeypatch, memory_db_ok=False)
    out = str(tmp_path / "gate.json")
    rc = G.main(_base_args(fake_server, contexts_file, out))
    assert rc == 2
    assert not Path(out).exists()


def test_main_refuses_when_semantic_search_always_fails(monkeypatch, fake_server, contexts_file,
                                                         tmp_path):
    """LIVE arm can't be measured at all -> too few LIVE replies -> refuse,
    not a silent SHADOW-only verdict."""
    _patch_common(monkeypatch)
    monkeypatch.setattr(G, "semantic_search", lambda *a, **k: None)
    out = str(tmp_path / "gate.json")
    rc = G.main(_base_args(fake_server, contexts_file, out))
    assert rc == 2
    assert not Path(out).exists()


def test_main_refuses_when_eval_score_unavailable(monkeypatch, fake_server, contexts_file,
                                                   tmp_path):
    _patch_common(monkeypatch, human_bin_ok=False)
    out = str(tmp_path / "gate.json")
    rc = G.main(_base_args(fake_server, contexts_file, out))
    assert rc == 2
    assert not Path(out).exists()


def test_main_refuses_when_generation_mostly_fails(monkeypatch, contexts_file, tmp_path):
    """Server reachable for preflight but chat completions fail for most
    requests -> not enough scored replies -> refuse."""
    _patch_common(monkeypatch)

    class _FlakyHandler(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_POST(self):  # noqa: N802
            length = int(self.headers.get("Content-Length", 0))
            self.rfile.read(length)
            if self.path == "/v1/embeddings":
                payload = json.dumps({"data": [{"embedding": [0.0]}]}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
                return
            # Every chat-completions call fails.
            self.send_response(500)
            self.end_headers()

    server = HTTPServer(("127.0.0.1", 0), _FlakyHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        base = f"http://127.0.0.1:{server.server_port}"
        out = str(tmp_path / "gate.json")
        rc = G.main(_base_args(base, contexts_file, out))
        assert rc == 2
        assert not Path(out).exists()
    finally:
        server.shutdown()
        thread.join(timeout=5)


def test_main_never_writes_partial_output_on_refuse(monkeypatch, fake_server, contexts_file,
                                                     tmp_path):
    """Belt-and-suspenders: a REFUSE must never leave a stale/partial gate
    file at --out (no-number-without-a-measurement.md)."""
    _patch_common(monkeypatch)
    out = tmp_path / "gate.json"
    out.write_text("PRE-EXISTING SENTINEL")
    rc = G.main(_base_args(fake_server, contexts_file, str(out),
                           extra=["--embed-url", "http://127.0.0.1:1"]))
    assert rc == 2
    # REFUSE must not touch --out at all, including an existing file there.
    assert out.read_text() == "PRE-EXISTING SENTINEL"


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
