#!/usr/bin/env python3
"""Hermetic tests for scripts/persona_timing_from_chatdb.py.

Synthetic chat.db + persona under tmp_path; never touches ~/Library/Messages
or ~/.human. Run with: pytest scripts/test_persona_timing_from_chatdb.py -v
"""
import json
import sqlite3
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import persona_timing_from_chatdb as pt  # noqa: E402

NOW = int(time.time())


def _chatdb(tmp_path, sends):
    """sends: list of (days_ago, local_hour, is_from_me)."""
    p = tmp_path / "chat.db"
    con = sqlite3.connect(p)
    con.execute("CREATE TABLE message(ROWID INTEGER PRIMARY KEY, date INTEGER, is_from_me INTEGER, text TEXT)")
    for days_ago, hour, mine in sends:
        # build a local-time timestamp at `hour`, `days_ago` days back
        t = time.localtime(NOW - days_ago * 86400)
        local = time.mktime((t.tm_year, t.tm_mon, t.tm_mday, hour, 30, 0, 0, 0, -1))
        apple_ns = int((local - pt.APPLE_EPOCH) * 1_000_000_000)
        con.execute("INSERT INTO message(date,is_from_me,text) VALUES(?,?,NULL)", (apple_ns, mine))
    con.commit()
    con.close()
    return str(p)


def test_histogram_counts_only_my_sends_in_window(tmp_path):
    db = _chatdb(tmp_path, [(1, 12, 1), (1, 12, 1), (1, 12, 0), (400, 12, 1)])
    h = pt.histogram(db, days=180, now=NOW)
    assert sum(h["weekday"]) + sum(h["weekend"]) == 2
    assert h["weekday"][12] + h["weekend"][12] == 2


def test_chronotype_lark_owl_intermediate_unknown():
    lark = {"weekday": [0] * 24, "weekend": [0] * 24}
    lark["weekday"][7] = 30; lark["weekday"][12] = 70
    assert pt.classify_chronotype(lark, 100)[0] == "morning_lark"
    owl = {"weekday": [0] * 24, "weekend": [0] * 24}
    owl["weekday"][22] = 30; owl["weekday"][12] = 70
    assert pt.classify_chronotype(owl, 100)[0] == "evening_owl"
    mid = {"weekday": [0] * 24, "weekend": [0] * 24}
    mid["weekday"][12] = 100
    assert pt.classify_chronotype(mid, 100)[0] == "intermediate"
    assert pt.classify_chronotype(mid, 1000)[0] == "unknown"


def test_routine_blocks_are_contiguous_and_cover_24h():
    # uniform share = 95/24 ~ 3.96: 5s are medium (>= 0.5x), 40 is high (>= 1.5x), 0 is low
    counts = [0] * 24
    for hr in range(10, 22):
        counts[hr] = 5
    counts[12] = 40
    blocks = pt.routine_blocks(counts)
    assert [b["time"] for b in blocks] == ["00-10", "10-12", "12-13", "13-22", "22-00"]
    assert [b["availability"] for b in blocks] == ["low", "medium", "high", "medium", "low"]
    assert blocks[-1]["time"].endswith("-00")
    assert abs(sum(b["measured_share"] for b in blocks) - 1.0) < 0.01


def test_write_persona_preserves_other_keys_and_backs_up(tmp_path):
    persona = tmp_path / "seth.json"
    original = {"version": 1, "name": "seth", "core": {"identity": "x", "traits": []},
                "voice": {"enabled": True}, "voice_messages": {"k": 1}}
    persona.write_text(json.dumps(original))
    db = _chatdb(tmp_path, [(1, 12, 1)] * 5)
    frag = pt.build_fragment(pt.histogram(db, 180, now=NOW), min_n=1, tz="America/New_York")
    backup = pt.write_persona(str(persona), frag)
    assert Path(backup).exists() and json.loads(Path(backup).read_text()) == original
    out = json.loads(persona.read_text())
    for k in original:
        assert out[k] == original[k]
    assert out["timezone"] == "America/New_York"
    assert out["chronotype"] in ("morning_lark", "intermediate", "evening_owl", "unknown")
    assert set(out["time_overlays"]) == {"late_night", "early_morning", "afternoon", "evening"}
    # all five sends land on one day-type (whichever "yesterday" is); the other is empty
    wd, we = out["daily_routine"]["weekday"], out["daily_routine"]["weekend"]
    assert (wd and not we) or (we and not wd)
