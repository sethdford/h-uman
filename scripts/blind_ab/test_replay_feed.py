"""Hermetic tests for replay_feed.py: the fragment and deflection heuristics,
the per-arm stats against Seth's real replies, the blind A/B triples, and the
refusals (an arm with errors, a short arm). Temp dirs only.
"""
import json
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import replay_feed as rf  # noqa: E402


# ── truncated fragment ────────────────────────────────────────────────

@pytest.mark.parametrize("reply", [
    "yeah i was just",
    "ok gonna grab the",
    "we should go to",
    "i think we can and",
    "gonna lock",
    "honestly it was a",
    "yeah but,",
    "the plan is:",
])
def test_fragment_positive(reply):
    assert rf.is_fragment(reply)


@pytest.mark.parametrize("reply", [
    "yeah i was just leaving",
    "ok gonna grab the car",
    "lol same",
    "sounds good.",
    "who's coming to?",          # terminal punctuation wins
    "see you there 😂",          # emoji ending
    "i'm in",                    # stranded preposition: complete in texting
    "who are you with",
    "yeah it is",                # bare auxiliary: complete
    "what are you up to",        # "to" after "up" completes the clause
    "i have to",
    "",
])
def test_fragment_negative(reply):
    assert not rf.is_fragment(reply)


# ── deflection ────────────────────────────────────────────────────────

def test_deflection_phrases_are_word_bounded():
    assert rf.is_deflection("you coming?", "idk yet")
    assert rf.is_deflection("what time?", "not sure, we'll see")
    assert rf.is_deflection("dinner?", "let me check")
    assert not rf.is_deflection("you coming?", "kidding lol")      # "idk" inside a word
    assert not rf.is_deflection("you coming?", "yeah be there at 7")


def test_question_answered_with_unrelated_question_is_deflection():
    assert rf.is_deflection("how was the interview?", "what about you?")
    assert not rf.is_deflection("how was the interview?", "the interview went fine, you?")
    assert not rf.is_deflection("ok cool", "you around later?")   # they asked nothing


# ── stats + triples end to end ────────────────────────────────────────

TURNS = [
    {"id": "t1", "inbound_bubbles": ["you coming tonight?"], "seth_action": "text",
     "seth_reply_bubbles": ["yeah", "what time"]},
    {"id": "t2", "inbound_bubbles": ["lol"], "seth_action": "tapback", "seth_reply_bubbles": []},
    {"id": "t3", "inbound_bubbles": ["how was it?"], "seth_action": "text",
     "seth_reply_bubbles": ["good"]},
]


def row(i, action, bubbles, fp):
    return {"id": i, "arm": "x", "action": action, "bubbles": bubbles,
            "bubble_count": len(bubbles), "reply_fp": fp}


def make_run(tmp_path, arms):
    run = tmp_path / "runs" / "r1"
    (run / "out").mkdir(parents=True)
    with open(run / "turns.jsonl", "w") as f:
        for t in TURNS:
            f.write(json.dumps(t) + "\n")
    for name, rows in arms.items():
        with open(run / "out" / f"{name}.jsonl", "w") as f:
            for r in rows:
                f.write(json.dumps(r) + "\n")
    return run


def test_feed_writes_triples_and_stats(tmp_path, capsys):
    run = make_run(tmp_path, {
        "off": [row("t1", "text", ["yeah i was just"], "aa"), row("t2", "tapback", [], None),
                row("t3", "text", ["idk"], "bb")],
        "on": [row("t1", "text", ["yeah what time?"], "cc"), row("t2", "text", ["haha"], "dd"),
               row("t3", "text", ["it was great"], "ee")],
    })
    assert rf.main(["--name", "r1", "--run-root", str(tmp_path / "runs")]) == 0
    stats = json.loads((run / "feed" / "stats.json").read_text())
    assert stats["turns"] == 3
    seth = stats["seth"]
    assert seth["tapback_share"] == pytest.approx(1 / 3)
    assert seth["fragment_rate"] == 0.0
    off, on = stats["arms"]["off"], stats["arms"]["on"]
    assert off["tapback_share"] == pytest.approx(1 / 3) and on["tapback_share"] == 0.0
    assert off["fragment_rate"] == pytest.approx(0.5)
    assert off["deflection_rate"] == pytest.approx(0.5)
    assert on["question_rate"] == pytest.approx(1 / 3)
    assert on["same_request_as_off"] == 0 and on["compared_with_off"] == 2
    triples = json.loads((run / "feed" / "off" / "triples.json").read_text())
    assert [t["id"] for t in triples] == ["t1", "t3"]          # tapback turn excluded
    assert triples[0]["seth_reply"] == "yeah\nwhat time"
    out = capsys.readouterr()
    assert "what time" not in out.out and "you coming" not in out.out


def test_feed_warns_when_an_arm_never_changed_the_request(tmp_path, capsys):
    make_run(tmp_path, {
        "off": [row(i, "text", ["ok"], f"fp{i}") for i in ("t1", "t2", "t3")],
        "on": [row(i, "text", ["ok"], f"fp{i}") for i in ("t1", "t2", "t3")],
    })
    assert rf.main(["--name", "r1", "--run-root", str(tmp_path / "runs")]) == 0
    assert "never reached the prompt" in capsys.readouterr().err


def test_feed_refuses_an_arm_with_errors(tmp_path, capsys):
    run = make_run(tmp_path, {
        "off": [row("t1", "text", ["ok"], "a"), row("t2", "error", [], None),
                row("t3", "text", ["ok"], "b")],
    })
    assert rf.main(["--name", "r1", "--run-root", str(tmp_path / "runs")]) == 1
    assert not (run / "feed").exists()


def test_summarize_empty_is_absent_not_zero():
    assert rf.summarize([]) == {"n": 0}
