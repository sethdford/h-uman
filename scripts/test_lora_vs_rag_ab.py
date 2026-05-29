#!/usr/bin/env python3
"""Tests for lora_vs_rag_ab.py (W7-3)."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import lora_vs_rag_ab as ab

CORPUS = [
    "yeah lunch sounds good, the usual spot?",
    "nah cant make the meeting, got a conflict",
    "lol fair point, lemme think on it",
    "gonna head out soon, catch you later",
    "idk maybe tomorrow works better tbh",
]


def test_retrieve_similar_ranks_by_overlap():
    got = ab.retrieve_similar("are we still on for lunch?", CORPUS, k=1)
    assert got == ["yeah lunch sounds good, the usual spot?"], got


def test_retrieve_similar_k_bounds():
    got = ab.retrieve_similar("meeting tomorrow lunch", CORPUS, k=3)
    assert len(got) == 3


def test_build_rag_fewshot_includes_examples_and_prompt():
    p = ab.build_rag_fewshot("you free?", ["yeah", "nah busy"])
    assert "yeah" in p and "nah busy" in p and "you free?" in p


def test_compare_paths_rag_wins_when_grounded_in_real_voice():
    # LoRA path = formal/off-voice; RAG path = echoes real messages → should win.
    lora = ["I am available at your convenience." for _ in CORPUS]
    rag = list(CORPUS)
    res = ab.compare_paths(CORPUS, lora, rag)
    assert res["overall_winner"] == "rag", res
    assert res["rag_aggregate"] > res["lora_aggregate"]
    assert res["axis_wins"]["rag"] >= res["axis_wins"]["lora"]


def test_compare_paths_structure():
    res = ab.compare_paths(CORPUS, CORPUS, CORPUS)
    assert set(res) >= {"overall_winner", "per_axis", "recommendation", "axis_wins"}
    # identical inputs → every axis a tie
    assert all(v["winner"] == "tie" for v in res["per_axis"].values()), res["per_axis"]


def test_run_ab_dry_run_produces_comparison():
    prompts = ["lunch today?", "you coming to the meeting?"]
    res = ab.run_ab(CORPUS, prompts, dry_run=True, mlx_url="x", k=2)
    assert res["overall_winner"] in ("lora", "rag", "tie")
    assert "recommendation" in res


def main():
    tests = [
        test_retrieve_similar_ranks_by_overlap,
        test_retrieve_similar_k_bounds,
        test_build_rag_fewshot_includes_examples_and_prompt,
        test_compare_paths_rag_wins_when_grounded_in_real_voice,
        test_compare_paths_structure,
        test_run_ab_dry_run_produces_comparison,
    ]
    print("Testing lora_vs_rag_ab.py")
    print("=" * 60)
    p = f = 0
    for t in tests:
        try:
            t()
            print(f"✓ {t.__name__}")
            p += 1
        except AssertionError as e:
            print(f"✗ {t.__name__}: {e}")
            f += 1
        except Exception as e:  # noqa: BLE001
            print(f"✗ {t.__name__}: {type(e).__name__}: {e}")
            f += 1
    print("=" * 60)
    print(f"Results: {p} passed, {f} failed")
    return 0 if f == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
