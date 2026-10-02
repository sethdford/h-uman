"""Hermetic tests for the memory-benchmark converters in scripts/datasets/.

Each converter turns one public dataset (LoCoMo, MSC, SOC-2508) into the
replay-harness turn JSONL ({contact_id, inbound_bubbles[], ts, history[]}).
The fixtures under tests/fixtures/memory_benchmarks/ are tiny SYNTHETIC
samples written in each upstream format; no dataset file, no network, no
~/.human.
"""
import json
import subprocess
import sys
from datetime import datetime, timedelta
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts" / "datasets"))

import locomo_to_replay as lr  # noqa: E402
import msc_to_replay as mr  # noqa: E402
import replay_common as rc  # noqa: E402
import soc2508_to_replay as sr  # noqa: E402

FX = ROOT / "tests" / "fixtures" / "memory_benchmarks"
TS = "%Y-%m-%d %H:%M:%S"


def _locomo(**kw):
    return lr.convert(json.loads((FX / "locomo_tiny.json").read_text()), **kw)


def _msc(**kw):
    eps = [json.loads(l) for l in (FX / "msc_session5_tiny.txt").read_text().splitlines() if l]
    return mr.convert(eps, **kw)


def _soc(**kw):
    chats = [json.loads(l) for l in (FX / "soc2508_tiny.jsonl").read_text().splitlines() if l]
    return sr.convert(chats, **kw)


def _probe(out, qsub):
    hits = [p for p in out["probes"] if qsub in p["probe"]["question_original"]]
    assert len(hits) == 1, qsub
    return hits[0]


# ── shared helpers ───────────────────────────────────────────────────────


def test_contact_id_is_reserved_domain_never_a_real_handle():
    cid = rc.bench_contact_id("locomo", "conv-26")
    assert cid.endswith("@bench.invalid")
    assert cid != rc.bench_contact_id("locomo", "conv-30")


def test_casual_question_maps_names_to_pronouns():
    q = rc.casual_question("What is the name of Rory's dog?", "Dana", "Rory", key="k1")
    assert "your dog" in q and "Rory" not in q
    q = rc.casual_question("When did Dana start pottery classes?", "Dana", "Rory", key="k1")
    assert "did I start pottery classes" in q and "Dana" not in q
    q = rc.casual_question("Is Rory a runner?", "Dana", "Rory", key="k1")
    assert "are you a runner" in q
    q = rc.casual_question("What pets do Rory and Dana have?", "Dana", "Rory", key="k1")
    assert "do we have" in q


def test_casual_question_is_deterministic_and_varies_by_key():
    a = rc.casual_question("Where did Rory travel?", "Dana", "Rory", key="a")
    assert a == rc.casual_question("Where did Rory travel?", "Dana", "Rory", key="a")
    framings = {rc.casual_question("Where did Rory travel?", "Dana", "Rory", key=str(i)) for i in range(40)}
    assert len(framings) > 2  # the texting-style prefix rotates


# ── LoCoMo ───────────────────────────────────────────────────────────────


def test_locomo_speakers_map_to_contact_and_seth():
    out = _locomo()
    hist = out["probes"][0]["history"]
    assert [h["from_me"] for h in hist] == [False, True, False, False, True, True, False, True]
    swapped = _locomo(seth_speaker="a")
    assert [h["from_me"] for h in swapped["probes"][0]["history"]][:2] == [True, False]


def test_locomo_session_dates_become_history_timestamps_with_gaps():
    hist = _locomo()["probes"][0]["history"]
    assert hist[0]["ts"] == "2023-05-08 13:56:00"
    assert hist[5]["ts"] == "2023-05-20 19:10:00"  # session 2 opens at its own date
    stamps = [datetime.strptime(h["ts"], TS) for h in hist]
    assert stamps == sorted(stamps) and len(set(stamps)) == len(stamps)
    assert stamps[1] - stamps[0] >= timedelta(seconds=20)


def test_locomo_photo_turns_render_like_imessage():
    hist = _locomo()["probes"][0]["history"]
    assert hist[2]["text"].endswith(" [Photo]")
    cap = _locomo(photo_captions=True)["probes"][0]["history"]
    assert cap[2]["text"].endswith("[Photo: a photo of a clay bowl]")


def test_locomo_turns_group_consecutive_contact_bubbles():
    turns = _locomo()["turns"]
    assert [len(t["history"]) for t in turns] == [0, 2, 6]
    assert turns[1]["inbound_bubbles"] == [
        "Nice. I started pottery classes yesterday at the Kiln Room. [Photo]",
        "Still terrible at it lol",
    ]
    assert turns[1]["seth_reply_bubbles"] == ["Ha, keep at it. My dog Biscuit says hi."]
    for t in turns:
        assert set(rc.TURN_KEYS) <= set(t)
        assert t["contact_id"].endswith("@bench.invalid")


def test_locomo_probes_carry_category_gold_and_casual_question():
    out = _locomo()
    assert len(out["probes"]) == 7
    dog = _probe(out, "Rory's dog")
    assert dog["probe"]["category"] == "single_hop"
    assert dog["probe"]["gold_answers"] == ["Biscuit"]
    assert "your dog" in dog["inbound_bubbles"][0]
    year = _probe(out, "charity race")
    assert year["probe"]["category"] == "temporal" and year["probe"]["gold_answers"] == ["2023"]
    assert _probe(out, "pets")["probe"]["category"] == "multi_hop"
    od = _probe(out, "ceramics")["probe"]
    assert od["category"] == "open_domain" and od["gold_answers"] == ["Yes"]  # official ';' split
    adv = _probe(out, "raise money")["probe"]
    assert adv["category"] == "adversarial"
    assert adv["gold_answers"] == [] and adv["adversarial_answer"] == "literacy"


def test_locomo_probe_is_asked_after_the_whole_history():
    p = _locomo()["probes"][0]
    last = datetime.strptime(p["history"][-1]["ts"], TS)
    assert datetime.strptime(p["ts"], TS) - last == timedelta(hours=24)
    p2 = _locomo(probe_gap_hours=2)["probes"][0]
    assert datetime.strptime(p2["ts"], TS) - last == timedelta(hours=2)


def test_locomo_evidence_maps_to_history_and_window():
    out = _locomo(window=3)
    dog = _probe(out, "Rory's dog")["probe"]
    assert dog["evidence_history_idx"] == [4] and dog["evidence_in_window"] is False
    race = _probe(out, "charity race")["probe"]
    assert race["evidence_history_idx"] == [5] and race["evidence_in_window"] is True
    lisbon = _probe(out, "Where did Rory travel")["probe"]  # malformed upstream id "D:1:2"
    assert lisbon["evidence_history_idx"] == [1]
    wide = _probe(_locomo(window=25), "Rory's dog")["probe"]
    assert wide["evidence_in_window"] is True


def test_locomo_history_limit_truncates_and_marks_lost_evidence():
    out = _locomo(history_limit=3)
    p = _probe(out, "Rory's dog")
    assert len(p["history"]) == 3
    assert p["probe"]["evidence_in_history"] is False
    assert _probe(out, "charity race")["probe"]["evidence_in_history"] is True


def test_locomo_per_category_sample_is_deterministic_and_capped():
    out = _locomo(per_category=1)
    cats = [p["probe"]["category"] for p in out["probes"]]
    assert sorted(cats) == ["adversarial", "multi_hop", "open_domain", "single_hop", "temporal"]
    again = _locomo(per_category=1)
    assert [p["id"] for p in out["probes"]] == [p["id"] for p in again["probes"]]
    assert out["stats"]["probes"] == 5 and out["stats"]["probes_before_sampling"] == 7


def test_locomo_stats_and_determinism():
    a, b = _locomo(), _locomo()
    assert json.dumps(a, sort_keys=True) == json.dumps(b, sort_keys=True)
    st = a["stats"]
    assert st["conversations"] == 1 and st["turns"] == 3 and st["probes"] == 7
    assert st["probes_by_category"] == {
        "adversarial": 1, "multi_hop": 1, "open_domain": 1, "single_hop": 2, "temporal": 2}


# ── MSC ──────────────────────────────────────────────────────────────────


def test_msc_sessions_become_one_history_with_time_back_gaps():
    out = _msc()
    hist = out["probes"][0]["history"]
    assert len(hist) == 16  # 4 + 4 + 2 + 2 previous + 4 current
    assert [h["from_me"] for h in hist[:4]] == [False, True, False, True]  # Speaker 2 = Seth
    s1 = datetime.strptime(hist[0]["ts"], TS)
    s2 = datetime.strptime(hist[4]["ts"], TS)
    assert s2 - s1 == timedelta(days=6)  # "6 days 12 hours ago" vs "12 hours ago"
    stamps = [datetime.strptime(h["ts"], TS) for h in hist]
    assert stamps == sorted(stamps)


def test_msc_callback_probes_only_when_grounded_in_seth_lines():
    out = _msc()
    got = {p["probe"]["question_original"]: p["probe"]["gold_answers"] for p in out["probes"]}
    assert got == {
        "how many cats do you have again?": ["two", "2"],
        "what do you do for work again?": ["nurse"],
        "what's your favorite food again?": ["ramen"],
        "where do you live again?": ["Denver"],
        "what do you drive again?": ["red pickup"],
    }  # "favorite movie is Alien" never appears in Seth's lines -> no probe
    for p in out["probes"]:
        assert p["probe"]["dataset"] == "msc" and p["probe"]["category"] == "single_hop"
        assert p["inbound_bubbles"][0].endswith("?")


def test_msc_generic_gold_is_not_a_probe():
    facts = mr.callback_facts(["I live in the city.", "I live in a small town.", "I live in Ohio."])
    assert [g for _, g, _ in facts] == [["Ohio"]]


def test_msc_callback_window_uses_latest_mention():
    out = _msc(window=4)
    by_gold = {p["probe"]["gold_answers"][0]: p["probe"] for p in out["probes"]}
    assert by_gold["ramen"]["evidence_in_window"] is True  # repeated in the current session
    assert by_gold["nurse"]["evidence_in_window"] is False


def test_msc_speaker_one_as_seth_flips_roles():
    out = _msc(seth_speaker=1)
    assert out["probes"] == []  # speaker 1's personas match no callback template
    first = out["turns"][0]
    assert first["inbound_bubbles"] == ["I work as a nurse at the county hospital. I also have two cats."]
    assert [h["from_me"] for h in first["history"]] == [True]


# ── SOC-2508 ─────────────────────────────────────────────────────────────


def test_soc_multi_bubble_turns_tags_and_delays():
    out = _soc()
    turns = out["turns"]
    assert len(turns) == 2 and out["probes"] == []
    assert turns[0]["inbound_bubbles"] == ["hey!! you around?", "[Photo] look at this"]
    assert turns[0]["seth_reply_bubbles"] == ["omg gorgeous", "where is that"]
    assert turns[0]["seth_reply_delay_seconds"] == 300
    assert turns[1]["inbound_bubbles"] == ["lake tahoe <3", "[GIF]"]  # "<3" is not a tag
    assert turns[1]["seth_reply_bubbles"] == ["[Voice Message]", "jealous"]
    t0 = datetime.strptime(turns[0]["ts"], TS)
    start = datetime(2025, 8, 8, 9, 24, 44)  # chat_id epoch 1754645084, UTC
    assert start < t0 < start + timedelta(minutes=5)  # ts = the last inbound bubble
    hist = turns[1]["history"]
    assert len(hist) == 4 and hist[2]["from_me"] is True
    gap = datetime.strptime(turns[1]["ts"], TS) - datetime.strptime(hist[-1]["ts"], TS)
    assert gap >= timedelta(hours=1, minutes=2)


def test_soc_stats_report_bubble_and_delay_distribution():
    st = _soc()["stats"]
    assert st["conversations"] == 1 and st["turns"] == 2
    assert st["bubbles_per_part"] == {"2": 4}  # delay/end tags are not bubbles
    assert st["delayed_parts"] == 2


# ── CLI ──────────────────────────────────────────────────────────────────


def test_cli_writes_turns_probes_and_stats(tmp_path):
    out = tmp_path / "replay"
    r = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "datasets" / "locomo_to_replay.py"),
         "--input", str(FX / "locomo_tiny.json"), "--out-dir", str(out)],
        capture_output=True, text=True, check=True)
    stats = json.loads(r.stdout)
    assert stats["probes"] == 7
    probes = [json.loads(l) for l in (out / "probes.jsonl").read_text().splitlines()]
    turns = [json.loads(l) for l in (out / "turns.jsonl").read_text().splitlines()]
    assert len(probes) == 7 and len(turns) == 3
    assert json.loads((out / "stats.json").read_text()) == stats


# ── contract with `human replay` (hu_cli_replay_parse_turn) ──────────────


def _check_parser_contract(rec):
    """What hu_cli_replay_parse_turn requires, else HU_ERR_PARSE: a string
    id, a non-empty contact_id, a non-empty array of string inbound_bubbles,
    and history objects whose text is a string; from_me bool, ts string."""
    assert isinstance(rec["id"], str) and rec["id"]
    assert isinstance(rec["contact_id"], str) and rec["contact_id"]
    assert rec["inbound_bubbles"] and all(isinstance(b, str) and b for b in rec["inbound_bubbles"])
    assert isinstance(rec["ts"], str) and datetime.strptime(rec["ts"], TS)
    for h in rec["history"]:
        assert set(h) == {"from_me", "text", "ts"}
        assert isinstance(h["from_me"], bool) and isinstance(h["text"], str)
        assert datetime.strptime(h["ts"], TS)


def test_every_record_meets_the_replay_parser_contract():
    outs = [_locomo(), _msc(), _soc()]
    recs = [r for o in outs for r in o["turns"] + o["probes"]]
    assert len(recs) > 10
    ids = [r["id"] for r in recs]
    assert len(ids) == len(set(ids))
    for r in recs:
        _check_parser_contract(r)
    for o in outs:
        for t in o["turns"]:
            assert t["seth_action"] == "text" and t["seth_reply_bubbles"]
