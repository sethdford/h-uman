#!/usr/bin/env python3
"""Hermetic tests for scripts/eval_prospective_memory.py.

Builds a synthetic memory.db under tmp_path — never touches ~/.human.
Run with: pytest scripts/test_eval_prospective_memory.py -v
"""
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import eval_prospective_memory as epm  # noqa: E402

NOW = 1_790_000_000
SINCE = NOW - 7 * 86400


def _db(tmp_path, triggers, inbound):
    p = tmp_path / "memory.db"
    con = sqlite3.connect(p)
    con.execute("CREATE TABLE prospective_memories(id INTEGER PRIMARY KEY, trigger_type TEXT, "
                "trigger_value TEXT, action TEXT, contact_id TEXT, expires_at INTEGER, "
                "fired INTEGER DEFAULT 0, created_at INTEGER)")
    con.execute("CREATE TABLE messages(id INTEGER PRIMARY KEY, session_id TEXT, role TEXT, "
                "content TEXT, created_at TEXT)")
    for t in triggers:
        con.execute("INSERT INTO prospective_memories(trigger_type,trigger_value,action,contact_id,"
                    "expires_at,fired,created_at) VALUES('keyword',?,?,?,?,?,?)",
                    (t["v"], "act", t["c"], t.get("exp"), t.get("fired", 0), t.get("created", SINCE + 10)))
    for sid, text, ep in inbound:
        ts = epm.datetime.fromtimestamp(ep, epm.timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
        con.execute("INSERT INTO messages(session_id,role,content,created_at) VALUES(?, 'user', ?, ?)",
                    (sid, text, ts))
    con.commit()
    con.close()
    return str(p)


def test_counts_and_refusal_below_min_n(tmp_path):
    db = _db(tmp_path,
             [{"v": "interview", "c": "+1"}],
             [("+1", "my interview is tomorrow", SINCE + 100)])
    r = epm.evaluate(db, SINCE, min_n=30, now=NOW)
    assert r["counts"] == {"open": 1, "expired_unfired": 0, "fired": 0, "pruned": 0,
                           "cued": 1, "fired_and_cued": 0, "inbound_messages_since": 1}
    assert r["rates"]["status"] == "REFUSE"
    assert r["rates"]["fire_rate_on_cue"] is None
    assert r["rates"]["f1"] is None and r["gate"]["pass"] is None


def test_fire_rate_when_n_sufficient(tmp_path):
    trig = [{"v": "interview", "c": "+1", "fired": 1}, {"v": "closing", "c": "+2"}]
    msgs = [("+1", "interview went well", SINCE + 100), ("+2", "closing is the 14th", SINCE + 100)]
    db = _db(tmp_path, trig, msgs)
    r = epm.evaluate(db, SINCE, min_n=2, now=NOW)
    assert r["counts"]["cued"] == 2 and r["counts"]["fired_and_cued"] == 1
    assert r["rates"]["status"] == "OK"
    assert r["rates"]["fire_rate_on_cue"] == 0.5


def test_whole_word_match_not_substring(tmp_path):
    db = _db(tmp_path, [{"v": "work", "c": "+1"}],
             [("+1", "went to bath and body works", SINCE + 100)])
    assert epm.evaluate(db, SINCE, 1, now=NOW)["counts"]["cued"] == 0
    (tmp_path / "b").mkdir()
    db2 = _db(tmp_path / "b", [{"v": "work", "c": "+1"}],
              [("+1", "back at work today", SINCE + 100)])
    assert epm.evaluate(db2, SINCE, 1, now=NOW)["counts"]["cued"] == 1


def test_cue_before_trigger_creation_or_before_since_does_not_count(tmp_path):
    db = _db(tmp_path, [{"v": "interview", "c": "+1", "created": SINCE + 500}],
             [("+1", "interview tomorrow", SINCE + 100),      # after since, before creation
              ("+1", "interview tomorrow", SINCE - 100)])     # before since
    r = epm.evaluate(db, SINCE, 1, now=NOW)
    assert r["counts"]["cued"] == 0
    assert r["counts"]["inbound_messages_since"] == 1


def test_expired_and_pruned_buckets(tmp_path):
    db = _db(tmp_path, [{"v": "a", "c": "+1", "exp": NOW - 1},
                        {"v": "b", "c": "+1", "fired": 2},
                        {"v": "c", "c": "+1", "exp": NOW + 1}], [])
    c = epm.evaluate(db, SINCE, 30, now=NOW)["counts"]
    assert c["expired_unfired"] == 1 and c["pruned"] == 1 and c["open"] == 1


def test_other_contacts_message_does_not_cue(tmp_path):
    db = _db(tmp_path, [{"v": "interview", "c": "+1"}],
             [("+2", "interview tomorrow", SINCE + 100)])
    assert epm.evaluate(db, SINCE, 1, now=NOW)["counts"]["cued"] == 0
