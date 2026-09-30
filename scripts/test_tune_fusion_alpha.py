#!/usr/bin/env python3
"""Hermetic tests for scripts/tune_fusion_alpha.py (and the sampler/env helpers it
shares with scripts/eval_memory_benchmarks.py). No binary, no embedder, no ~/.human.

Run with: pytest scripts/test_tune_fusion_alpha.py -v
"""
import json
import math
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))

import eval_memory_benchmarks as emb  # noqa: E402
import tune_fusion_alpha as tfa  # noqa: E402


def rec(group, per_alpha, rrf=None, hit=None):
    """A record whose alpha arms all score `per_alpha(a)` on every metric."""
    arms = {tfa.alpha_arm(a): {m: per_alpha(a) for m in tfa.METRIC_NAMES} for a in tfa.ALPHAS}
    base = {m: (rrf if rrf is not None else 0.5) for m in tfa.METRIC_NAMES}
    arms.update({"kw": dict(base), "sem": dict(base), "rrf": dict(base)})
    return {"group": group, "sem_empty": False, "arms": arms}


def test_alpha_grid_is_eleven_exact_tenths():
    assert tfa.ALPHAS == (0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0)
    assert [tfa.alpha_arm(a) for a in (0.0, 0.3, 1.0)] == ["alpha=0.0", "alpha=0.3", "alpha=1.0"]


def test_metrics_hit_recall_and_ndcg():
    m = tfa.metrics(["x", "g1", "y", "z", "w", "g2"], {"g1", "g2"})
    assert m["hit@1"] == 0.0
    assert m["r@5"] == 1.0 and m["r@10"] == 1.0
    # one relevant at rank 2 in the top 5; ideal has two relevant at ranks 1-2
    assert m["ndcg@5"] == pytest.approx((1 / math.log2(3)) / (1 + 1 / math.log2(3)))
    top = tfa.metrics(["g"], {"g"})
    assert top == {"hit@1": 1.0, "r@5": 1.0, "r@10": 1.0, "ndcg@5": 1.0}
    miss = tfa.metrics([], {"g"})
    assert miss == {"hit@1": 0.0, "r@5": 0.0, "r@10": 0.0, "ndcg@5": 0.0}


def test_metrics_refuses_empty_gold():
    with pytest.raises(ValueError):
        tfa.metrics(["a"], set())


def test_units_dedupe_in_rank_order():
    assert tfa.lme_units(["s7:t1", "s3:t0", "s7:t4", "s9:t2"]) == ["7", "3", "9"]
    keys = [f"D1:{i}" for i in range(12)] + ["D1:0"]
    assert tfa.locomo_units(keys) == keys[:10]


def test_loco_selects_on_the_other_groups_not_the_held_out_one():
    # Group A peaks at alpha 0.2; B and C peak at 0.8. Fold A must pick 0.8 (from
    # B+C) and score A at 0.8, not at A's own best -- that gap is the point of LOCO.
    peak = lambda p: (lambda a: round(1.0 - abs(a - p), 4))  # noqa: E731
    records = [rec("A", peak(0.2)), rec("B", peak(0.8)), rec("C", peak(0.8))]
    out = tfa.loco(records)
    assert out["folds"] == 3 and out["n"] == 3
    # fold A -> 0.8; fold B -> {A,C} tie over [0.2..0.8] -> nearest 0.5 -> 0.5; fold C likewise
    assert out["selected_alpha_counts"] == {"0.5": 2, "0.8": 1}
    assert out["loco_selected_alpha"] == 0.5
    held = (peak(0.2)(0.8) + peak(0.8)(0.5) + peak(0.8)(0.5)) / 3
    assert out["held_out"]["hit@1"] == pytest.approx(held, abs=1e-4)
    assert out["in_sample_best_alpha_optimistic"] == 0.8  # mean peaks where 2 of 3 groups do
    assert out["rrf_same_questions"]["hit@1"] == 0.5


def test_select_alpha_ties_break_toward_half_then_smaller():
    flat = [rec("A", lambda a: 0.7), rec("B", lambda a: 0.7)]
    assert tfa.select_alpha(flat) == 0.5
    assert tfa.select_alpha(flat, (0.4, 0.6)) == 0.4


def test_loco_refuses_a_single_group():
    with pytest.raises(ValueError):
        tfa.loco([rec("A", lambda a: a)])


def test_summary_shape_is_metrics_only_and_json_serialisable():
    records = [rec("locomo:0", lambda a: a), rec("locomo:1", lambda a: 1 - a),
               rec("locomo:1", lambda a: a)]
    for r in records:  # anything text-shaped riding on a record must not reach the output
        r["question_text"] = "SENTINEL-QUESTION-TEXT"
    s = tfa.summarize(records)
    assert s["n"] == 3 and s["groups"] == 2
    assert list(s["arms"]) == ["kw", "sem", "rrf"] + [tfa.alpha_arm(a) for a in tfa.ALPHAS]
    for arm in s["arms"].values():
        assert set(arm) == set(tfa.METRIC_NAMES)
    assert set(s["loco"]) == {"folds", "n", "held_out", "paired_vs_rrf", "selected_alpha_counts",
                              "loco_selected_alpha", "in_sample_best_alpha_optimistic",
                              "rrf_same_questions"}
    text = json.dumps(s)
    assert "SENTINEL" not in text


def test_semantic_dead_refuses(capsys):
    records = [rec(f"g{i}", lambda a: a) for i in range(10)]
    records[0]["sem_empty"] = True
    tfa.check_semantic_alive(records, "x")  # 1/10 is within the 10% allowance
    records[1]["sem_empty"] = True
    with pytest.raises(SystemExit):
        tfa.check_semantic_alive(records, "x")


def test_record_scores_each_arm_on_its_units():
    keys = {"kw": ["s1:t0"], "sem": ["s2:t0"], "rrf": ["s2:t3", "s1:t0"]}
    keys.update({tfa.alpha_arm(a): ["s1:t0"] for a in tfa.ALPHAS})
    r = tfa.record("lme:q1", keys, tfa.lme_units, {"2"})
    assert r["arms"]["sem"]["hit@1"] == 1.0
    assert r["arms"]["kw"]["hit@1"] == 0.0
    assert r["arms"]["rrf"]["hit@1"] == 1.0
    assert r["sem_empty"] is False


def test_plain_env_pins_rrf_and_validates_alpha():
    assert emb.plain_env() == {"HU_HYBRID_FUSION": "rrf"}
    assert emb.plain_env("score", 0.3) == {"HU_HYBRID_FUSION": "score", "HU_HYBRID_FUSION_ALPHA": "0.30"}
    for bad in (None, -0.1, 1.5):
        with pytest.raises(ValueError):
            emb.plain_env("score", bad)


def test_locomo_convs_sample_is_deterministic_and_keeps_run_order(tmp_path):
    convs = []
    for ci in range(3):
        qa = [{"question": f"q{ci}{k}", "evidence": [f"D1:{k}"]} for k in range(5)]
        qa.append({"question": "no evidence", "evidence": []})
        convs.append({"conversation": {"session_1": [{"dia_id": "D1:0", "speaker": "a", "text": "hi"}]},
                      "qa": qa})
    (tmp_path / "locomo10.json").write_text(json.dumps(convs))
    a = emb.locomo_convs(6, 3, data_dir=str(tmp_path))
    b = emb.locomo_convs(6, 3, data_dir=str(tmp_path))
    assert [ci for ci, _, _ in a] == [0, 1, 2]
    assert [[q["question"] for q in qa] for _, _, qa in a] == [[q["question"] for q in qa] for _, _, qa in b]
    assert all(len(qa) == 2 and all(q["evidence"] for q in qa) for _, _, qa in a)
    assert a[0][1] == [("D1:0", "session_1", "a: hi")]


def test_paired_vs_rrf_counts_wins_ties_losses_per_question():
    # alpha arms score 1.0 on group A and 0.0 on group B; rrf is 0.5 everywhere,
    # except one A question where rrf also scores 1.0 (a tie).
    a1, a2, b1 = rec("A", lambda a: 1.0), rec("A", lambda a: 1.0), rec("B", lambda a: 0.0)
    a2["arms"]["rrf"] = {m: 1.0 for m in tfa.METRIC_NAMES}
    out = tfa.loco([a1, a2, b1])
    assert out["paired_vs_rrf"]["hit@1"] == {"win": 1, "tie": 1, "loss": 1}
    assert sum(out["paired_vs_rrf"]["ndcg@5"].values()) == out["n"] == 3


def _fake_locomo(monkeypatch, fail_questions):
    convs = [(0, [("D1:0", "s", "x")], [{"question": f"q{i}", "evidence": ["D1:0"]}
                                         for i in range(10)])]
    monkeypatch.setattr(tfa.emb, "locomo_convs", lambda limit, seed: convs)
    monkeypatch.setattr(tfa.emb, "build_db", lambda binp, dbp, rows: len(rows))
    calls = []

    def fake_run_search(binp, dbp, args, env_extra=None):
        q = args[-1]
        hybrid = "--hybrid" in args
        calls.append((q, hybrid, (env_extra or {}).get("HU_HYBRID_FUSION")))
        # the failing question's embedder dies on its alpha=0.3 hybrid call only
        failed = hybrid and q in fail_questions and \
            (env_extra or {}).get("HU_HYBRID_FUSION_ALPHA") == "0.30"
        return (["D1:0"], not failed)
    monkeypatch.setattr(tfa, "run_search", fake_run_search)
    return calls


def test_embed_error_question_is_excluded_from_every_arm_and_counted(monkeypatch):
    _fake_locomo(monkeypatch, {"q3"})
    records, excluded = tfa.run_locomo("bin", 10, 3, "/tmp/unused")
    assert excluded == 1
    assert len(records) == 9  # q3 is in no arm -- not scored as zeros in alpha=0.3
    for r in records:
        assert r["arms"][tfa.alpha_arm(0.3)]["hit@1"] == 1.0
        assert r["arms"]["rrf"]["hit@1"] == 1.0


def test_embed_errors_over_ten_percent_refuse_with_exit_2(monkeypatch):
    _fake_locomo(monkeypatch, {"q1", "q2"})  # 2/10 = 20%
    with pytest.raises(SystemExit) as e:
        tfa.run_locomo("bin", 10, 3, "/tmp/unused")
    assert e.value.code == 2


def test_run_search_flags_a_keyword_only_fallback_as_a_dense_failure(monkeypatch):
    class P:
        def __init__(self, rc, out, err):
            self.returncode, self.stdout, self.stderr = rc, out, err
    results = iter([
        P(0, b"  [1] D1:3 (0.500): x\n", b""),
        P(0, b"  [1] D1:3 (0.500): x\n",
          b"search --hybrid: semantic index unavailable, using keyword only\n"),
        P(1, b"", b"search --hybrid: network error\n"),
    ])
    monkeypatch.setattr(tfa.subprocess, "run", lambda *a, **k: next(results))
    assert tfa.run_search("bin", "db", ["search", "q"]) == (["D1:3"], True)
    assert tfa.run_search("bin", "db", ["search", "--hybrid", "--plain", "q"])[1] is False
    assert tfa.run_search("bin", "db", ["search", "--hybrid", "--plain", "q"])[1] is False
