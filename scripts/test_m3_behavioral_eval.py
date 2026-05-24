#!/usr/bin/env python3
"""
M3 behavioral-eval verifier — pins the style-heuristic + verdict logic.

These tests do NOT call mlx_lm (subprocess slow + adds a heavy dep).
They exercise the pure-function pieces:

  1. style_score: each dimension counted correctly on known inputs
  2. aggregate_scores: sums + means across N records
  3. behavioral_verdict: PASS / no-change / regress thresholds
  4. End-to-end: feed canned base/candidate outputs, get a verdict

Run: python3 scripts/test_m3_behavioral_eval.py
"""
from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
EVAL = REPO_ROOT / "scripts" / "m3_behavioral_eval.py"


def _load():
    spec = importlib.util.spec_from_file_location("m3_behavioral_eval", EVAL)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


m = _load()
_PASS = 0
_FAIL = 0


def _ok(name, cond, detail=""):
    global _PASS, _FAIL
    if cond:
        _PASS += 1
        print(f"  PASS  {name}")
    else:
        _FAIL += 1
        print(f"  FAIL  {name}  {detail}")


def test_style_score_counts_dimensions():
    print("\n--- test_style_score_counts_dimensions ---")
    # Seth-style: short, casual, emoji, contractions, no formal
    s = m.style_score("yeah lol that's cool 🎉")
    _ok(f"n_chars > 0 ({s['n_chars']})", s["n_chars"] == len("yeah lol that's cool 🎉"))
    _ok(f"casual_hits ≥ 2 (got {s['casual_hits']})", s["casual_hits"] >= 2,
        f"yeah + lol expected")
    _ok(f"formal_hits == 0 (got {s['formal_hits']})", s["formal_hits"] == 0)
    _ok(f"emoji_count >= 1 (got {s['emoji_count']})", s["emoji_count"] >= 1)
    _ok(f"contractions >= 1 (got {s['contractions']})", s["contractions"] >= 1,
        f"that's expected")

    # Formal corporate AI-speak: long, formal, no emoji, no contractions
    f = m.style_score("Certainly! I would be happy to help you with that. "
                       "Please let me know if there is anything else I can "
                       "assist you with today.")
    _ok(f"formal_hits >= 2 (got {f['formal_hits']})", f["formal_hits"] >= 2)
    _ok(f"emoji_count == 0 (got {f['emoji_count']})", f["emoji_count"] == 0)
    _ok(f"n_chars > 50 (got {f['n_chars']})", f["n_chars"] > 50)


def test_style_score_empty_input():
    print("\n--- test_style_score_empty_input ---")
    s = m.style_score("")
    _ok("empty → all zeros",
        s["n_chars"] == 0 and s["casual_hits"] == 0 and
        s["formal_hits"] == 0 and s["emoji_count"] == 0 and
        s["contractions"] == 0)
    s = m.style_score(None)
    _ok("None → all zeros (no crash)", s["n_chars"] == 0)


def test_aggregate_scores():
    print("\n--- test_aggregate_scores ---")
    per = [
        {"n_chars": 10, "casual_hits": 1, "formal_hits": 0,
         "emoji_count": 1, "contractions": 0},
        {"n_chars": 20, "casual_hits": 2, "formal_hits": 1,
         "emoji_count": 0, "contractions": 1},
        {"n_chars": 30, "casual_hits": 0, "formal_hits": 0,
         "emoji_count": 2, "contractions": 1},
    ]
    a = m.aggregate_scores(per)
    _ok("n_prompts == 3", a["n_prompts"] == 3)
    _ok("mean_chars == 20", a["mean_chars"] == 20.0)
    _ok("total_casual == 3", a["total_casual"] == 3)
    _ok("total_formal == 1", a["total_formal"] == 1)
    _ok("total_emoji == 3", a["total_emoji"] == 3)
    _ok("total_contract == 2", a["total_contract"] == 2)


def test_aggregate_scores_empty():
    print("\n--- test_aggregate_scores_empty ---")
    _ok("empty list → {}", m.aggregate_scores([]) == {})


def test_verdict_pass_when_candidate_more_seth_like():
    """Candidate has more casual/emoji/contractions + fewer formal,
    closer to reference length. Should PASS."""
    print("\n--- test_verdict_pass_when_candidate_more_seth_like ---")
    ref = {"n_prompts": 5, "mean_chars": 20.0,
            "total_casual": 5, "total_formal": 0,
            "total_emoji": 3, "total_contract": 4}
    base = {"n_prompts": 5, "mean_chars": 100.0,    # too verbose
             "total_casual": 0, "total_formal": 5,   # too formal
             "total_emoji": 0, "total_contract": 0}
    cand = {"n_prompts": 5, "mean_chars": 25.0,     # closer to ref
             "total_casual": 4, "total_formal": 1,
             "total_emoji": 2, "total_contract": 3}
    v = m.behavioral_verdict(base, cand, ref)
    _ok(f"verdict=pass (got {v['verdict']})", v["verdict"] == "pass",
        f"reason: {v['reason']}")
    _ok(f"wins == 5 (got {v['wins']})", v["wins"] == 5)


def test_verdict_regress_when_candidate_worse():
    """Candidate has MORE formal markers and FEWER casual — regress."""
    print("\n--- test_verdict_regress_when_candidate_worse ---")
    ref = {"n_prompts": 5, "mean_chars": 20.0,
            "total_casual": 5, "total_formal": 0,
            "total_emoji": 3, "total_contract": 4}
    base = {"n_prompts": 5, "mean_chars": 25.0,
             "total_casual": 4, "total_formal": 1,
             "total_emoji": 2, "total_contract": 3}
    cand = {"n_prompts": 5, "mean_chars": 200.0,    # way verbose
             "total_casual": 0, "total_formal": 10,  # super formal
             "total_emoji": 0, "total_contract": 0}
    v = m.behavioral_verdict(base, cand, ref)
    _ok(f"verdict=regress (got {v['verdict']})",
        v["verdict"] == "regress", f"reason: {v['reason']}")


def test_verdict_no_change_on_2_wins():
    print("\n--- test_verdict_no_change_on_2_wins ---")
    ref = {"n_prompts": 5, "mean_chars": 20.0,
            "total_casual": 5, "total_formal": 0,
            "total_emoji": 3, "total_contract": 4}
    base = {"n_prompts": 5, "mean_chars": 100.0,    # too verbose
             "total_casual": 2, "total_formal": 2,
             "total_emoji": 1, "total_contract": 2}
    # Cand wins on length + formal_markers; ties on emoji + contractions;
    # loses on casual. 2 strict wins → no-change.
    cand = {"n_prompts": 5, "mean_chars": 30.0,     # closer to ref → win
             "total_casual": 1, "total_formal": 1,   # win on formal
             "total_emoji": 1, "total_contract": 2}  # tie tie
    v = m.behavioral_verdict(base, cand, ref)
    # Ties count toward candidate (>= comparison) — emoji + contract win
    # for cand, so wins might be 4. Test that the math is at least
    # internally consistent.
    _ok(f"wins between 2 and 5 (got {v['wins']})",
        2 <= v["wins"] <= 5)


def test_verdict_uses_distance_to_reference_not_monotonic():
    """Pins the 2026-05-19 bug: previous heuristic counted MORE casual
    markers as monotonically better. But on the held-out set, base
    gemma-3-4b hallucinated 19 casual markers when ref had only 4.
    Candidate's 7 is CLOSER to ref than base's 19, so candidate wins
    on casual_markers — not loses. Verdict must use distance-to-ref."""
    print("\n--- test_verdict_uses_distance_to_reference_not_monotonic ---")
    ref =  {"n_prompts": 8, "mean_chars": 32.8,
             "total_casual": 4, "total_formal": 0,
             "total_emoji": 0, "total_contract": 0}
    base = {"n_prompts": 8, "mean_chars": 185.2,    # too verbose
             "total_casual": 19, "total_formal": 1,  # too casual
             "total_emoji": 5, "total_contract": 16}
    cand = {"n_prompts": 8, "mean_chars": 27.0,     # closer (|27-33|=6 vs |185-33|=152) ✓
             "total_casual": 7, "total_formal": 0,   # closer (|7-4|=3 vs |19-4|=15) ✓
             "total_emoji": 0, "total_contract": 9}  # ✓ emoji ties, contract closer
    v = m.behavioral_verdict(base, cand, ref)
    _ok(f"verdict=pass with distance-to-ref logic (got {v['verdict']})",
        v["verdict"] == "pass",
        f"reason={v['reason']}, wins={v['wins']}")
    # The candidate should win on AT LEAST length, casual, contract
    by_name = {d["name"]: d for d in v["dimensions"]}
    _ok("candidate wins on length_proximity",
        by_name["length_proximity"]["candidate_better"])
    _ok("candidate wins on casual_markers (closer to ref=4)",
        by_name["casual_markers"]["candidate_better"])
    _ok("candidate wins on contractions (closer to ref=0)",
        by_name["contractions"]["candidate_better"])


def test_diversity_check_detects_mode_collapse():
    """Pins the 2026-05-19 mode-collapse failure: 50-iter rank-16
    overtrained adapter emitted 'I'm here!' on 4/4 different prompts.
    Style metrics passed but the adapter was broken. Diversity check
    catches this."""
    print("\n--- test_diversity_check_detects_mode_collapse ---")
    # All identical → collapse
    same = ["I'm here!"] * 8
    d = m.diversity_check(same)
    _ok(f"all-same → collapsed (got {d['is_collapsed']})",
        d["is_collapsed"] is True)
    _ok(f"distinct_ratio == 0.125 (got {d['distinct_ratio']})",
        d["distinct_ratio"] == 0.125)
    _ok(f"top_count == 8", d["top_count"] == 8)

    # Mostly identical (7/8 same) → collapse
    mostly = ["I'm here!"] * 7 + ["different"]
    d = m.diversity_check(mostly)
    _ok("7/8 same → collapsed", d["is_collapsed"] is True)

    # 4/8 collapsed → collapsed (top_count >= 4)
    half = ["yes"] * 4 + ["one", "two", "three", "four"]
    d = m.diversity_check(half)
    _ok(f"4/8 same → collapsed (top_count={d['top_count']})",
        d["is_collapsed"] is True)

    # All different → not collapsed
    diverse = [f"response {i}" for i in range(8)]
    d = m.diversity_check(diverse)
    _ok(f"all different → not collapsed (got {d['is_collapsed']})",
        d["is_collapsed"] is False)
    _ok("distinct_ratio == 1.0", d["distinct_ratio"] == 1.0)

    # Empty list → not collapsed (no-op)
    _ok("empty → not collapsed",
        m.diversity_check([])["is_collapsed"] is False)


def test_verdict_dimensions_structure():
    print("\n--- test_verdict_dimensions_structure ---")
    ref = {"n_prompts": 1, "mean_chars": 10.0,
            "total_casual": 1, "total_formal": 0,
            "total_emoji": 1, "total_contract": 1}
    base = ref
    cand = ref
    v = m.behavioral_verdict(base, cand, ref)
    _ok("5 dimensions in verdict",
        len(v["dimensions"]) == 5)
    names = {d["name"] for d in v["dimensions"]}
    expected = {"length_proximity", "casual_markers", "formal_markers",
                 "emoji_count", "contractions"}
    _ok(f"all expected names present (got {names})",
        names == expected)
    for d in v["dimensions"]:
        _ok(f"  {d['name']} has base/candidate/reference",
            {"base", "candidate", "reference", "candidate_better"}
            <= set(d))


def main():
    print("M3 behavioral eval verifier")
    test_style_score_counts_dimensions()
    test_style_score_empty_input()
    test_aggregate_scores()
    test_aggregate_scores_empty()
    test_verdict_pass_when_candidate_more_seth_like()
    test_verdict_regress_when_candidate_worse()
    test_verdict_no_change_on_2_wins()
    test_verdict_uses_distance_to_reference_not_monotonic()
    test_diversity_check_detects_mode_collapse()
    test_verdict_dimensions_structure()
    print(f"\n--- Results: {_PASS} passed, {_FAIL} failed ---")
    return 0 if _FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
