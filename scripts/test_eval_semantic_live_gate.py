#!/usr/bin/env python3
"""pytest suite for eval_semantic_live_gate.py (Contract C1).

No network: the Gemini judge is short-circuited via HU_GATE_FAKE=1 (the
module's own fake path — see call_gemini()), and a tiny stdlib HTTP server
stands in for :8741 (chat completions) and the embedder. The `human` CLI is
never invoked directly here — semantic_search()/score_arm() are monkeypatched
so the suite doesn't depend on a compiled binary.

Three families:
  1. Pure-function tests (decide_verdict, pairing, select_contexts, parsing
     helpers) — no I/O at all.
  2. run_arm()/pairing integration tests against the fake server, proving
     failures in one arm do not silently pollute the other arm's comparison.
  3. End-to-end main() tests: the happy path, the INCONCLUSIVE path (low
     recall coverage), and every REFUSE path.
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
    # Contexts (by 0-based index in the request order) whose chat-completions
    # reply should come back empty, class-level so tests can configure it.
    EMPTY_REPLY_INDICES = set()
    _counter = {"n": 0}

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
            idx = _FakeHandler._counter["n"]
            _FakeHandler._counter["n"] += 1
            if idx in _FakeHandler.EMPTY_REPLY_INDICES:
                self._send_json({"choices": [{"message": {"content": ""}}]})
                return
            reply = "with-memories reply" if has_memories else "plain reply"
            self._send_json({"choices": [{"message": {"content": reply}}]})
            return
        self._send_json({"error": "unknown path"}, code=404)


@pytest.fixture()
def fake_server():
    _FakeHandler.EMPTY_REPLY_INDICES = set()
    _FakeHandler._counter["n"] = 0
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
        "--out", out_path,
    ]
    return args + (extra or [])


def _patch_common(monkeypatch, human_bin_ok=True, memory_db_ok=True, recall_hit_ratio=1.0):
    """Common monkeypatches so main() doesn't need a real `human` binary or a
    real memory.db — those are exercised by the (documented, manual) live run
    against production, not by this offline suite.

    recall_hit_ratio controls what fraction of semantic_search() calls return
    a non-empty snippet list, so tests can drive recall_coverage below or
    above --min-recall-coverage deterministically."""
    monkeypatch.setattr(G, "copy_memory_db",
                        lambda src, dst: (str(Path(dst) / "memory.db") if memory_db_ok else None))

    calls = {"n": 0}

    def fake_semantic_search(human_bin, memory_db, embed_url, query, k, timeout=90):
        calls["n"] += 1
        # Deterministic-but-tunable: every 1/recall_hit_ratio-th call is empty.
        if recall_hit_ratio <= 0:
            return []
        step = max(1, round(1 / recall_hit_ratio))
        if calls["n"] % step == 0:
            return ["a relevant memory about " + query[:10]]
        return []
    monkeypatch.setattr(G, "semantic_search", fake_semantic_search)

    def fake_score_arm(human_bin, rows, timeout=90):
        if not human_bin_ok or not rows:
            return None
        return {
            "n": len(rows),
            "axes": {
                "anti_ai": {"mean": 1.0, "stderr": 0.0, "n": len(rows)},
                "relationship": {"mean": 0.0, "stderr": 0.0, "n": 0},
                "fidelity": {"mean": 0.0, "stderr": 0.0, "n": 0, "available": False},
            },
        }
    monkeypatch.setattr(G, "score_arm", fake_score_arm)
    monkeypatch.setattr(G, "build_system_prompt", lambda args: "FAKE SYSTEM PROMPT")


# ---------------------------------------------------------------------------
# 1. Pure verdict logic — no I/O
# ---------------------------------------------------------------------------
def _summary(composite=0.8, ei=4.0, reality=4.0):
    return {"composite": composite, "ei_mean": ei, "reality_mean": reality}


def test_promote_when_live_matches_shadow_and_coverage_is_full():
    verdict, reasons = G.decide_verdict(_summary(), _summary(), recall_coverage=1.0)
    assert verdict == "PROMOTE"
    assert reasons == []


def test_promote_when_live_strictly_better():
    verdict, reasons = G.decide_verdict(_summary(0.7, 3.0, 3.0), _summary(0.9, 4.5, 4.5),
                                        recall_coverage=1.0)
    assert verdict == "PROMOTE"


def test_hold_when_composite_drops_beyond_tolerance():
    shadow = _summary(composite=0.80)
    live = _summary(composite=0.70)
    verdict, reasons = G.decide_verdict(shadow, live, recall_coverage=1.0, composite_tolerance=0.02)
    assert verdict == "HOLD"
    assert any("composite dropped" in r for r in reasons)


def test_composite_drop_within_tolerance_still_promotes():
    shadow = _summary(composite=0.80)
    live = _summary(composite=0.79)
    verdict, _ = G.decide_verdict(shadow, live, recall_coverage=1.0, composite_tolerance=0.02)
    assert verdict == "PROMOTE"


def test_hold_when_ei_drops_beyond_tolerance():
    """The AlpsBench finding this gate exists to catch: memory retrieval can
    degrade emotional intelligence even while other axes look fine."""
    shadow = _summary(ei=4.0)
    live = _summary(ei=3.0)
    verdict, reasons = G.decide_verdict(shadow, live, recall_coverage=1.0, ei_tolerance=0.15)
    assert verdict == "HOLD"
    assert any("emotional_intelligence dropped" in r for r in reasons)


def test_hold_when_reality_awareness_drops_beyond_tolerance():
    shadow = _summary(reality=4.0)
    live = _summary(reality=3.2)
    verdict, reasons = G.decide_verdict(shadow, live, recall_coverage=1.0, reality_tolerance=0.15)
    assert verdict == "HOLD"
    assert any("reality_awareness dropped" in r for r in reasons)


def test_hold_reasons_accumulate_for_multiple_regressions():
    shadow = _summary(composite=0.8, ei=4.0, reality=4.0)
    live = _summary(composite=0.5, ei=2.0, reality=2.0)
    verdict, reasons = G.decide_verdict(shadow, live, recall_coverage=1.0)
    assert verdict == "HOLD"
    assert len(reasons) == 3


def test_ei_improving_does_not_mask_a_composite_drop():
    """Classifier-score-plus-flag-gate style: no single strong axis should be
    able to buy back a regression on another required axis."""
    shadow = _summary(composite=0.80, ei=3.0)
    live = _summary(composite=0.60, ei=5.0)
    verdict, reasons = G.decide_verdict(shadow, live, recall_coverage=1.0)
    assert verdict == "HOLD"
    assert any("composite dropped" in r for r in reasons)


def test_inconclusive_when_recall_coverage_too_low():
    """The core fix for the false-PROMOTE bug: if LIVE's semantic search
    rarely returned anything, the two arms are effectively the same
    experiment run twice, and a matching composite/EI proves nothing."""
    shadow = _summary(composite=0.8, ei=4.0, reality=4.0)
    live = _summary(composite=0.8, ei=4.0, reality=4.0)  # identical — looks like PROMOTE
    verdict, reasons = G.decide_verdict(shadow, live, recall_coverage=0.1,
                                        min_recall_coverage=0.5)
    assert verdict == "INCONCLUSIVE"
    assert any("recall coverage" in r for r in reasons)


def test_recall_coverage_exactly_at_threshold_is_not_inconclusive():
    shadow, live = _summary(), _summary()
    verdict, _ = G.decide_verdict(shadow, live, recall_coverage=0.5, min_recall_coverage=0.5)
    assert verdict != "INCONCLUSIVE"


def test_inconclusive_takes_priority_over_a_real_regression():
    """Low coverage means the run doesn't test what it claims to test, so it
    should not ALSO claim a confident HOLD verdict on the same broken data."""
    shadow = _summary(composite=0.8)
    live = _summary(composite=0.3)  # would otherwise be a loud HOLD
    verdict, reasons = G.decide_verdict(shadow, live, recall_coverage=0.0, min_recall_coverage=0.5)
    assert verdict == "INCONCLUSIVE"
    assert len(reasons) == 1


# ---------------------------------------------------------------------------
# 2. Pairing — the bug this revision fixes
# ---------------------------------------------------------------------------
def test_paired_ids_is_the_intersection():
    shadow = {0: {}, 1: {}, 2: {}, 5: {}}
    live = {1: {}, 2: {}, 3: {}}
    assert G.paired_ids(shadow, live) == [1, 2]


def test_paired_ids_empty_when_no_overlap():
    assert G.paired_ids({0: {}}, {1: {}}) == []


def test_summarize_paired_arm_only_uses_paired_ids():
    """A context present in `results` but NOT in `ids` must not leak into the
    summary — this is exactly the bug where SHADOW's composite over 40 items
    was compared against LIVE's composite over a different 32 items."""
    results = {
        0: {"anti_ai": 1.0, "ei": 5, "reality": 5},
        1: {"anti_ai": 0.0, "ei": 1, "reality": 1},  # excluded — not in `ids`
    }
    summary = G.summarize_paired_arm(results, ids=[0])
    assert summary["n"] == 1
    assert summary["anti_ai_mean"] == 1.0
    assert summary["ei_mean"] == 5.0


def test_recall_coverage_of_counts_nonzero_recall_bytes():
    live_results = {
        0: {"recall_bytes": 120},
        1: {"recall_bytes": 0},
        2: {"recall_bytes": 45},
        3: {"recall_bytes": 0},
    }
    assert G.recall_coverage_of(live_results, ids=[0, 1, 2, 3]) == 0.5


def test_recall_coverage_of_empty_ids_is_zero():
    assert G.recall_coverage_of({}, ids=[]) == 0.0


def test_build_context_rows_carries_no_reply_text():
    shadow_results = {0: {"ei": 4, "reality": 5, "anti_ai": 1.0}}
    live_results = {0: {"ei": 3, "reality": 4, "anti_ai": 1.0, "recall_bytes": 88}}
    rows = G.build_context_rows(shadow_results, live_results, ids=[0])
    assert rows == [{
        "id": 0, "recall_bytes": 88,
        "shadow": {"ei": 4, "reality": 5, "anti_ai": 1.0},
        "live": {"ei": 3, "reality": 4, "anti_ai": 1.0},
    }]
    dumped = json.dumps(rows)
    assert "reply" not in dumped and "incoming" not in dumped


def test_run_arm_a_failure_in_one_context_does_not_appear_in_results(monkeypatch, fake_server):
    class Args:
        human_bin = "/nonexistent/human"  # forces semantic_search() -> None for "live"
        server = fake_server
        embed_url = fake_server
        model = "m"
        top_k = 5
        max_tokens = 40
        temperature = 0.7
        channel = "imessage"

    monkeypatch.setattr(G, "score_single_reply_anti_ai", lambda *a, **k: 1.0)
    results, fail_reasons = G.run_arm("live", ["ctx a", "ctx b"], "SYS", Args(), "/dev/null",
                                      log=lambda *a, **k: None)
    assert results == {}
    assert fail_reasons == {0: "semantic_search_failed", 1: "semantic_search_failed"}


# ---------------------------------------------------------------------------
# 3. select_contexts — deterministic, fixed subset
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
# 4. semantic-results parsing
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


def test_parse_semantic_results_multiline_content_with_colons():
    """Real production content looks like this (Task/Actions/Outcome blocks
    with embedded colons) — the parser must not split on the FIRST colon."""
    stdout = (
        "  [1] experience:abc (0.500): Task: do you like sectional?\n"
        "Actions: agent_turn\n"
        "Outcome: Leather sectionals are easier to clean.\n"
        "  [2] insight:1 (0.400): simple one-liner\n"
    )
    out = G._parse_semantic_results(stdout)
    assert len(out) == 2
    assert out[0].startswith("Task: do you like sectional?")
    assert "Outcome: Leather sectionals" in out[0]
    assert out[1] == "simple one-liner"


def test_build_memories_block_empty_is_none():
    assert G.build_memories_block([]) == (None, 0)
    assert G.build_memories_block(None) == (None, 0)


def test_build_memories_block_formats_bullets():
    block, dropped = G.build_memories_block(["a", "b"])
    assert dropped == 0
    assert block.startswith("Relevant memories:\n")
    assert "- a" in block and "- b" in block
    assert block.endswith("\n\n")


def test_build_memories_block_caps_per_hit_at_word_boundary(monkeypatch):
    monkeypatch.delenv("HU_SEMANTIC_RECALL_MAX_BYTES", raising=False)
    long_hit = " ".join(["word"] * 200)  # ~1000 chars
    block, dropped = G.build_memories_block([long_hit])
    assert dropped == 0
    line = block.split("\n")[1]
    assert line.startswith("- ")
    body = line[2:]
    assert len(body.encode("utf-8")) <= G.RECALL_HIT_MAX_BYTES
    assert not body.endswith(" ") and body.endswith("word")


def test_build_memories_block_respects_total_byte_budget(monkeypatch):
    monkeypatch.setenv("HU_SEMANTIC_RECALL_MAX_BYTES", "500")
    hits = [("x" * 7 + " ") * 40] * 5  # 320 chars each, 5 hits
    block, _ = G.build_memories_block(hits)
    bullets = [l for l in block.split("\n") if l.startswith("- ")]
    total = sum(len(b[2:].encode("utf-8")) for b in bullets)
    assert total <= 500
    assert 1 <= len(bullets) < 5
    # Deterministic: same input, same bytes.
    assert G.build_memories_block(hits)[0] == block


# ---------------------------------------------------------------------------
# 5. judge (fake mode) — deterministic, in-range, network-free, budgeted
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


def test_judge_gen_config_always_sets_thinking_budget():
    """gemini-3.x shares maxOutputTokens between invisible thinking and the
    reply — every call must set thinkingConfig explicitly, schema or not."""
    with_schema = G.judge_gen_config(0.2, G._EI_JUDGE_SCHEMA)
    without_schema = G.judge_gen_config(0.7, None)
    for cfg in (with_schema, without_schema):
        assert "thinkingConfig" in cfg
        assert cfg["thinkingConfig"]["thinkingBudget"] == G.JUDGE_THINKING_BUDGET
        assert isinstance(cfg["thinkingConfig"]["thinkingBudget"], int)


def test_judge_gen_config_leaves_room_for_the_json_body():
    cfg = G.judge_gen_config(0.2, G._EI_JUDGE_SCHEMA)
    budget = cfg["thinkingConfig"]["thinkingBudget"]
    assert cfg["maxOutputTokens"] - budget >= 512


# ---------------------------------------------------------------------------
# 6. embedder preflight against the fake server
# ---------------------------------------------------------------------------
def test_preflight_embedder_true_against_fake_server(fake_server):
    assert G.preflight_embedder(fake_server) is True


def test_preflight_embedder_false_when_unreachable():
    assert G.preflight_embedder("http://127.0.0.1:1") is False


# ---------------------------------------------------------------------------
# 7. generate() against the fake server, incl. the memories-block distinction
# ---------------------------------------------------------------------------
def test_generate_plain_reply(fake_server):
    reply = G.generate(fake_server, "m", "system prompt, no memories", "hey", 50, 0.7)
    assert reply == "plain reply"


def test_generate_with_memories_block_changes_response(fake_server):
    sp = "Relevant memories:\n- likes hiking\n\nsystem prompt"
    reply = G.generate(fake_server, "m", sp, "hey", 50, 0.7)
    assert reply == "with-memories reply"


# ---------------------------------------------------------------------------
# 8. end-to-end main() — happy path, INCONCLUSIVE, and every REFUSE path
# ---------------------------------------------------------------------------
def test_main_happy_path_writes_promote_or_hold_with_context_rows(monkeypatch, fake_server,
                                                                  contexts_file, tmp_path):
    _patch_common(monkeypatch, recall_hit_ratio=1.0)  # full recall coverage
    out = str(tmp_path / "gate.json")
    rc = G.main(_base_args(fake_server, contexts_file, out))
    assert rc in (0, 1)
    doc = json.loads(Path(out).read_text())
    assert doc["verdict"] in ("PROMOTE", "HOLD")
    assert doc["n_paired"] >= 30
    assert doc["shadow"]["n_ei"] >= 30
    assert doc["live"]["n_ei"] >= 30
    assert doc["recall_coverage"] == 1.0
    assert len(doc["context_rows"]) == doc["n_paired"]
    row = doc["context_rows"][0]
    assert set(row) == {"id", "recall_bytes", "shadow", "live"}
    dumped = json.dumps(doc)
    assert "real inbound message" not in dumped  # no context/reply text leaked


def test_main_inconclusive_when_recall_coverage_low(monkeypatch, fake_server, contexts_file,
                                                     tmp_path):
    _patch_common(monkeypatch, recall_hit_ratio=0.05)  # far below the 0.5 default floor
    out = str(tmp_path / "gate.json")
    rc = G.main(_base_args(fake_server, contexts_file, out))
    assert rc == 1  # INCONCLUSIVE is not a promotion — non-zero, but a file IS written
    doc = json.loads(Path(out).read_text())
    assert doc["verdict"] == "INCONCLUSIVE"
    assert doc["recall_coverage"] < 0.5
    assert any("recall coverage" in r for r in doc["reasons"])


def test_main_paired_set_excludes_arm_only_contexts(monkeypatch, fake_server, contexts_file,
                                                     tmp_path):
    """Force some LIVE generations to come back empty; the paired set (and
    every downstream mean) must exclude exactly those contexts from BOTH
    arms, not just from LIVE."""
    _patch_common(monkeypatch, recall_hit_ratio=1.0)
    # SHADOW runs first (32 calls, indices 0..31), then LIVE (indices 32..63).
    # Fail 5 LIVE contexts.
    _FakeHandler.EMPTY_REPLY_INDICES = {32, 33, 34, 35, 36}
    out = str(tmp_path / "gate.json")
    rc = G.main(_base_args(fake_server, contexts_file, out, extra=["--min-n", "27"]))
    assert rc in (0, 1)
    doc = json.loads(Path(out).read_text())
    assert doc["n_live_only"] == 0
    assert doc["n_shadow_only"] == 5
    assert doc["n_paired"] == 27  # 32 requested - 5 live failures, min-n lowered below to fit
    assert doc["shadow"]["n"] == doc["n_paired"]
    assert doc["live"]["n"] == doc["n_paired"]


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
    """LIVE arm can't be measured at all -> paired set is empty -> refuse,
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
    requests -> not enough paired replies -> refuse."""
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


# ---------------------------------------------------------------------------
# Content filter — mirrors hu_semantic_recall_hit_is_excluded (2026-09-02
# finding: episodic scaffold + AI-identity confrontation hits drive the
# adapter into think-only output that reaches the daemon as "").
# ---------------------------------------------------------------------------
def test_hit_is_excluded_for_scaffold_and_confrontation_word_boundary():
    assert G.hit_is_excluded("Task: hi\nActions: agent_turn\nOutcome: ok\nScore: 1.0000")
    assert G.hit_is_excluded("are you texting or your ai?? Can you just call?")
    assert G.hit_is_excluded("Is this Seth")
    assert G.hit_is_excluded("questioning if the recipient is an AI")
    assert G.hit_is_excluded("lol you're an AI aren't you")
    # Word boundary: "ai" inside said / wait / maid must not fire.
    assert not G.hit_is_excluded("he said to wait, the maid is coming")
    # Bare "AI" as a topic, or "Task" as a word, is a memory not a confrontation.
    assert not G.hit_is_excluded("Mel started an AI research job in Tampa")
    assert not G.hit_is_excluded("Task force meeting moved to friday")
    assert not G.hit_is_excluded("")


def test_build_memories_block_drops_excluded_hits_and_reports_count():
    snippets = [
        "Task: x\nActions: agent_turn\nOutcome: ok\nScore: 1.0000",
        "Mel has been applying for jobs in Tampa",
        "Is this Seth",
        "dinner at the waterfront place friday",
    ]
    block, dropped = G.build_memories_block(snippets)
    assert dropped == 2
    assert "Task:" not in block and "Is this Seth" not in block
    assert "- Mel has been applying" in block
    assert "- dinner at the waterfront" in block
    # All excluded -> no block at all, and the count says why.
    block, dropped = G.build_memories_block(["Is this Seth", "are you a bot"])
    assert block is None and dropped == 2
    block, dropped = G.build_memories_block([])
    assert block is None and dropped == 0
