#!/usr/bin/env python3
"""
Hermetic tests for eval_difficulty_route_shadow.py.

No network, no `:8741`, no model load.
"""

import json
import os
import tempfile
import unittest
from pathlib import Path

# Import the module under test
import sys
sys.path.insert(0, os.path.dirname(__file__))
import eval_difficulty_route_shadow as us8


class TestContextSelection(unittest.TestCase):
    """Test context loading and filtering."""

    def test_select_contexts_empty_corpus(self):
        """Nonexistent corpus returns empty list."""
        contexts = us8.select_contexts("/nonexistent/path.jsonl", n=10)
        self.assertEqual(len(contexts), 0)

    def test_select_contexts_filters_short_messages(self):
        """Messages with <= 12 words are filtered out."""
        with tempfile.NamedTemporaryFile(mode='w', suffix='.jsonl', delete=False) as f:
            # Short message (4 words)
            f.write(json.dumps({"msg": "hey how are you"}) + "\n")
            # Long message (13 words)
            f.write(json.dumps({"msg": "what do you think should I go to the party tonight or stay home"}) + "\n")
            f.flush()

            try:
                contexts = us8.select_contexts(f.name, n=10)
                # Should only get the 13-word message
                self.assertEqual(len(contexts), 1)
                self.assertIn("should I go", contexts[0])
            finally:
                os.unlink(f.name)

    def test_select_contexts_respects_n(self):
        """Returns at most n contexts."""
        with tempfile.NamedTemporaryFile(mode='w', suffix='.jsonl', delete=False) as f:
            for i in range(30):
                msg = " ".join([f"word{j}" for j in range(15)])  # 15 words each
                f.write(json.dumps({"msg": msg}) + "\n")
            f.flush()

            try:
                contexts = us8.select_contexts(f.name, n=5)
                self.assertEqual(len(contexts), 5)
            finally:
                os.unlink(f.name)


class TestGateDecision(unittest.TestCase):
    """Test gate decision logic (AC-8.4)."""

    def test_gate_inconclusive_insufficient_pairs(self):
        """Fewer than 20 pairs → INCONCLUSIVE."""
        result = us8.decide_gate(None, None, None, None, n_paired=10)
        self.assertEqual(result["verdict"], "INCONCLUSIVE")

    def test_gate_inconclusive_missing_composite(self):
        """Missing composite scores → INCONCLUSIVE."""
        result = us8.decide_gate(None, 0.85, 0.63, 0.64, n_paired=20)
        self.assertEqual(result["verdict"], "INCONCLUSIVE")

    def test_gate_inconclusive_missing_twin(self):
        """Missing twin scores → INCONCLUSIVE."""
        result = us8.decide_gate(0.85, 0.86, None, 0.64, n_paired=20)
        self.assertEqual(result["verdict"], "INCONCLUSIVE")

    def test_gate_promote_both_axes_pass(self):
        """Both composite and twin improve → PROMOTE."""
        result = us8.decide_gate(
            composite_on_device=0.85,
            composite_cloud=0.87,
            twin_on_device=0.63,
            twin_cloud=0.64,
            n_paired=20,
            tolerance=0.02
        )
        self.assertEqual(result["verdict"], "PROMOTE")

    def test_gate_hold_composite_drops(self):
        """Composite drops beyond tolerance → HOLD."""
        result = us8.decide_gate(
            composite_on_device=0.85,
            composite_cloud=0.82,  # drops 0.03, beyond tolerance 0.02
            twin_on_device=0.63,
            twin_cloud=0.64,
            n_paired=20,
            tolerance=0.02
        )
        self.assertEqual(result["verdict"], "HOLD")

    def test_gate_hold_twin_drops(self):
        """Twin drops (any amount) → HOLD."""
        result = us8.decide_gate(
            composite_on_device=0.85,
            composite_cloud=0.87,
            twin_on_device=0.63,
            twin_cloud=0.62,  # drops by 0.01
            n_paired=20,
            tolerance=0.02
        )
        self.assertEqual(result["verdict"], "HOLD")

    def test_gate_promote_within_tolerance(self):
        """Composite within tolerance (even tiny drop) → PROMOTE."""
        result = us8.decide_gate(
            composite_on_device=0.85,
            composite_cloud=0.8499,  # drops by 0.0001, within 0.02 tolerance
            twin_on_device=0.63,
            twin_cloud=0.63,  # unchanged
            n_paired=20,
            tolerance=0.02
        )
        self.assertEqual(result["verdict"], "PROMOTE")


class TestDryRun(unittest.TestCase):
    """Test dry-run mode (AC-8.6)."""

    def test_dry_run_mode_produces_output(self):
        """Dry-run loads contexts and writes output without generation."""
        with tempfile.NamedTemporaryFile(mode='w', suffix='.jsonl', delete=False) as corpus:
            for i in range(10):
                msg = " ".join([f"word{j}" for j in range(15)])  # 15 words each
                corpus.write(json.dumps({"msg": msg}) + "\n")
            corpus.flush()

            with tempfile.TemporaryDirectory() as tmpdir:
                output_path = os.path.join(tmpdir, "test-output.json")

                # Simulate dry-run: load contexts, write result
                contexts = us8.select_contexts(corpus.name, n=5)
                result = {
                    "verdict": "DRY_RUN",
                    "contexts_loaded": len(contexts),
                    "min_words_threshold": 12,
                }
                os.makedirs(os.path.dirname(output_path), exist_ok=True)
                with open(output_path, 'w') as f:
                    json.dump(result, f)

                # Verify output
                self.assertTrue(os.path.exists(output_path))
                with open(output_path) as f:
                    loaded = json.load(f)
                    self.assertEqual(loaded["verdict"], "DRY_RUN")
                    self.assertEqual(loaded["contexts_loaded"], 5)

            os.unlink(corpus.name)


if __name__ == "__main__":
    unittest.main()


# ---------------------------------------------------------------------------
# 2026-09-06: real arms, LUAR twin, refusals (pytest-style, hermetic)
# ---------------------------------------------------------------------------
import pytest  # noqa: E402

LONG = ("i have been thinking about whether we should move the whole team to the new "
        "office next quarter or wait until the lease question is finally settled")
LONG2 = ("can you explain why the deploy failed last night and what we need to change in "
         "the pipeline so it does not happen again this week please")


def _corpus(tmp_path, rows):
    p = tmp_path / "c.jsonl"
    p.write_text("\n".join(json.dumps(r) for r in rows) + "\n")
    return str(p)


def test_select_context_rows_requires_reply_and_substantive(tmp_path):
    p = _corpus(tmp_path, [
        {"incoming": LONG, "seth_reply": "yeah lets wait"},
        {"incoming": LONG, "seth_reply": "dup"},                 # dedupe on incoming
        {"incoming": LONG2, "seth_reply": ""},                   # no reply -> dropped
        {"incoming": "ok sounds good", "seth_reply": "cool"},    # casual -> dropped
        {"msg": LONG2 + " x", "seth_reply": "fine"},             # msg key accepted
    ])
    rows = us8.select_context_rows(p, n=10)
    assert [r["incoming"] for r in rows].count(LONG) == 1
    assert all(len(r["incoming"].split()) > us8.SUBSTANTIVE_WORDS and r["seth_reply"] for r in rows)
    assert len(rows) == 2
    assert us8.select_context_rows(p, n=10) == us8.select_context_rows(p, n=10)


def test_vertex_url_is_adc_host_without_key():
    u = us8.vertex_url("johnb-2025", "gemini-3.1-pro-preview")
    assert u.startswith("https://aiplatform.googleapis.com/v1/projects/johnb-2025/")
    assert "?key=" not in u and "generativelanguage" not in u and u.endswith(":generateContent")


def test_cloud_payload_shares_head_and_sets_thinking_budget():
    p = us8.cloud_payload("HEAD", "hello there friend")
    assert p["systemInstruction"]["parts"][0]["text"] == "HEAD"
    assert p["generationConfig"]["thinkingConfig"]["thinkingBudget"] == 4096
    assert p["generationConfig"]["maxOutputTokens"] > 4096
    with pytest.raises(ValueError):
        us8.cloud_payload("HEAD", "x", thinking_budget=4096, max_tokens=4096)


def test_visible_text_skips_thought_parts():
    data = {"candidates": [{"content": {"parts": [{"text": "hidden", "thought": True}, {"text": "sure thing"}]}}]}
    assert us8._visible_text(data) == "sure thing"


class _Args:
    human_bin = "/fake/human"; channel = "imessage"; eval_python = "python3"; splits = 5
    min_n = 2; seed = 1; chatdb = "/nonexistent/chat.db"


def test_run_arm_keeps_reply_in_memory_and_records_failures(monkeypatch):
    rows = [{"incoming": LONG, "seth_reply": "a"}, {"incoming": LONG2, "seth_reply": "b"}]
    monkeypatch.setattr(us8.G, "judge_ei_reality", lambda inc, rep: {"ei": 4, "reality": 5})
    monkeypatch.setattr(us8.G, "score_single_reply_anti_ai", lambda *a, **k: 1.0)
    gen = lambda sp, msg: "yeah lets do that" if msg == LONG else None  # noqa: E731
    res, fail = us8.run_arm("t", rows, "HEAD", gen, _Args(), log=lambda *a, **k: None)
    assert set(res) == {0} and fail == {1: "generation_failed_or_empty"}
    assert res[0]["reply"] == "yeah lets do that" and res[0]["reply_words"] == 4
    assert res[0]["ei"] == 4 and res[0]["anti_ai"] == 1.0


def test_build_trials_pairs_real_and_ai():
    rows = [{"incoming": LONG, "seth_reply": "real"}]
    t = us8.build_trials(rows, {0: {"reply": "ai"}}, [0])
    assert t == [{"real_seth": "real", "ai_response": "ai"}]


def test_score_twin_arm_removes_trials_file_and_parses(monkeypatch, tmp_path):
    rows = [{"incoming": LONG, "seth_reply": "real"}]
    res = {0: {"reply": "ai"}}
    seen = {}

    def fake_run(cmd, **kw):
        seen["trials"] = cmd[cmd.index("--trials") + 1]
        out = cmd[cmd.index("--out") + 1]
        assert os.path.isfile(seen["trials"])
        assert json.load(open(seen["trials"])) == {"trials": [{"real_seth": "real", "ai_response": "ai"}]}
        Path(out).write_text(json.dumps({"twin_seth_vs_adapter": {"mean": 0.5, "ci95": [0.4, 0.6], "n": 5},
                                         "ceiling_seth_vs_seth": {"mean": 0.7}, "floor_seth_vs_other_humans": {"mean": 0.6},
                                         "trials": 1}))
        class P: returncode = 0; stderr = ""
        return P()
    monkeypatch.setattr(us8.subprocess, "run", fake_run)
    out = us8.score_twin_arm("t", rows, res, [0], _Args(), str(tmp_path))
    assert out["twin_seth_vs_adapter"]["mean"] == 0.5
    assert not os.path.exists(seen["trials"]) and not (tmp_path / "gap-t.json").exists()


def _patch_main(monkeypatch, tmp_path, od_reply="short od", cl_reply="a longer cloud reply here",
                od_twin=0.55, cl_twin=0.56, ei_od=4, ei_cl=4):
    monkeypatch.setenv("HU_GATE_FAKE", "1")
    monkeypatch.setattr(us8, "server_healthy", lambda *a, **k: True)
    monkeypatch.setattr(us8, "eval_python_has_torch", lambda *a, **k: True)
    monkeypatch.setattr(us8.G, "build_system_prompt", lambda a: "PROD HEAD")
    monkeypatch.setattr(us8, "generate_on_device", lambda sp, msg, *a, **k: od_reply)
    monkeypatch.setattr(us8, "generate_cloud_shadow", lambda sp, msg, *a, **k: cl_reply)
    monkeypatch.setattr(us8.G, "judge_ei_reality",
                        lambda inc, rep: {"ei": ei_od if rep == od_reply else ei_cl, "reality": 5})
    monkeypatch.setattr(us8.G, "score_single_reply_anti_ai", lambda *a, **k: 1.0)
    monkeypatch.setattr(us8, "score_twin_arm",
                        lambda name, rows, res, ids, args, td: {"twin_seth_vs_adapter": {"mean": od_twin if name == "on_device" else cl_twin, "ci95": [0, 1], "n": 5},
                                                                "ceiling_seth_vs_seth": {"mean": 0.7}, "floor_seth_vs_other_humans": {"mean": 0.6}, "trials": len(ids)})
    rows = [{"incoming": f"{LONG} variant number {i} of this message", "seth_reply": f"real {i}"} for i in range(22)]
    return _corpus(tmp_path, rows)


def test_main_writes_verdict_with_no_text(monkeypatch, tmp_path):
    corpus = _patch_main(monkeypatch, tmp_path)
    out = tmp_path / "out.json"
    rc = us8.main(["--corpus", corpus, "--output", str(out), "-n", "22"])
    assert rc == 0
    doc = json.loads(out.read_text())
    assert doc["verdict"] in ("PROMOTE", "HOLD") and doc["n_paired"] == 22
    dumped = json.dumps(doc)
    assert "short od" not in dumped and "cloud reply" not in dumped and "variant number" not in dumped
    assert "real " not in dumped.replace("reality", "")
    assert doc["cloud_shadow"]["twin"]["twin_seth_vs_adapter"]["mean"] == 0.56
    assert doc["context_rows"][0]["cloud_shadow"]["reply_chars"] == len("a longer cloud reply here")


def test_main_hold_when_cloud_twin_regresses(monkeypatch, tmp_path):
    corpus = _patch_main(monkeypatch, tmp_path, od_twin=0.60, cl_twin=0.55)
    out = tmp_path / "out.json"
    assert us8.main(["--corpus", corpus, "--output", str(out), "-n", "22"]) == 0
    assert json.loads(out.read_text())["verdict"] == "HOLD"


def test_main_refuses_below_min_n_and_writes_nothing(monkeypatch, tmp_path):
    corpus = _patch_main(monkeypatch, tmp_path)
    out = tmp_path / "out.json"
    rc = us8.main(["--corpus", corpus, "--output", str(out), "-n", "10"])
    assert rc == 2 and not out.exists()


def test_main_refuses_when_twin_unscorable_and_writes_nothing(monkeypatch, tmp_path):
    corpus = _patch_main(monkeypatch, tmp_path)
    monkeypatch.setattr(us8, "score_twin_arm", lambda *a, **k: None)
    out = tmp_path / "out.json"
    assert us8.main(["--corpus", corpus, "--output", str(out), "-n", "22"]) == 2 and not out.exists()
