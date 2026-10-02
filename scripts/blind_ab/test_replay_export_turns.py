"""Hermetic tests for replay_export_turns.py on a fixture chat.db.

Pins: 1:1 threads only; a turn is their text run + Seth's text run or his
tapback on their message; history is the thread before the turn, labelled;
the db is opened read-only and left byte-identical; output lands 0600 in a
0700 run dir outside the repo; nothing but counts reaches stdout.
"""
import hashlib
import json
import os
import sqlite3
import stat
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import replay_export_turns as ex  # noqa: E402

BASE = 800_000_000  # seconds since 2001 (2026-05)


def ns(sec):
    return (BASE + sec) * 1_000_000_000


def build_chat_db(path):
    con = sqlite3.connect(path)
    con.executescript("""
        CREATE TABLE handle (ROWID INTEGER PRIMARY KEY, id TEXT);
        CREATE TABLE chat (ROWID INTEGER PRIMARY KEY);
        CREATE TABLE chat_handle_join (chat_id INTEGER, handle_id INTEGER);
        CREATE TABLE chat_message_join (chat_id INTEGER, message_id INTEGER);
        CREATE TABLE message (ROWID INTEGER PRIMARY KEY, guid TEXT, text TEXT,
            attributedBody BLOB, handle_id INTEGER, date INTEGER, is_from_me INTEGER,
            item_type INTEGER DEFAULT 0, associated_message_type INTEGER DEFAULT 0,
            associated_message_guid TEXT);
    """)
    con.executemany("INSERT INTO handle VALUES (?,?)", [(1, "+15550001111"), (2, "+15550002222"),
                                                        (3, "friend@example.com")])
    con.executemany("INSERT INTO chat VALUES (?)", [(1,), (2,)])
    # chat 1: 1:1 with handle 1; chat 2: a group (handles 2 and 3)
    con.executemany("INSERT INTO chat_handle_join VALUES (?,?)", [(1, 1), (2, 2), (2, 3)])
    msgs = [
        # (rowid, guid, text, handle, t, from_me, amt, aguid)
        (1, "g1", "old news", 1, 0, 0, 0, None),
        (2, "g2", "ha nice", 1, 60, 1, 0, None),
        (3, "g3", "hey", 1, 1000, 0, 0, None),             # turn A: two inbound bubbles
        (4, "g4", "you around tonight?", 1, 1010, 0, 0, None),
        (5, "g5", "yeah", 1, 1100, 1, 0, None),             # Seth: two bubbles
        (6, "g6", "what time", 1, 1105, 1, 0, None),
        (7, "g7", "lol perfect", 1, 2000, 0, 0, None),      # turn B: Seth tapbacks it
        (8, "g8", None, 1, 2030, 1, 2001, "p:0/g7"),
        (9, "g9", "you up", 1, 3000, 0, 0, None),           # no reply within the gap
        (10, "g10", "sorry just saw this", 1, 3000 + 6 * 3600, 1, 0, None),
        (11, "g11", "group hi", 2, 1000, 0, 0, None),       # group: never exported
        (12, "g12", "group reply", 2, 1050, 1, 0, None),
    ]
    for rowid, guid, text, h, t, me, amt, aguid in msgs:
        con.execute("INSERT INTO message VALUES (?,?,?,?,?,?,?,0,?,?)",
                    (rowid, guid, text, None, h, ns(t), me, amt, aguid))
        con.execute("INSERT INTO chat_message_join VALUES (?,?)", (1 if h == 1 else 2, rowid))
    con.commit()
    con.close()


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


@pytest.fixture
def fixture(tmp_path):
    db = tmp_path / "chat.db"
    build_chat_db(str(db))
    return tmp_path, str(db)


def run_export(tmp_path, db, capsys, *extra):
    rc = ex.main(["--name", "r1", "--run-root", str(tmp_path / "runs"), "--db", db,
                  "--since-days", "0", *extra])
    out = capsys.readouterr()
    return rc, out


def read_turns(tmp_path):
    with open(tmp_path / "runs" / "r1" / "turns.jsonl") as f:
        return [json.loads(line) for line in f]


def test_exports_text_and_tapback_turns_from_one_to_one_threads_only(fixture, capsys):
    tmp_path, db = fixture
    rc, _ = run_export(tmp_path, db, capsys)
    assert rc == 0
    turns = read_turns(tmp_path)
    assert [t["seth_action"] for t in turns] == ["text", "text", "tapback"]
    first, a, b = turns
    assert first["inbound_bubbles"] == ["old news"] and first["history"] == []
    assert a["contact_id"] == "+15550001111"
    assert a["inbound_bubbles"] == ["hey", "you around tonight?"]
    assert a["seth_reply_bubbles"] == ["yeah", "what time"]
    assert [h["from_me"] for h in a["history"]] == [False, True]
    assert [h["text"] for h in a["history"]] == ["old news", "ha nice"]
    assert b["inbound_bubbles"] == ["lol perfect"] and b["seth_reply_bubbles"] == []
    assert all("group" not in json.dumps(t) for t in turns)
    assert [t["id"] for t in turns] == ["t0001", "t0002", "t0003"]


def test_history_is_capped(fixture, capsys):
    tmp_path, db = fixture
    run_export(tmp_path, db, capsys, "--history", "1")
    a = read_turns(tmp_path)[1]
    assert [h["text"] for h in a["history"]] == ["ha nice"]


def test_chat_db_untouched_and_output_private(fixture, capsys):
    tmp_path, db = fixture
    before = sha(db)
    rc, out = run_export(tmp_path, db, capsys)
    assert rc == 0
    assert sha(db) == before
    run_dir = tmp_path / "runs" / "r1"
    assert stat.S_IMODE(os.stat(run_dir).st_mode) == 0o700
    assert stat.S_IMODE(os.stat(run_dir / "turns.jsonl").st_mode) == 0o600
    for text in ("you around tonight", "what time", "lol perfect", "+15550001111", "old news"):
        assert text not in out.out and text not in out.err


def test_refuses_a_run_dir_inside_the_repo(fixture, capsys):
    tmp_path, db = fixture
    rc = ex.main(["--name", "r1", "--run-root", ex.REPO_ROOT, "--db", db, "--since-days", "0"])
    assert rc == 2
    assert not os.path.exists(os.path.join(ex.REPO_ROOT, "r1"))


def test_limit_and_per_contact_caps(fixture, capsys):
    tmp_path, db = fixture
    run_export(tmp_path, db, capsys, "--limit", "1")
    turns = read_turns(tmp_path)
    assert len(turns) == 1
    assert turns[0]["seth_action"] == "tapback"  # newest first


def test_apple_ts_handles_seconds_and_nanoseconds():
    assert ex.apple_ts(ns(5)) == BASE + 5 + ex.APPLE_EPOCH
    assert ex.apple_ts(BASE + 5) == BASE + 5 + ex.APPLE_EPOCH
