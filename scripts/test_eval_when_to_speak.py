#!/usr/bin/env python3
"""Hermetic tests for scripts/eval_when_to_speak.py (US-4).

Covers:
  - AC-4.1: `proactive_decisions` is preferred over the fallback source when
    it has in-window rows; the fallback path still works (regression guard)
    when the table is absent or empty-in-window.
  - AC-4.2: the refusal contract (exit non-zero, "REFUSE: insufficient n",
    no output file written) still holds after the join/dedup rewrite.
  - AC-4.3/join-dedup: resolve_decision_events() collapses a proposal row
    (trigger='init_proposer_llm') and its outcome row (trigger=
    'proactive_send') sharing the same (contact, ts) into ONE logical
    event, in all three shapes the design calls out -- confirmed send,
    failed send, fired-and-dropped-pre-send -- and compute_fir() feeds only
    confirmed-delivery events into the rate while counting the other two
    shapes as separate diagnostic counters, never silently.
  - MIR's has_send presence check is unaffected by the same duplicate rows
    that would otherwise double-count FIR.
  - AC-4.4: --compare-baseline's three states (unavailable/available x2
    verdict directions), including the sprint evidence-JSON's nested
    "primary_run.rate" shape (the actual committed US-3 artifact shape).
  - FIR_WINDOW_HOURS stays a real importable module constant and the
    argparse default does not drift from it.

Builds synthetic sqlite files shaped like chat.db and memory.db under
tmp_path -- NEVER touches the real ~/Library/Messages/chat.db or
~/.human/memory.db. No network, no subprocess, no live daemon.

Run with: pytest scripts/test_eval_when_to_speak.py -v
"""
import json
import sqlite3
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
sys.path.insert(0, str(Path(__file__).parent / "blind_ab"))

import eval_when_to_speak as ews  # noqa: E402
from score import wilson  # noqa: E402

APPLE_EPOCH = 978307200
BASE_TS = 1_700_000_000.0  # arbitrary fixed anchor; tests pass --days large
                            # enough that it never falls outside the lookback
                            # window regardless of when the suite runs.
BIG_DAYS = 999999


def unix_to_apple_ns(ts_unix):
    return int(round((ts_unix - APPLE_EPOCH) * 1_000_000_000))


# ── chat.db fixture (only the columns load_dm_messages() touches) ───────


def make_chat_db(path):
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
            item_type INTEGER DEFAULT 0
        );
        CREATE TABLE chat_message_join (chat_id INTEGER, message_id INTEGER);
        """
    )
    con.commit()
    con.close()


class ChatDbBuilder:
    def __init__(self, path):
        make_chat_db(path)
        self.con = sqlite3.connect(str(path))
        self._msg_id = 0
        self._handle_id = 0

    def add_dm_chat(self, chat_id, contact_id):
        self._handle_id += 1
        h = self._handle_id
        self.con.execute("INSERT INTO handle (ROWID, id) VALUES (?, ?)", (h, contact_id))
        self.con.execute(
            "INSERT INTO chat_handle_join (chat_id, handle_id) VALUES (?, ?)", (chat_id, h)
        )
        return h

    def add_message(self, chat_id, ts_unix, is_from_me):
        self._msg_id += 1
        mid = self._msg_id
        self.con.execute(
            "INSERT INTO message (ROWID, date, is_from_me) VALUES (?, ?, ?)",
            (mid, unix_to_apple_ns(ts_unix), 1 if is_from_me else 0),
        )
        self.con.execute(
            "INSERT INTO chat_message_join (chat_id, message_id) VALUES (?, ?)", (chat_id, mid)
        )
        return mid

    def commit_and_close(self):
        self.con.commit()
        self.con.close()


# ── memory.db fixtures (proactive_decisions / fallback tables) ──────────


def make_memory_db_with_proactive_decisions(path, rows, also_create_empty_table=True):
    """rows: list of (ts, contact, trigger, decision, sent[, reason])
    tuples. If `also_create_empty_table` and rows is empty, the table is
    still created (empty) -- exercises load_decisions()'s "table exists but
    empty in this window" fallthrough."""
    con = sqlite3.connect(str(path))
    con.execute(
        """
        CREATE TABLE proactive_decisions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            ts INTEGER NOT NULL,
            contact TEXT,
            trigger TEXT NOT NULL,
            decision TEXT NOT NULL,
            reason TEXT,
            sent INTEGER NOT NULL DEFAULT 0,
            message_ref TEXT
        )
        """
    )
    for row in rows:
        ts, contact, trigger, decision, sent = row[:5]
        reason = row[5] if len(row) > 5 else None
        con.execute(
            "INSERT INTO proactive_decisions (ts, contact, trigger, decision, sent, reason) "
            "VALUES (?, ?, ?, ?, ?, ?)",
            (ts, contact, trigger, decision, 1 if sent else 0, reason),
        )
    con.commit()
    con.close()


def make_memory_db_fallback_only(path, proactive_sends=None, production_outcomes=None):
    con = sqlite3.connect(str(path))
    if proactive_sends is not None:
        con.execute("CREATE TABLE proactive_sends (sent_timestamp INTEGER, contact TEXT)")
        for ts, contact in proactive_sends:
            con.execute(
                "INSERT INTO proactive_sends (sent_timestamp, contact) VALUES (?, ?)", (ts, contact)
            )
    if production_outcomes is not None:
        con.execute("CREATE TABLE production_outcomes (send_timestamp INTEGER, target TEXT)")
        for ts, target in production_outcomes:
            con.execute(
                "INSERT INTO production_outcomes (send_timestamp, target) VALUES (?, ?)", (ts, target)
            )
    con.commit()
    con.close()


def make_memory_db_empty(path):
    con = sqlite3.connect(str(path))
    con.commit()
    con.close()


# ── pure-function tests: resolve_decision_events() ──────────────────────


def test_join_dedup_confirmed_send():
    """A proposal row + a matching outcome row (same contact, ts) resolve
    to exactly ONE eligible event, not two."""
    rows = [
        {"ts": 100, "contact": "A", "decision": "send", "sent": False, "trigger": "init_proposer_llm"},
        {"ts": 100, "contact": "A", "decision": "send", "sent": True, "trigger": "proactive_send"},
    ]
    events = ews.resolve_decision_events(rows)
    assert len(events) == 1
    e = events[0]
    assert e["contact"] == "A" and e["ts"] == 100
    assert e["resolved_decision"] == "send"
    assert e["resolved_sent"] is True
    assert e["dropped_pre_send"] is False
    assert e["send_failed"] is False


def test_join_dedup_failed_send():
    """Proposal fired (decision=send) but the outcome row records a
    channel-send failure -- resolves to zero FIR-eligible events and
    increments send_failed, NOT dropped_pre_send. F1: resolved_decision is
    preserved as 'send' (the proposal's intent) so MIR counts it
    symmetrically with dropped_pre_send cases."""
    rows = [
        {"ts": 200, "contact": "B", "decision": "send", "sent": False, "trigger": "init_proposer_llm"},
        {
            "ts": 200, "contact": "B", "decision": "decline", "sent": False,
            "trigger": "proactive_send", "reason": "send_failed",
        },
    ]
    events = ews.resolve_decision_events(rows)
    assert len(events) == 1
    e = events[0]
    assert e["resolved_decision"] == "send"  # F1: preserved for MIR symmetry
    assert e["resolved_sent"] is False
    assert e["dropped_pre_send"] is False
    assert e["send_failed"] is True


def test_join_dedup_fired_and_dropped_pre_send():
    """Only the proposal row exists (no outcome row at that (contact, ts))
    -- the daemon never attempted to send. Zero FIR-eligible events,
    increments dropped_pre_send, NOT send_failed."""
    rows = [
        {"ts": 300, "contact": "C", "decision": "send", "sent": False, "trigger": "init_proposer_llm"},
    ]
    events = ews.resolve_decision_events(rows)
    assert len(events) == 1
    e = events[0]
    assert e["resolved_decision"] == "send"
    assert e["resolved_sent"] is False
    assert e["dropped_pre_send"] is True
    assert e["send_failed"] is False


def test_proposer_plain_decline_is_neither_dropped_nor_failed():
    """A proposer row that itself declined (never FIRED) is not a
    dropped_pre_send case -- that label is reserved for a FIRED
    (decision=='send') proposal with no outcome row."""
    rows = [
        {"ts": 400, "contact": "D", "decision": "decline", "sent": False, "trigger": "init_proposer_llm"},
    ]
    events = ews.resolve_decision_events(rows)
    assert len(events) == 1
    e = events[0]
    assert e["resolved_decision"] == "decline"
    assert e["dropped_pre_send"] is False
    assert e["send_failed"] is False


def test_fallback_rows_pass_through_unresolved():
    """Fallback-sourced rows (trigger='fallback', tagged by
    load_decisions()) have no join ambiguity -- each is its own resolved
    event, decision/sent taken directly from the row (pre-fix behavior for
    the fallback source, unchanged)."""
    rows = [
        {"ts": 500, "contact": "E", "decision": "send", "sent": True, "trigger": "fallback"},
    ]
    events = ews.resolve_decision_events(rows)
    assert len(events) == 1
    e = events[0]
    assert e["resolved_decision"] == "send"
    assert e["resolved_sent"] is True
    assert e["dropped_pre_send"] is False
    assert e["send_failed"] is False


def test_distinct_contacts_and_timestamps_never_collapse():
    """Groups are keyed on (contact, ts) -- rows for different contacts, or
    the same contact at different timestamps, must never merge."""
    rows = [
        {"ts": 100, "contact": "A", "decision": "send", "sent": True, "trigger": "proactive_send"},
        {"ts": 100, "contact": "B", "decision": "send", "sent": True, "trigger": "proactive_send"},
        {"ts": 200, "contact": "A", "decision": "decline", "sent": False, "trigger": "init_proposer_llm"},
    ]
    events = ews.resolve_decision_events(rows)
    assert len(events) == 3


# ── pure-function tests: compute_fir() over resolved events ──────────────


def test_compute_fir_counts_dropped_and_failed_separately_from_eligible():
    events = [
        {"contact": "A", "ts": 1000, "resolved_decision": "send", "resolved_sent": True,
         "dropped_pre_send": False, "send_failed": False},
        {"contact": "B", "ts": 2000, "resolved_decision": "send", "resolved_sent": False,
         "dropped_pre_send": True, "send_failed": False},
        {"contact": "C", "ts": 3000, "resolved_decision": "decline", "resolved_sent": False,
         "dropped_pre_send": False, "send_failed": True},
    ]
    fir = ews.compute_fir(
        events, seth_sends_by_chat={}, contact_chat_map={}, contact_replies_by_contact={},
        seth_before_secs=6 * 3600.0, contact_after_secs=24 * 3600.0,
    )
    assert fir["n"] == 1
    assert fir["dropped_pre_send"] == 1
    assert fir["send_failed"] == 1
    assert fir["false_interruptions"] == 1  # no reply configured -> A is a false interruption
    assert fir["rate"] == 1.0
    expected_p, expected_lo, expected_hi = wilson(1, 1)
    assert fir["wilson_ci"] == [expected_lo, expected_hi]


def test_compute_fir_seth_already_engaged_excludes_event():
    """A confirmed send whose contact Seth had already messaged shortly
    before is excluded entirely -- not eligible, not a false interruption."""
    events = [
        {"contact": "A", "ts": 1000, "resolved_decision": "send", "resolved_sent": True,
         "dropped_pre_send": False, "send_failed": False},
    ]
    fir = ews.compute_fir(
        events,
        seth_sends_by_chat={"chat-1": [999.0]},  # Seth sent 1s before the daemon's send
        contact_chat_map={"A": "chat-1"},
        contact_replies_by_contact={},
        seth_before_secs=6 * 3600.0,
        contact_after_secs=24 * 3600.0,
    )
    assert fir["n"] == 0
    assert fir["dropped_pre_send"] == 0
    assert fir["send_failed"] == 0
    assert fir["rate"] is None
    assert fir["wilson_ci"] == [0.0, 0.0]


def test_compute_fir_got_reply_is_not_a_false_interruption():
    events = [
        {"contact": "A", "ts": 1000, "resolved_decision": "send", "resolved_sent": True,
         "dropped_pre_send": False, "send_failed": False},
    ]
    fir = ews.compute_fir(
        events,
        seth_sends_by_chat={},
        contact_chat_map={"A": "chat-1"},
        contact_replies_by_contact={"A": [1500.0]},  # replied within the 24h window
        seth_before_secs=6 * 3600.0,
        contact_after_secs=24 * 3600.0,
    )
    assert fir["n"] == 1
    assert fir["false_interruptions"] == 0
    assert fir["rate"] == 0.0


# ── pure-function tests: MIR unaffected by pre-fix duplicate rows ────────


def test_mir_presence_check_unaffected_by_duplicate_source_rows():
    """Two raw rows (a proposal + its outcome) sharing (contact, ts), once
    resolved and deduped, still yield exactly the correct has_send presence
    for compute_mir -- proving the any() contract holds through the
    SELECT-gains-trigger + resolve/dedup rewrite (design test case 6)."""
    raw_rows = [
        {"ts": 1030, "contact": "A", "decision": "send", "sent": False, "trigger": "init_proposer_llm"},
        {"ts": 1030, "contact": "A", "decision": "send", "sent": True, "trigger": "proactive_send"},
    ]
    events = ews.resolve_decision_events(raw_rows)
    assert len(events) == 1  # deduped, not two raw rows leaking through

    decisions_by_contact = ews.decisions_by_contact_index(events)
    positives = [{"chat_id": 1, "contact": "A", "ts": 1000.0, "positive": True}]
    mir = ews.compute_mir(
        positives, decisions_by_contact, before_secs=3600.0, after_secs=24 * 3600.0
    )
    assert mir["n"] == 1
    assert mir["missed"] == 0  # the deduped send at ts=1030 is within [1000-3600, 1000+86400]
    assert mir["rate"] == 0.0
    expected_p, expected_lo, expected_hi = wilson(0, 1)
    assert mir["wilson_ci"] == [expected_lo, expected_hi]


def test_mir_missed_when_no_send_nearby():
    positives = [{"chat_id": 1, "contact": "Z", "ts": 1000.0, "positive": True}]
    mir = ews.compute_mir(positives, decisions_by_contact={}, before_secs=3600.0, after_secs=3600.0)
    assert mir["n"] == 1
    assert mir["missed"] == 1
    assert mir["rate"] == 1.0


def test_mir_zero_positives_returns_none_rate_not_a_crash():
    mir = ews.compute_mir([], decisions_by_contact={}, before_secs=3600.0, after_secs=3600.0)
    assert mir == {"n": 0, "missed": 0, "rate": None, "wilson_ci": [0.0, 0.0]}


def test_mir_send_failed_and_dropped_pre_send_symmetric():
    """F1: FIRED-then-failed (send_failed=True) and FIRED-then-dropped
    (dropped_pre_send=True) are both FIRED cases where the policy noticed
    and tried to engage. They should be counted symmetrically by MIR --
    both treated as presence of a 'send' decision. Verifies that
    resolved_decision='send' is preserved for send_failed cases."""
    # One contact with FIRED-then-failed
    rows_failed = [
        {"ts": 100, "contact": "A", "decision": "send", "sent": False, "trigger": "init_proposer_llm"},
        {"ts": 100, "contact": "A", "decision": "decline", "sent": False, "trigger": "proactive_send"},
    ]
    # One contact with FIRED-then-dropped
    rows_dropped = [
        {"ts": 200, "contact": "B", "decision": "send", "sent": False, "trigger": "init_proposer_llm"},
    ]
    all_rows = rows_failed + rows_dropped

    events = ews.resolve_decision_events(all_rows)
    assert len(events) == 2
    failed_event = [e for e in events if e["contact"] == "A"][0]
    dropped_event = [e for e in events if e["contact"] == "B"][0]

    # Both should have resolved_decision='send' for MIR symmetry
    assert failed_event["resolved_decision"] == "send"
    assert failed_event["send_failed"] is True
    assert dropped_event["resolved_decision"] == "send"
    assert dropped_event["dropped_pre_send"] is True

    # Build decisions_by_contact index (what MIR uses)
    decisions_by_contact = ews.decisions_by_contact_index(events)
    positives = [
        {"chat_id": 1, "contact": "A", "ts": 50.0, "positive": True},
        {"chat_id": 2, "contact": "B", "ts": 150.0, "positive": True},
    ]

    mir = ews.compute_mir(positives, decisions_by_contact, before_secs=3600.0, after_secs=3600.0)
    # Both positives should find a nearby 'send' decision, so neither is missed
    assert mir["n"] == 2
    assert mir["missed"] == 0
    assert mir["rate"] == 0.0


# ── pure-function tests: --compare-baseline (AC-4.4) ─────────────────────


def test_compare_to_baseline_unavailable_when_flag_omitted():
    result = ews.compare_to_baseline(0.5, None)
    assert result == {"available": False, "reason": "US-3 baseline not provided or US-3 refused"}


def test_compare_to_baseline_unavailable_missing_path_names_it(tmp_path):
    missing = tmp_path / "does-not-exist.json"
    result = ews.compare_to_baseline(0.5, str(missing))
    assert result["available"] is False
    assert str(missing) in result["reason"]


def test_compare_to_baseline_unreadable_path_does_not_crash(tmp_path):
    bad = tmp_path / "corrupt.json"
    bad.write_text("{not valid json")
    result = ews.compare_to_baseline(0.5, str(bad))
    assert result["available"] is False
    assert str(bad) in result["reason"]


def test_compare_to_baseline_fir_le_baseline_flat_shape(tmp_path):
    """Flat US-3 script-output shape (top-level 'rate')."""
    baseline = tmp_path / "baseline.json"
    baseline.write_text(json.dumps({"rate": 0.5, "n": 50}))
    result = ews.compare_to_baseline(0.3, str(baseline))
    assert result == {
        "available": True,
        "fir_rate": 0.3,
        "seth_baseline_rate": 0.5,
        "verdict": "fir_le_baseline",
    }


def test_compare_to_baseline_fir_gt_baseline_nested_primary_run_shape(tmp_path):
    """Nested sprint-evidence shape -- the actual committed
    sprints/.../evidence/us3-seth-initiation-baseline.json layout, where
    the run lives under 'primary_run', not at the top level."""
    baseline = tmp_path / "us3-seth-initiation-baseline.json"
    baseline.write_text(json.dumps({"story": "US-3", "primary_run": {"rate": 0.32, "n": 50}}))
    result = ews.compare_to_baseline(0.6, str(baseline))
    assert result["available"] is True
    assert result["fir_rate"] == 0.6
    assert result["seth_baseline_rate"] == 0.32
    assert result["verdict"] == "fir_gt_baseline"


def test_compare_to_baseline_refused_baseline_has_no_rate(tmp_path):
    """US-3 refused: no 'rate' anywhere (top-level or nested)."""
    baseline = tmp_path / "refused.json"
    baseline.write_text(json.dumps({"story": "US-3", "refused": True}))
    result = ews.compare_to_baseline(0.4, str(baseline))
    assert result["available"] is False
    assert "no usable" in result["reason"]


# ── FIR_WINDOW_HOURS constant parity (design test case 9) ────────────────


def test_fir_window_hours_is_a_real_module_constant():
    assert ews.FIR_WINDOW_HOURS == 24.0


def test_argparse_default_does_not_drift_from_fir_window_hours_constant(tmp_path, capsys):
    """Runs main() end-to-end WITHOUT --fir-window-hours and checks the
    written JSON's windows.fir_window_hours equals the module constant --
    a behavioral check, not just a source-text grep, so it fails if the
    argparse default is ever hardcoded back to a literal."""
    db_path, out_dir, _contact = _build_minimal_success_fixture(tmp_path)
    rc = ews.main([
        "--chat-db", str(db_path["chat_db"]),
        "--memory-db", str(db_path["memory_db"]),
        "--out-dir", str(out_dir),
        "--days", str(BIG_DAYS),
        "--min-n", "1",
    ])
    captured = capsys.readouterr()
    assert rc == 0, captured.err
    out_files = list(out_dir.glob("when-to-speak-*.json"))
    assert len(out_files) == 1
    data = json.loads(out_files[0].read_text())
    assert data["windows"]["fir_window_hours"] == ews.FIR_WINDOW_HOURS


# ── end-to-end fixture builder + AC-4.1 / AC-4.3 / privacy tests ─────────


def _build_minimal_success_fixture(tmp_path):
    """Builds a chat.db + memory.db pair that clears main()'s "no DM
    messages" / structural checks with --min-n 1, exercising:
      - contact A: a positive moment (Seth replies within the reply
        window) PLUS a confirmed proactive send (proposal+outcome pair,
        same (contact, ts)) that is NOT excluded by seth-already-engaged
        and gets no contact reply -> one false interruption.
      - contact B: a FIRED-but-dropped-pre-send proposal (proposal row
        only, no outcome row).
      - contact C: a FIRED-but-failed-to-send proposal (proposal row +
        outcome row recording a channel-send failure).
    Returns ({"chat_db": path, "memory_db": path}, out_dir, contact_a_id).
    """
    chat_db_path = tmp_path / "chat.db"
    memory_db_path = tmp_path / "memory.db"
    out_dir = tmp_path / "out"

    contact_a = "+15550001111"
    fb = ChatDbBuilder(chat_db_path)
    fb.add_dm_chat(chat_id=1, contact_id=contact_a)
    fb.add_message(chat_id=1, ts_unix=BASE_TS, is_from_me=False)          # inbound
    fb.add_message(chat_id=1, ts_unix=BASE_TS + 60.0, is_from_me=True)    # Seth replies fast
    fb.commit_and_close()

    make_memory_db_with_proactive_decisions(
        memory_db_path,
        rows=[
            # contact A: confirmed send (proposal + outcome, same ts).
            (BASE_TS + 30.0, contact_a, "init_proposer_llm", "send", False),
            (BASE_TS + 30.0, contact_a, "proactive_send", "send", True),
            # contact B: fired, dropped pre-send (proposal only).
            (BASE_TS + 1000.0, "+15550002222", "init_proposer_llm", "send", False),
            # contact C: fired, failed to send (proposal + failing outcome).
            (BASE_TS + 2000.0, "+15550003333", "init_proposer_llm", "send", False),
            (BASE_TS + 2000.0, "+15550003333", "proactive_send", "decline", False, "send_failed"),
        ],
    )
    return {"chat_db": chat_db_path, "memory_db": memory_db_path}, out_dir, contact_a


def test_source_is_proactive_decisions_and_shape_is_complete(tmp_path, capsys):
    """AC-4.1 (source line reads 'proactive_decisions') + AC-4.3 (the
    committed JSON carries MIR/FIR with the new diagnostic counters,
    known_limitations, and the comparison block) in one real run."""
    db_paths, out_dir, contact_a = _build_minimal_success_fixture(tmp_path)
    baseline_path = tmp_path / "baseline.json"
    baseline_path.write_text(json.dumps({"rate": 0.5, "n": 50}))

    rc = ews.main([
        "--chat-db", str(db_paths["chat_db"]),
        "--memory-db", str(db_paths["memory_db"]),
        "--out-dir", str(out_dir),
        "--days", str(BIG_DAYS),
        "--min-n", "1",
        "--compare-baseline", str(baseline_path),
    ])
    captured = capsys.readouterr()
    assert rc == 0, captured.err

    out_files = list(out_dir.glob("when-to-speak-*.json"))
    assert len(out_files) == 1
    data = json.loads(out_files[0].read_text())

    # AC-4.1
    assert data["decisions_source"] == "proactive_decisions"

    # AC-4.3 shape
    assert data["mir"]["n"] == 1
    assert data["mir"]["missed"] == 0  # contact A's confirmed send is nearby -> not missed
    assert data["fir"]["n"] == 1       # only contact A's confirmed-delivery event is eligible
    assert data["fir"]["false_interruptions"] == 1  # no reply after the send
    assert data["fir"]["dropped_pre_send"] == 1     # contact B
    assert data["fir"]["send_failed"] == 1           # contact C
    assert "wilson_ci" in data["mir"] and "wilson_ci" in data["fir"]
    assert data["inputs"]["resolved_events"] == 3  # one per contact, deduped from 5 raw rows
    assert data["inputs"]["decisions_loaded"] == 5

    # AC-4.4
    assert data["comparison"]["available"] is True
    assert data["comparison"]["verdict"] == "fir_gt_baseline"  # fir.rate=1.0 > baseline 0.5

    # Risk-analysis known_limitations disclosed verbatim.
    assert data["known_limitations"] == ews.KNOWN_LIMITATIONS

    # Privacy: no contact identifier or phone number anywhere in the output.
    dumped = json.dumps(data)
    assert contact_a not in dumped
    assert "+15550002222" not in dumped
    assert "+15550003333" not in dumped


def test_fallback_used_when_proactive_decisions_table_absent(tmp_path, capsys):
    """Regression guard: with no proactive_decisions table at all, the
    pre-existing fallback path still runs (decisions_source == 'fallback')."""
    chat_db_path = tmp_path / "chat.db"
    memory_db_path = tmp_path / "memory.db"
    out_dir = tmp_path / "out"

    fb = ChatDbBuilder(chat_db_path)
    fb.add_dm_chat(chat_id=1, contact_id="+15550009999")
    fb.add_message(chat_id=1, ts_unix=BASE_TS, is_from_me=True)
    fb.commit_and_close()

    make_memory_db_fallback_only(
        memory_db_path, proactive_sends=[(BASE_TS + 10.0, "+15550009999")]
    )

    rc = ews.main([
        "--chat-db", str(chat_db_path),
        "--memory-db", str(memory_db_path),
        "--out-dir", str(out_dir),
        "--days", str(BIG_DAYS),
        "--min-n", "0",
    ])
    captured = capsys.readouterr()
    assert rc == 0, captured.err
    out_files = list(out_dir.glob("when-to-speak-*.json"))
    assert len(out_files) == 1
    data = json.loads(out_files[0].read_text())
    assert data["decisions_source"] == "fallback"


def test_fallback_used_when_proactive_decisions_table_empty_in_window(tmp_path, capsys):
    """Regression guard for load_decisions()'s fallthrough comment: the
    table EXISTS but has zero rows -- must still fall back, not error or
    silently report n=0 as if that were the real proactive_decisions
    source."""
    chat_db_path = tmp_path / "chat.db"
    memory_db_path = tmp_path / "memory.db"
    out_dir = tmp_path / "out"

    fb = ChatDbBuilder(chat_db_path)
    fb.add_dm_chat(chat_id=1, contact_id="+15550008888")
    fb.add_message(chat_id=1, ts_unix=BASE_TS, is_from_me=True)
    fb.commit_and_close()

    # Create BOTH: an empty proactive_decisions table and a populated
    # fallback table, in one memory.db.
    con = sqlite3.connect(str(memory_db_path))
    con.execute(
        """
        CREATE TABLE proactive_decisions (
            id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER NOT NULL, contact TEXT,
            trigger TEXT NOT NULL, decision TEXT NOT NULL, reason TEXT,
            sent INTEGER NOT NULL DEFAULT 0, message_ref TEXT
        )
        """
    )
    con.execute("CREATE TABLE proactive_sends (sent_timestamp INTEGER, contact TEXT)")
    con.execute(
        "INSERT INTO proactive_sends (sent_timestamp, contact) VALUES (?, ?)",
        (BASE_TS + 10.0, "+15550008888"),
    )
    con.commit()
    con.close()

    rc = ews.main([
        "--chat-db", str(chat_db_path),
        "--memory-db", str(memory_db_path),
        "--out-dir", str(out_dir),
        "--days", str(BIG_DAYS),
        "--min-n", "0",
    ])
    captured = capsys.readouterr()
    assert rc == 0, captured.err
    data = json.loads(next(out_dir.glob("when-to-speak-*.json")).read_text())
    assert data["decisions_source"] == "fallback"


# ── AC-4.2: refusal contract ──────────────────────────────────────────────


def test_refuses_below_min_n_writes_nothing(tmp_path, capsys):
    """A single positive moment and a single decision (n=1 << default
    min_n=30) must refuse: exit non-zero, stderr contains REFUSE, and NO
    output file is written -- the file-not-written half of AC-4.2
    (.claude/rules/no-number-without-a-measurement.md: refuse means
    writing nothing, not writing a zero)."""
    chat_db_path = tmp_path / "chat.db"
    memory_db_path = tmp_path / "memory.db"
    out_dir = tmp_path / "out"

    fb = ChatDbBuilder(chat_db_path)
    fb.add_dm_chat(chat_id=1, contact_id="+15550007777")
    fb.add_message(chat_id=1, ts_unix=BASE_TS, is_from_me=False)
    fb.add_message(chat_id=1, ts_unix=BASE_TS + 60.0, is_from_me=True)
    fb.commit_and_close()

    make_memory_db_with_proactive_decisions(
        memory_db_path,
        rows=[
            (BASE_TS + 30.0, "+15550007777", "init_proposer_llm", "send", False),
            (BASE_TS + 30.0, "+15550007777", "proactive_send", "send", True),
        ],
    )

    rc = ews.main([
        "--chat-db", str(chat_db_path),
        "--memory-db", str(memory_db_path),
        "--out-dir", str(out_dir),
        "--days", str(BIG_DAYS),
        # --min-n omitted -> default 30, well above this fixture's n=1.
    ])
    captured = capsys.readouterr()

    assert rc != 0
    assert "REFUSE: insufficient n" in captured.err
    assert not out_dir.exists() or list(out_dir.iterdir()) == []


def test_chat_db_missing_refuses(tmp_path, capsys):
    rc = ews.main([
        "--chat-db", str(tmp_path / "nope.db"),
        "--memory-db", str(tmp_path / "also-nope.db"),
        "--out-dir", str(tmp_path / "out"),
    ])
    captured = capsys.readouterr()
    assert rc != 0
    assert "REFUSE" in captured.err


def test_memory_db_missing_refuses(tmp_path, capsys):
    chat_db_path = tmp_path / "chat.db"
    fb = ChatDbBuilder(chat_db_path)
    fb.add_dm_chat(chat_id=1, contact_id="+15550006666")
    fb.add_message(chat_id=1, ts_unix=BASE_TS, is_from_me=True)
    fb.commit_and_close()

    rc = ews.main([
        "--chat-db", str(chat_db_path),
        "--memory-db", str(tmp_path / "no-memory-db-here.db"),
        "--out-dir", str(tmp_path / "out"),
    ])
    captured = capsys.readouterr()
    assert rc != 0
    assert "REFUSE" in captured.err


def test_refusal_prints_diagnostic_counters_on_stderr(tmp_path, capsys):
    """F2: On REFUSE due to insufficient n, the script must print
    diagnostic counters to stderr so they can be regenerated from
    stderr alone (.claude/rules/no-number-without-a-measurement.md)."""
    chat_db_path = tmp_path / "chat.db"
    memory_db_path = tmp_path / "memory.db"
    out_dir = tmp_path / "out"

    fb = ChatDbBuilder(chat_db_path)
    fb.add_dm_chat(chat_id=1, contact_id="+15550005555")
    fb.add_message(chat_id=1, ts_unix=BASE_TS, is_from_me=False)
    fb.add_message(chat_id=1, ts_unix=BASE_TS + 60.0, is_from_me=True)
    fb.commit_and_close()

    make_memory_db_with_proactive_decisions(
        memory_db_path,
        rows=[
            # One event to trigger refusal (n=1 << min_n=30)
            (BASE_TS + 30.0, "+15550005555", "init_proposer_llm", "send", False),
            (BASE_TS + 30.0, "+15550005555", "proactive_send", "send", True),
        ],
    )

    rc = ews.main([
        "--chat-db", str(chat_db_path),
        "--memory-db", str(memory_db_path),
        "--out-dir", str(out_dir),
        "--days", str(BIG_DAYS),
        # --min-n omitted -> default 30, triggers refusal
    ])
    captured = capsys.readouterr()

    assert rc != 0
    assert "REFUSE: insufficient n" in captured.err
    # F2: diagnostic counters must appear on stderr
    assert "resolved_events=" in captured.err
    assert "fir_n=" in captured.err
    assert "fir_dropped_pre_send=" in captured.err
    assert "fir_send_failed=" in captured.err
    assert "mir_n=" in captured.err
    # Verify specific values (1 resolved event, n=1 for both MIR and FIR)
    assert "resolved_events=1" in captured.err
    assert "fir_n=1" in captured.err
    assert "mir_n=1" in captured.err
