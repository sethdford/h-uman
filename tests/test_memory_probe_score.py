"""Hermetic tests for scripts/datasets/memory_probe_score.py.

The scorer reads replay outputs per arm and reports memory-probe recall by
question category. Matching is LLM-free (normalized exact + token-F1 +
gold-token recall + date match); the optional judge must be loopback-only and
is exercised here through an injected fake — no network.
"""
import json
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts" / "datasets"))

import memory_probe_score as mps  # noqa: E402


def _probe(pid, category, gold, adv=None, in_window=True):
    return {
        "id": pid,
        "contact_id": "x@bench.invalid",
        "inbound_bubbles": ["q?"],
        "ts": "2023-01-01 00:00:00",
        "history": [],
        "probe": {"dataset": "locomo", "category": category, "gold_answers": gold,
                  "adversarial_answer": adv, "question_original": "q?",
                  "evidence_in_window": in_window},
    }


def _out(pid, *bubbles, action="text"):
    return {"id": pid, "action": action, "bubbles": list(bubbles)}


# ── matching primitives ──────────────────────────────────────────────────


def test_normalize_matches_official_shape_plus_number_words():
    assert mps.normalize("The Kiln Room, obviously!") == "kiln room obviously"
    assert mps.normalize("two cats") == "2 cats"


def test_single_hop_hit_and_miss():
    s = mps.score_probe(_probe("p", "single_hop", ["Biscuit"]), "lol his name is biscuit")
    assert s["hit"] is True and s["contains"] is True
    s = mps.score_probe(_probe("p", "single_hop", ["Biscuit"]), "no idea tbh")
    assert s["hit"] is False and s["f1"] == 0.0


def test_token_recall_threshold_on_multi_token_gold():
    p = _probe("p", "single_hop", ["red pickup truck"])
    assert mps.score_probe(p, "it's a red pickup")["hit"] is True  # 2/3 tokens
    assert mps.score_probe(p, "a truck")["hit"] is False  # 1/3 tokens


def test_temporal_dates_match_across_formats():
    p = _probe("p", "temporal", ["7 May 2023"])
    assert mps.score_probe(p, "it was may 7th i think")["hit"] is True
    assert mps.score_probe(p, "5/7/2023")["hit"] is True
    assert mps.score_probe(p, "may 8th")["hit"] is False
    fused = _probe("p", "temporal", ["The weekend after 3June, 2022."])  # upstream typo
    assert mps.score_probe(fused, "it was june 3rd")["date_match"] is True
    y = _probe("p", "temporal", ["2023"])
    assert mps.score_probe(y, "back in 2023")["hit"] is True
    assert mps.score_probe(y, "back in 2022")["hit"] is False


def test_comma_without_space_still_separates_words():
    p = _probe("p", "multi_hop", ["Gamecube, PC,Playstation."])
    assert mps.score_probe(p, "gamecube, pc,playstation")["hit"] is True


def test_adversarial_answer_of_only_stopwords_cannot_be_asserted():
    p = _probe("p", "adversarial", [], adv='"That"')
    s = mps.score_probe(p, "i don't remember you mentioning that")
    assert s["asserted_adversarial"] is False and s["hit"] is True


def test_multi_hop_needs_every_part():
    p = _probe("p", "multi_hop", ["dog, cat"])
    assert mps.score_probe(p, "you have a dog and i want a cat")["hit"] is True
    s = mps.score_probe(p, "a dog")
    assert s["hit"] is False and s["contains_frac"] == 0.5


def test_adversarial_rewards_abstaining_and_punishes_asserting():
    p = _probe("p", "adversarial", [], adv="literacy")
    assert mps.score_probe(p, "wait you didn't do the 10k, that was me")["hit"] is True
    assert mps.score_probe(p, "idk honestly")["hit"] is True
    s = mps.score_probe(p, "it was for literacy right")
    assert s["hit"] is False and s["asserted_adversarial"] is True
    assert mps.score_probe(p, "it was great")["hit"] is False  # neither abstains nor corrects
    hedged = mps.score_probe(p, "idk, maybe literacy?")  # hedges, then asserts the trap
    assert hedged["abstained"] is True and hedged["hit"] is False


# ── per-arm report ───────────────────────────────────────────────────────


PROBES = [
    _probe("a", "single_hop", ["Biscuit"], in_window=False),
    _probe("b", "temporal", ["7 May 2023"]),
    _probe("c", "adversarial", [], adv="literacy"),
    _probe("d", "single_hop", ["Lisbon"]),
]


def test_report_per_arm_per_category_and_window():
    arms = {
        "base": [_out("a", "no clue"), _out("b", "may 7"), _out("c", "literacy!"),
                 _out("d", "lisbon")],
        "cand": [_out("a", "biscuit"), _out("b", "may 7"), _out("c", "never did that"),
                 _out("d", "lisbon")],
    }
    rep = mps.build_report(PROBES, arms)
    base, cand = rep["arms"]["base"], rep["arms"]["cand"]
    assert base["overall"]["hit_rate"] == 0.5 and cand["overall"]["hit_rate"] == 1.0
    assert base["by_category"]["single_hop"] == pytest.approx(
        {"n": 2, "hits": 1, "hit_rate": 0.5, "contains_rate": 0.5, "mean_f1": 0.5,
         "mean_recall": 0.5}, abs=1e-9)
    assert cand["by_category"]["adversarial"]["hit_rate"] == 1.0
    assert base["by_window"]["beyond_window"] == pytest.approx(
        {"n": 1, "hits": 0, "hit_rate": 0.0}, abs=1e-9)
    assert cand["by_window"]["beyond_window"]["hit_rate"] == 1.0


def test_missing_and_non_text_outputs_count_as_misses_not_drops():
    arms = {"x": [_out("a", "biscuit"), _out("b", action="silence")]}
    rep = mps.build_report(PROBES, arms)["arms"]["x"]
    assert rep["overall"]["n"] == 4  # every probe, not just the answered ones
    assert rep["overall"]["hits"] == 1
    assert rep["missing"] == 2 and rep["no_reply"] == 1


def test_replay_output_accepts_text_field_and_line_index():
    probes = PROBES[:2]
    arms = {"x": [{"line": 1, "text": "May 7"}, {"line": 0, "reply": "biscuit"}]}
    rep = mps.build_report(probes, arms)["arms"]["x"]
    assert rep["overall"]["hits"] == 2 and rep["missing"] == 0


def test_empty_replies_score_zero_outside_adversarial():
    arms = {"empty": [_out(p["id"], "") for p in PROBES]}
    rep = mps.build_report(PROBES, arms)["arms"]["empty"]
    for cat in ("single_hop", "temporal"):
        assert rep["by_category"][cat]["hit_rate"] == 0.0


# ── judge: loopback only, optional ───────────────────────────────────────


@pytest.mark.parametrize("url", [
    "http://127.0.0.1:8080/v1/chat/completions",
    "http://localhost:9000/v1/chat/completions",
    "http://[::1]:8000/v1/chat/completions",
])
def test_judge_url_loopback_accepted(url):
    assert mps.check_loopback_url(url) == url


@pytest.mark.parametrize("url", [
    "https://api.example.com/v1/chat/completions",
    "http://localhost.evil.com/v1",
    "http://127.0.0.1.nip.io/v1",
    "http://user@127.0.0.1/v1",
    "",
])
def test_judge_url_non_loopback_refused(url):
    with pytest.raises(ValueError):
        mps.check_loopback_url(url)


def test_judge_column_is_added_without_replacing_string_metrics():
    calls = []

    def fake_judge(question, gold, reply):
        calls.append((question, gold, reply))
        return "biscuit" in reply

    arms = {"x": [_out("a", "biscuit"), _out("d", "portugal")]}
    rep = mps.build_report(PROBES, arms, judge=fake_judge)["arms"]["x"]
    assert len(calls) == 2  # missing outputs are never sent to the judge
    assert rep["overall"]["judge_rate"] == 0.25
    assert rep["overall"]["hit_rate"] == 0.25  # string metrics unchanged


def test_cli_end_to_end(tmp_path):
    pf = tmp_path / "probes.jsonl"
    pf.write_text("".join(json.dumps(p) + "\n" for p in PROBES))
    af = tmp_path / "base.jsonl"
    af.write_text(json.dumps(_out("d", "lisbon")) + "\n")
    jf = tmp_path / "report.json"
    r = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "datasets" / "memory_probe_score.py"),
         "--probes", str(pf), "--arm", f"base={af}", "--json", str(jf)],
        capture_output=True, text=True, check=True)
    assert "single_hop" in r.stdout and "base" in r.stdout
    rep = json.loads(jf.read_text())
    assert rep["arms"]["base"]["overall"]["hits"] == 1
    assert rep["arms"]["base"]["missing"] == 3


def test_cli_refuses_remote_judge(tmp_path):
    pf = tmp_path / "probes.jsonl"
    pf.write_text(json.dumps(PROBES[0]) + "\n")
    af = tmp_path / "a.jsonl"
    af.write_text(json.dumps(_out("a", "biscuit")) + "\n")
    r = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "datasets" / "memory_probe_score.py"),
         "--probes", str(pf), "--arm", f"a={af}",
         "--judge-url", "https://api.example.com/v1/chat/completions"],
        capture_output=True, text=True)
    assert r.returncode != 0 and "loopback" in r.stderr
