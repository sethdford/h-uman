"""Hermetic pytest suite for the fusion-honesty fixes in
scripts/eval_semantic_live_gate.py (the --fusion promotion pair: HU_HYBRID_FUSION
rrf|score). Reuses the fixtures already established in
scripts/test_eval_semantic_live_gate.py (fake_server, fake_judge, contexts_file,
_base_args, _patch_common) rather than redefining them — see that file's own
docstring for the network-free/no-real-binary discipline this suite follows.

Two honesty gaps found in review of PR #556 ("gated score-level lexical+dense
fusion"), both covered here:

  B1. A --fusion arm can PASS vacuously when the CLI child process reports
      (via stderr, not returncode) that the semantic index was unavailable
      and it silently ran keyword-only. Fixed by hybrid_search() surfacing
      SEMANTIC_UNAVAILABLE_MARKER and main() refusing outright (exit 2, no
      file) rather than let that call count toward the pairing.
  B2. Even without an outright failure, a --fusion pair whose two arms
      retrieved near-identical contexts measured the A/A noise floor, not
      the requested merge mode. Fixed by contexts_differing_fraction() +
      decide_verdict()'s new --min-diff-frac check, which forces INCONCLUSIVE
      when too few paired contexts actually differed between arms.

Plus task A: the `limitations` field documenting that graph-boost /
typed-seeding recall is not reachable from the CLI path this gate uses.

No network, no real `human` binary, no chat.db/memory.db — the fake HTTP
server stands in for :8741 + the embedder (started per-test, random port),
and HU_GATE_FAKE=1 short-circuits the Gemini judge (see fake_judge fixture).
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
import eval_semantic_live_gate as G  # noqa: E402
from test_eval_semantic_live_gate import (  # noqa: E402
    fake_server, fake_judge, contexts_file, _base_args, _patch_common,
)

# Re-exported so pytest sees them as fixtures of THIS module too (imported
# fixture functions are resolved normally; see pytest docs on fixture
# availability). Silence "imported but unused" for fake_server/fake_judge:
# they are used implicitly via pytest's fixture-injection, not by name.
assert fake_server and fake_judge


# ---------------------------------------------------------------------------
# B1 — the semantic-unavailable-marker false-PASS path
# ---------------------------------------------------------------------------
def test_fusion_semantic_unavailable_marker_in_live_arm_refuses_and_writes_nothing(
        monkeypatch, fake_server, contexts_file, tmp_path):
    """If the CLI reports the keyword-only fallback for the LIVE arm's calls
    (fusion_env HU_HYBRID_FUSION == 'score'), the run must refuse outright —
    exit non-zero, no verdict/gate JSON written — never a PROMOTE/HOLD
    computed on a keyword-only fallback wearing a --fusion score label."""
    _patch_common(monkeypatch)

    def fake_hybrid(human_bin, memory_db, embed_url, query, k, fusion_env, timeout=90):
        semantic_unavailable = fusion_env["HU_HYBRID_FUSION"] == "score"
        return ["a memory about " + query[:10]], semantic_unavailable
    monkeypatch.setattr(G, "hybrid_search", fake_hybrid)

    out = tmp_path / "gate.json"
    rc = G.main(_base_args(fake_server, contexts_file, str(out),
                           extra=["--fusion", "score", "--alpha", "0.4"]))
    assert rc == 2
    assert not out.exists()


def test_fusion_semantic_unavailable_marker_in_shadow_arm_refuses_and_writes_nothing(
        monkeypatch, fake_server, contexts_file, tmp_path):
    """Same as above but the marker fires on the SHADOW (rrf baseline) arm
    instead — either arm being invalid must refuse the whole run."""
    _patch_common(monkeypatch)

    def fake_hybrid(human_bin, memory_db, embed_url, query, k, fusion_env, timeout=90):
        semantic_unavailable = fusion_env["HU_HYBRID_FUSION"] == "rrf"
        return ["a memory about " + query[:10]], semantic_unavailable
    monkeypatch.setattr(G, "hybrid_search", fake_hybrid)

    out = tmp_path / "gate.json"
    rc = G.main(_base_args(fake_server, contexts_file, str(out),
                           extra=["--fusion", "score", "--alpha", "0.4"]))
    assert rc == 2
    assert not out.exists()


def test_fusion_no_marker_does_not_refuse(monkeypatch, fake_server, contexts_file, tmp_path):
    """Control: with the marker absent from every call, the refusal path
    must NOT fire — proves the two tests above are testing the marker, not
    some other reason main() might refuse."""
    _patch_common(monkeypatch)

    def fake_hybrid(human_bin, memory_db, embed_url, query, k, fusion_env, timeout=90):
        return ["memory via " + fusion_env["HU_HYBRID_FUSION"] + " for " + query[:10]], False
    monkeypatch.setattr(G, "hybrid_search", fake_hybrid)

    out = tmp_path / "gate.json"
    rc = G.main(_base_args(fake_server, contexts_file, str(out),
                           extra=["--fusion", "score", "--alpha", "0.4"]))
    assert rc in (0, 1)
    assert out.exists()


def test_hybrid_search_marker_detection_is_stderr_based_not_returncode(monkeypatch, tmp_path):
    """Unit-level pin on hybrid_search() itself: the CLI's fallback returns 0
    with well-formed stdout, so returncode/empty-result checks alone cannot
    see it — only the exact stderr marker can."""
    human = tmp_path / "human"
    human.write_text("")

    class P:
        returncode = 0
        stdout = "  [1] k1 (0.500): a keyword-only memory\n"
        stderr = "search --hybrid: semantic index unavailable, using keyword only\n"
    monkeypatch.setattr(G.subprocess, "run", lambda *a, **k: P())
    results, semantic_unavailable = G.hybrid_search(str(human), "/tmp/m.db", "http://e", "q", 5,
                                                     G.fusion_env_for_arm("live", "score", 0.3))
    assert results == ["a keyword-only memory"]
    assert semantic_unavailable is True


def test_semantic_unavailable_ids_reads_the_run_arm_flag():
    results = {0: {"semantic_unavailable": False}, 1: {"semantic_unavailable": True},
               2: {"semantic_unavailable": True}}
    assert G.semantic_unavailable_ids(results) == [1, 2]


# ---------------------------------------------------------------------------
# B2 — contexts_differing / contexts_total floor
# ---------------------------------------------------------------------------
def test_fusion_identical_contexts_across_all_queries_forces_inconclusive(
        monkeypatch, fake_server, contexts_file, tmp_path):
    """Both arms retrieve the SAME memory content regardless of fusion mode
    (the shape of the false-PASS this gate exists to catch, minus an
    outright stderr marker — e.g. a merge bug that ignores HU_HYBRID_FUSION
    entirely). contexts_differing must read 0 and the verdict must be forced
    INCONCLUSIVE, not PROMOTE/HOLD."""
    _patch_common(monkeypatch)

    def fake_hybrid(human_bin, memory_db, embed_url, query, k, fusion_env, timeout=90):
        return ["the same memory no matter the fusion mode"], False
    monkeypatch.setattr(G, "hybrid_search", fake_hybrid)

    out = tmp_path / "gate.json"
    rc = G.main(_base_args(fake_server, contexts_file, str(out),
                           extra=["--fusion", "score", "--alpha", "0.4"]))
    assert rc == 1  # INCONCLUSIVE is a written verdict, not a refusal
    doc = json.loads(out.read_text())
    assert doc["verdict"] == "INCONCLUSIVE"
    assert doc["contexts_differing"] == 0
    assert doc["contexts_total"] == 32
    assert doc["contexts_differing_frac"] == 0.0
    assert any("contexts_differing fraction" in r for r in doc["reasons"])


def test_fusion_diff_frac_above_floor_with_a_real_improvement_computes_verdict_normally(
        monkeypatch, fake_server, contexts_file, tmp_path):
    """Contexts genuinely differ between arms (diff_frac 1.0, well above the
    0.05 floor) AND the LIVE arm's reply is judged better than SHADOW's on
    every paired context — the verdict must be computed normally (PROMOTE),
    not short-circuited to INCONCLUSIVE by the new floor."""
    _patch_common(monkeypatch)

    def fake_hybrid(human_bin, memory_db, embed_url, query, k, fusion_env, timeout=90):
        return ["memory via " + fusion_env["HU_HYBRID_FUSION"] + " for " + query[:10]], False
    monkeypatch.setattr(G, "hybrid_search", fake_hybrid)

    def fake_generate(server, model, sp, ctx, max_tokens, temperature):
        return "improved reply" if "via score" in sp else "baseline reply"
    monkeypatch.setattr(G, "generate", fake_generate)

    def fake_judge_fn(incoming, reply):
        return {"ei": 5, "reality": 5} if reply == "improved reply" else {"ei": 3, "reality": 3}
    monkeypatch.setattr(G, "judge_ei_reality", fake_judge_fn)

    out = tmp_path / "gate.json"
    rc = G.main(_base_args(fake_server, contexts_file, str(out),
                           extra=["--fusion", "score", "--alpha", "0.4"]))
    assert rc == 0
    doc = json.loads(out.read_text())
    assert doc["verdict"] == "PROMOTE"
    assert doc["contexts_differing"] == doc["contexts_total"] == 32
    assert doc["contexts_differing_frac"] == 1.0
    assert doc["live"]["ei_mean"] > doc["shadow"]["ei_mean"]


def test_contexts_differing_fraction_pure_function():
    shadow = {0: {"context_hash": "a"}, 1: {"context_hash": "b"}, 2: {"context_hash": "c"}}
    live = {0: {"context_hash": "a"}, 1: {"context_hash": "different"}, 2: {"context_hash": "c"}}
    differing, total, frac = G.contexts_differing_fraction(shadow, live, [0, 1, 2])
    assert (differing, total) == (1, 3)
    assert abs(frac - (1 / 3)) < 1e-9


def test_contexts_differing_fraction_empty_ids_is_zero_not_a_crash():
    assert G.contexts_differing_fraction({}, {}, []) == (0, 0, 0.0)


def test_decide_verdict_forces_inconclusive_below_diff_frac_floor():
    summary = {"composite": 0.8, "ei_mean": 4.0, "reality_mean": 4.0}
    verdict, reasons = G.decide_verdict(summary, summary, recall_coverage=1.0,
                                        diff_frac=0.01, min_diff_frac=0.05)
    assert verdict == "INCONCLUSIVE"
    assert any("diff_frac" in r or "contexts_differing" in r for r in reasons)


def test_decide_verdict_diff_frac_none_skips_the_check():
    """Non-fusion callers pass diff_frac=None; the check must be a no-op,
    not a crash or a spurious INCONCLUSIVE."""
    summary = {"composite": 0.8, "ei_mean": 4.0, "reality_mean": 4.0}
    verdict, reasons = G.decide_verdict(summary, summary, recall_coverage=1.0, diff_frac=None)
    assert verdict == "PROMOTE"
    assert reasons == []


def test_decide_verdict_diff_frac_at_floor_is_not_inconclusive():
    summary = {"composite": 0.8, "ei_mean": 4.0, "reality_mean": 4.0}
    verdict, _ = G.decide_verdict(summary, summary, recall_coverage=1.0,
                                  diff_frac=0.05, min_diff_frac=0.05)
    assert verdict == "PROMOTE"


# ---------------------------------------------------------------------------
# Task A — the `limitations` field (graph-boost not measured by this gate)
# ---------------------------------------------------------------------------
def test_limitations_field_present_on_the_default_non_fusion_gate(
        monkeypatch, fake_server, contexts_file, tmp_path):
    _patch_common(monkeypatch, recall_hit_ratio=1.0)
    out = tmp_path / "gate.json"
    rc = G.main(_base_args(fake_server, contexts_file, str(out)))
    assert rc in (0, 1)
    doc = json.loads(out.read_text())
    assert doc["limitations"] == [G.GRAPH_BOOST_LIMITATION]
    assert "graph" in G.GRAPH_BOOST_LIMITATION.lower()
    assert "graph=NULL" in G.GRAPH_BOOST_LIMITATION or "graph" in G.GRAPH_BOOST_LIMITATION


def test_limitations_field_present_on_a_fusion_gate(monkeypatch, fake_server, contexts_file,
                                                    tmp_path):
    _patch_common(monkeypatch)

    def fake_hybrid(human_bin, memory_db, embed_url, query, k, fusion_env, timeout=90):
        return ["memory via " + fusion_env["HU_HYBRID_FUSION"] + " for " + query[:10]], False
    monkeypatch.setattr(G, "hybrid_search", fake_hybrid)
    out = tmp_path / "gate.json"
    rc = G.main(_base_args(fake_server, contexts_file, str(out),
                           extra=["--fusion", "score", "--alpha", "0.4"]))
    assert rc in (0, 1)
    doc = json.loads(out.read_text())
    assert doc["limitations"] == [G.GRAPH_BOOST_LIMITATION]


def test_semantic_unavailable_marker_constant_matches_the_c_source():
    """Pin the constant to the ACTUAL stderr text at
    src/app/cli_commands.c (the `search --hybrid` branch): 'search --hybrid:
    semantic index unavailable, using keyword only'. If the C source's
    wording ever changes, this test (and SEMANTIC_UNAVAILABLE_MARKER) must
    change with it — that's the point of pinning the exact substring rather
    than a looser heuristic."""
    real_cli_stderr_line = "search --hybrid: semantic index unavailable, using keyword only\n"
    assert G.SEMANTIC_UNAVAILABLE_MARKER in real_cli_stderr_line
