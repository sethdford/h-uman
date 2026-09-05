#!/usr/bin/env python3
"""Hermetic tests for scripts/eval_seth_initiation_baseline.py (US-3).

Builds a synthetic sqlite file shaped like chat.db (the minimal columns
load_dm_messages() queries: handle, chat_handle_join, message,
chat_message_join) with hand-constructed timestamps, then verifies the
initiation/unanswered logic and the refusal contract. NEVER touches the
real ~/Library/Messages/chat.db — every fixture here is a tmp_path file.

Run with: pytest scripts/test_eval_seth_initiation_baseline.py -v
"""
import inspect
import json
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
sys.path.insert(0, str(Path(__file__).parent / "blind_ab"))

import eval_seth_initiation_baseline as esib  # noqa: E402
import eval_when_to_speak as ews  # noqa: E402
from score import wilson  # noqa: E402

APPLE_EPOCH = 978307200


def unix_to_apple_ns(ts_unix):
    return int(round((ts_unix - APPLE_EPOCH) * 1_000_000_000))


def make_fixture_db(path):
    """Creates (and commits + closes) a chat.db-shaped sqlite file at
    `path`, with only the columns load_dm_messages() /
    load_dm_messages_excluding_tapbacks() touch. associated_message_type
    defaults to 0 (ordinary message) so every pre-existing test that
    never sets it keeps behaving exactly as before (Finding F1's fix
    filters on COALESCE(associated_message_type, 0) = 0)."""
    con = sqlite3.connect(str(path))
    con.executescript(
        """
        CREATE TABLE handle (ROWID INTEGER PRIMARY KEY, id TEXT);
        CREATE TABLE chat_handle_join (chat_id INTEGER, handle_id INTEGER);
        CREATE TABLE message (
            ROWID INTEGER PRIMARY KEY,
            date INTEGER,
            is_from_me INTEGER,
            is_system_message INTEGER DEFAULT 0,
            item_type INTEGER DEFAULT 0,
            associated_message_type INTEGER DEFAULT 0
        );
        CREATE TABLE chat_message_join (chat_id INTEGER, message_id INTEGER);
        """
    )
    con.commit()
    con.close()


class FixtureBuilder:
    """Small stateful helper so tests can add DM chats/messages by unix
    timestamp without hand-tracking ROWIDs."""

    def __init__(self, path):
        make_fixture_db(path)
        self.con = sqlite3.connect(str(path))
        self._msg_id = 0
        self._handle_id = 0

    def add_dm_chat(self, chat_id, contact_id):
        self._handle_id += 1
        handle_row = self._handle_id
        self.con.execute("INSERT INTO handle (ROWID, id) VALUES (?, ?)", (handle_row, contact_id))
        self.con.execute(
            "INSERT INTO chat_handle_join (chat_id, handle_id) VALUES (?, ?)", (chat_id, handle_row)
        )
        return handle_row

    def add_group_chat(self, chat_id, contact_ids):
        """A chat joined to >1 handle_id — load_dm_messages() must exclude it."""
        for contact_id in contact_ids:
            self.add_dm_chat(chat_id, contact_id)

    def add_message(self, chat_id, ts_unix, is_from_me, associated_message_type=0):
        self._msg_id += 1
        msg_id = self._msg_id
        self.con.execute(
            "INSERT INTO message (ROWID, date, is_from_me, associated_message_type) VALUES (?, ?, ?, ?)",
            (msg_id, unix_to_apple_ns(ts_unix), 1 if is_from_me else 0, associated_message_type),
        )
        self.con.execute(
            "INSERT INTO chat_message_join (chat_id, message_id) VALUES (?, ?)", (chat_id, msg_id)
        )
        return msg_id

    def commit_and_close(self):
        self.con.commit()
        self.con.close()


BASE_TS = 1_700_000_000.0  # arbitrary fixed anchor, well within any lookback window


# ── pure-function tests (no chat.db at all) ─────────────────────────────


def test_known_rate_exact():
    """N=40 initiations with exactly K=11 unanswered, constructed by
    direct dict, not via chat.db. compute_baseline()'s rate and Wilson CI
    must match the imported wilson() bit-for-bit."""
    n, k = 40, 11
    labeled = [{"chat_id": 1, "ts": float(i), "unanswered": i < k} for i in range(n)]
    result = esib.compute_baseline(labeled)
    assert result["n"] == n
    assert result["unanswered"] == k
    assert result["rate"] == k / n
    expected_p, expected_lo, expected_hi = wilson(k, n)
    assert result["wilson_ci"] == [expected_lo, expected_hi]


def test_window_parity_with_eval_when_to_speak():
    """Proves FIR_WINDOW_HOURS is the SAME imported name, not a
    coincidentally-equal copy (AC-3.6's third required case, verbatim)."""
    assert esib.FIR_WINDOW_HOURS is ews.FIR_WINDOW_HOURS


def test_no_write_statements_in_module():
    """Static half of AC-3.2: the module never issues a write statement.

    Checks the SQL-statement shape ("INSERT INTO", not bare "INSERT") so
    this doesn't false-positive on the legitimate `sys.path.insert(...)`
    calls at module import time -- a naive bare-"INSERT" substring check
    fails on those even though they are not SQL."""
    src = inspect.getsource(esib)
    assert "INSERT INTO" not in src.upper()
    assert "UPDATE" not in src.upper()
    assert "DELETE FROM" not in src.upper()


def test_open_ro_uri_used(tmp_path):
    """This module's own open_ro() (not eval_when_to_speak.py's, which
    lacks immutable=1) must connect with mode=ro AND immutable=1."""
    db_path = tmp_path / "chat.db"
    make_fixture_db(db_path)

    calls = {}
    real_connect = sqlite3.connect

    def spy_connect(database, *a, **kw):
        calls["uri"] = database
        return real_connect(database, *a, **kw)

    sqlite3.connect = spy_connect
    try:
        con = esib.open_ro(str(db_path))
        assert con is not None
        con.close()
    finally:
        sqlite3.connect = real_connect

    assert "mode=ro" in calls["uri"]
    assert "immutable=1" in calls["uri"]


def test_open_ro_missing_file_returns_none(tmp_path):
    assert esib.open_ro(str(tmp_path / "does-not-exist.db")) is None


# ── initiation / unanswered logic over synthetic chat.db ────────────────


def test_first_message_in_chat_is_an_initiation(tmp_path):
    db_path = tmp_path / "chat.db"
    fb = FixtureBuilder(db_path)
    fb.add_dm_chat(chat_id=1, contact_id="+15550001111")
    fb.add_message(chat_id=1, ts_unix=BASE_TS, is_from_me=True)
    fb.commit_and_close()

    chat_db = esib.open_ro(str(db_path))
    messages = ews.load_dm_messages(chat_db, since_unix=BASE_TS - 3600)
    initiations = esib.find_initiations(messages)
    chat_db.close()

    assert len(initiations) == 1
    assert initiations[0]["chat_id"] == 1


def test_gap_boundary_exact_hours(tmp_path):
    """A preceding message at exactly gap_hours*3600s before is an
    initiation (>=, not >); one second short is not."""
    gap_hours = 6.0
    gap_secs = gap_hours * 3600.0

    # Chat A: gap is exactly gap_secs -> the second outbound message IS an initiation.
    db_a = tmp_path / "chat_a.db"
    fb = FixtureBuilder(db_a)
    fb.add_dm_chat(chat_id=1, contact_id="+15550001111")
    fb.add_message(chat_id=1, ts_unix=BASE_TS, is_from_me=False)
    fb.add_message(chat_id=1, ts_unix=BASE_TS + gap_secs, is_from_me=True)
    fb.commit_and_close()
    chat_db = esib.open_ro(str(db_a))
    messages = ews.load_dm_messages(chat_db, since_unix=BASE_TS - 3600)
    initiations = esib.find_initiations(messages, gap_hours=gap_hours)
    chat_db.close()
    # Both the first (inbound) message-as-a-chat-boundary and the
    # exactly-on-boundary outbound message: only the outbound one counts
    # (find_initiations only labels is_from_me=True rows).
    assert any(i["ts"] == BASE_TS + gap_secs for i in initiations), initiations

    # Chat B: gap is one second short of gap_secs -> NOT an initiation.
    db_b = tmp_path / "chat_b.db"
    fb = FixtureBuilder(db_b)
    fb.add_dm_chat(chat_id=1, contact_id="+15550002222")
    fb.add_message(chat_id=1, ts_unix=BASE_TS, is_from_me=False)
    fb.add_message(chat_id=1, ts_unix=BASE_TS + gap_secs - 1, is_from_me=True)
    fb.commit_and_close()
    chat_db = esib.open_ro(str(db_b))
    messages = ews.load_dm_messages(chat_db, since_unix=BASE_TS - 3600)
    initiations = esib.find_initiations(messages, gap_hours=gap_hours)
    chat_db.close()
    assert not any(i["ts"] == BASE_TS + gap_secs - 1 for i in initiations), initiations


def test_group_chats_excluded(tmp_path):
    """A chat joined to 2 distinct handle_ids contributes zero
    initiations — mirrors load_dm_messages()'s own
    HAVING COUNT(DISTINCT handle_id) = 1, re-verifying the imported
    helper still filters groups."""
    db_path = tmp_path / "chat.db"
    fb = FixtureBuilder(db_path)
    fb.add_group_chat(chat_id=1, contact_ids=["+15550001111", "+15550002222"])
    # Plenty of outbound sends, each far enough apart to be an initiation
    # if this were (wrongly) treated as a DM.
    for i in range(5):
        fb.add_message(chat_id=1, ts_unix=BASE_TS + i * 100_000, is_from_me=True)
    fb.commit_and_close()

    chat_db = esib.open_ro(str(db_path))
    messages = ews.load_dm_messages(chat_db, since_unix=BASE_TS - 3600)
    chat_db.close()

    assert messages == []
    initiations = esib.find_initiations(messages)
    assert initiations == []


# ── Finding F1: tapback reactions must not count as replies or initiations ──


def test_inbound_tapback_not_counted_as_reply(tmp_path):
    """A tapback (associated_message_type=2000, e.g. a heart-react) on
    Seth's initiation must NOT count as the inbound reply that would mark
    it answered. Contrasts the fixed loader
    (load_dm_messages_excluding_tapbacks) against the unfiltered
    eval_when_to_speak.load_dm_messages() to pin the exact bug Finding F1
    describes: without the filter, label_unanswered() sees the tapback's
    is_from_me=0 row and (wrongly) calls the initiation answered."""
    db_path = tmp_path / "chat.db"
    fb = FixtureBuilder(db_path)
    fb.add_dm_chat(chat_id=1, contact_id="+15550001111")
    fb.add_message(chat_id=1, ts_unix=BASE_TS, is_from_me=True)  # Seth's real initiation
    # The only inbound row in the FIR window is a bare tapback, not a
    # real reply.
    fb.add_message(chat_id=1, ts_unix=BASE_TS + 60.0, is_from_me=False, associated_message_type=2000)
    fb.commit_and_close()

    chat_db = esib.open_ro(str(db_path))
    filtered = esib.load_dm_messages_excluding_tapbacks(chat_db, since_unix=BASE_TS - 3600)
    unfiltered = ews.load_dm_messages(chat_db, since_unix=BASE_TS - 3600)
    chat_db.close()

    # The tapback row is gone from the filtered messages; only Seth's
    # initiation remains. The unfiltered loader still has both rows.
    assert filtered == [(1, "+15550001111", BASE_TS, True)]
    assert len(unfiltered) == 2

    initiations = esib.find_initiations(filtered)
    labeled = esib.label_unanswered(initiations, filtered)
    assert len(labeled) == 1
    assert labeled[0]["unanswered"] is True

    # Regression pin: run the SAME logic against the unfiltered messages
    # and confirm it produces the wrong (pre-fix) answer -- a bare
    # heart-react counted as a reply.
    initiations_unfiltered = esib.find_initiations(unfiltered)
    labeled_unfiltered = esib.label_unanswered(initiations_unfiltered, unfiltered)
    assert len(labeled_unfiltered) == 1
    assert labeled_unfiltered[0]["unanswered"] is False


def test_outbound_tapback_not_counted_as_initiation(tmp_path):
    """A tapback SENT by Seth (associated_message_type=2000) must not be
    treated as a conversation-opening initiation -- a heart-react is not
    a fresh outreach, even though it is is_from_me=1 and (in this
    fixture) the first recorded row in the chat. Contrasts against the
    unfiltered loader to pin the bug Finding F1 describes."""
    db_path = tmp_path / "chat.db"
    fb = FixtureBuilder(db_path)
    fb.add_dm_chat(chat_id=1, contact_id="+15550002222")
    # Seth's only recorded row in this chat/window is a tapback, not a
    # real opener.
    fb.add_message(chat_id=1, ts_unix=BASE_TS, is_from_me=True, associated_message_type=2000)
    fb.commit_and_close()

    chat_db = esib.open_ro(str(db_path))
    filtered = esib.load_dm_messages_excluding_tapbacks(chat_db, since_unix=BASE_TS - 3600)
    unfiltered = ews.load_dm_messages(chat_db, since_unix=BASE_TS - 3600)
    chat_db.close()

    assert filtered == []
    assert esib.find_initiations(filtered) == []

    # Regression pin: the unfiltered loader still has the tapback row,
    # and find_initiations() (pre-fix behavior) would wrongly count it as
    # a first-message initiation.
    assert len(unfiltered) == 1
    assert len(esib.find_initiations(unfiltered)) == 1


def test_tapback_types_2000_through_2006_all_excluded(tmp_path):
    """Every documented tapback associated_message_type (2000 add, plus
    the 2001/2003-2006 variants/removals seen live on this machine) is
    excluded, not just the most common 2000 -- the filter is
    COALESCE(associated_message_type, 0) = 0, not an allowlist of one
    value."""
    db_path = tmp_path / "chat.db"
    fb = FixtureBuilder(db_path)
    fb.add_dm_chat(chat_id=1, contact_id="+15550003333")
    tapback_types = [2000, 2001, 2003, 2004, 2005, 2006]
    for i, t in enumerate(tapback_types):
        fb.add_message(chat_id=1, ts_unix=BASE_TS + i * 10.0, is_from_me=False, associated_message_type=t)
    # One ordinary inbound message so the chat isn't entirely empty post-filter.
    fb.add_message(chat_id=1, ts_unix=BASE_TS + 1000.0, is_from_me=False)
    fb.commit_and_close()

    chat_db = esib.open_ro(str(db_path))
    filtered = esib.load_dm_messages_excluding_tapbacks(chat_db, since_unix=BASE_TS - 3600)
    chat_db.close()

    assert len(filtered) == 1
    assert filtered[0][2] == BASE_TS + 1000.0


def test_unanswered_labeling_and_end_to_end_baseline(tmp_path):
    """Builds N=32 initiations across distinct chats: 20 answered
    (inbound reply inside the window), 12 unanswered (no reply, or reply
    outside the window) -- then verifies label_unanswered + compute_baseline
    agree with the by-hand count, and refuse-vs-min-n is not tripped."""
    db_path = tmp_path / "chat.db"
    fb = FixtureBuilder(db_path)
    window_secs = esib.FIR_WINDOW_HOURS * 3600.0
    n_answered, n_unanswered = 20, 12

    for i in range(n_answered):
        chat_id = i
        fb.add_dm_chat(chat_id=chat_id, contact_id=f"+1555000{i:04d}")
        ts = BASE_TS + i * 1_000_000
        fb.add_message(chat_id=chat_id, ts_unix=ts, is_from_me=True)
        fb.add_message(chat_id=chat_id, ts_unix=ts + 60.0, is_from_me=False)  # answered fast

    for i in range(n_unanswered):
        chat_id = 1000 + i
        fb.add_dm_chat(chat_id=chat_id, contact_id=f"+1555111{i:04d}")
        ts = BASE_TS + i * 1_000_000
        fb.add_message(chat_id=chat_id, ts_unix=ts, is_from_me=True)
        # No reply at all for half; a too-late reply for the other half.
        if i % 2 == 0:
            fb.add_message(chat_id=chat_id, ts_unix=ts + window_secs + 3600.0, is_from_me=False)
    fb.commit_and_close()

    chat_db = esib.open_ro(str(db_path))
    messages = ews.load_dm_messages(chat_db, since_unix=BASE_TS - 3600)
    chat_db.close()

    initiations = esib.find_initiations(messages)
    assert len(initiations) == n_answered + n_unanswered

    labeled = esib.label_unanswered(initiations, messages)
    baseline = esib.compute_baseline(labeled)

    assert baseline["n"] == n_answered + n_unanswered
    assert baseline["unanswered"] == n_unanswered
    assert baseline["rate"] == n_unanswered / (n_answered + n_unanswered)
    expected_p, expected_lo, expected_hi = wilson(n_unanswered, n_answered + n_unanswered)
    assert baseline["wilson_ci"] == [expected_lo, expected_hi]


# ── refusal contract + no-identifying-fields (main() end-to-end) ────────


def test_refuses_below_min_n(tmp_path, capsys):
    """5 initiations total (< 30) -> main() returns non-zero, no output
    file written, stderr contains REFUSE."""
    db_path = tmp_path / "chat.db"
    out_dir = tmp_path / "out"
    fb = FixtureBuilder(db_path)
    for i in range(5):
        chat_id = i
        fb.add_dm_chat(chat_id=chat_id, contact_id=f"+1555000{i:04d}")
        fb.add_message(chat_id=chat_id, ts_unix=BASE_TS + i * 1_000_000, is_from_me=True)
    fb.commit_and_close()

    rc = esib.main(
        [
            "--chat-db", str(db_path),
            "--out-dir", str(out_dir),
            "--days", "9999",
            "--min-n", "30",
        ]
    )
    captured = capsys.readouterr()

    assert rc != 0
    assert "REFUSE" in captured.err
    assert not out_dir.exists() or list(out_dir.iterdir()) == []


def test_chat_db_missing_refuses(tmp_path, capsys):
    rc = esib.main(["--chat-db", str(tmp_path / "nope.db"), "--out-dir", str(tmp_path / "out")])
    captured = capsys.readouterr()
    assert rc != 0
    assert "REFUSE" in captured.err


def test_output_has_no_identifying_fields_and_date_range(tmp_path, capsys):
    """End-to-end run against a synthetic DB with n=32 (>= default min-n
    of 30). Asserts the written JSON's top-level keys are exactly the
    documented allowlist, and date_range reflects the constructed data
    (not the --days argument)."""
    db_path = tmp_path / "chat.db"
    out_dir = tmp_path / "out"
    fb = FixtureBuilder(db_path)
    n = 32
    for i in range(n):
        chat_id = i
        fb.add_dm_chat(chat_id=chat_id, contact_id=f"+1555222{i:04d}")
        fb.add_message(chat_id=chat_id, ts_unix=BASE_TS + i * 1_000_000, is_from_me=True)
        if i % 3 != 0:
            fb.add_message(chat_id=chat_id, ts_unix=BASE_TS + i * 1_000_000 + 30.0, is_from_me=False)
    fb.commit_and_close()

    rc = esib.main(
        [
            "--chat-db", str(db_path),
            "--out-dir", str(out_dir),
            "--days", "9999",
            "--min-n", "30",
        ]
    )
    captured = capsys.readouterr()
    assert rc == 0, captured.err

    out_files = list(out_dir.glob("seth-initiation-baseline-*.json"))
    assert len(out_files) == 1
    data = json.loads(out_files[0].read_text())

    allowlist = {
        "generated_at", "days", "gap_hours", "fir_window_hours",
        "date_range", "n", "unanswered", "rate", "wilson_ci",
    }
    assert set(data.keys()) == allowlist

    # date_range must be derived from the constructed initiation
    # timestamps, not from --days=9999.
    import time as time_mod
    expected_first = time_mod.strftime("%Y-%m-%d", time_mod.gmtime(BASE_TS))
    expected_last = time_mod.strftime("%Y-%m-%d", time_mod.gmtime(BASE_TS + (n - 1) * 1_000_000))
    assert data["date_range"]["first"] == expected_first
    assert data["date_range"]["last"] == expected_last
    assert data["n"] == n

    # No phone number, contact identifier, or chat_id ever appears
    # anywhere in the serialized output.
    dumped = json.dumps(data)
    assert "+1555222" not in dumped
    assert "chat_id" not in dumped
    assert "contact" not in dumped
