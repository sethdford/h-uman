#!/usr/bin/env python3
"""
Phase H3b verifier — pins probe-collector behavior.

Tests:
  1. load_queue tolerates malformed lines (one bad line doesn't kill)
  2. save_queue is atomic (tmp + rename, no partial writes)
  3. find_next_pending picks oldest pending entry
  4. simulate_tick: pending → done, writes (K-1) pairs for letter reply
  5. simulate_tick: pending → done, writes K pairs for freetext reply
  6. simulate_tick: no --simulate-response → marks 'sent' (no pairs)
  7. simulate_tick: no pending entries → idempotent no-op
  8. simulate_tick: malformed entry (no candidates) → marks malformed
  9. End-to-end: H3 queues → collector tick → pairs written
 10. dispatch / poll modes return 3 (stub status) with informative output

Run: python3 scripts/test_m3_probe_collector.py
"""
from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
COLLECTOR = REPO_ROOT / "scripts" / "m3_probe_collector.py"
PROBE = REPO_ROOT / "scripts" / "m3_active_probe.py"


def _load():
    spec = importlib.util.spec_from_file_location("m3_probe_collector", COLLECTOR)
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


def _queue_entry(user_msg, cands, ts_ms=1000, handle="abc12345",
                   status="pending"):
    return {"ts_ms": ts_ms, "question": "🧠 [m3 probe]\nfake question",
            "user_message": user_msg, "candidates": cands,
            "handle": handle, "status": status}


def test_load_queue_tolerates_malformed_lines():
    print("\n--- test_load_queue_tolerates_malformed_lines ---")
    with tempfile.TemporaryDirectory() as d:
        q = Path(d) / "q.jsonl"
        q.write_text(
            json.dumps(_queue_entry("hi", ["a", "b"])) + "\n"
            "this is not json\n"
            + json.dumps(_queue_entry("yo", ["c", "d"])) + "\n"
            "{\"partial\": broken\n")
        entries = m.load_queue(q)
        _ok(f"loaded 2 valid entries (got {len(entries)})", len(entries) == 2)
        _ok("first entry preserved",
            entries and entries[0]["user_message"] == "hi")


def test_load_queue_missing_file():
    print("\n--- test_load_queue_missing_file ---")
    _ok("missing → []", m.load_queue(Path("/tmp/nonexistent_queue.jsonl")) == [])


def test_save_queue_is_atomic():
    print("\n--- test_save_queue_is_atomic ---")
    with tempfile.TemporaryDirectory() as d:
        q = Path(d) / "q.jsonl"
        entries = [_queue_entry("a", ["1", "2"]),
                    _queue_entry("b", ["3", "4"])]
        m.save_queue(q, entries)
        _ok("queue file exists after save", q.exists())
        _ok("tmp file removed (renamed in)",
            not q.with_suffix(q.suffix + ".tmp").exists())
        lines = [l for l in q.read_text().splitlines() if l.strip()]
        _ok(f"2 lines written (got {len(lines)})", len(lines) == 2)


def test_find_next_pending_picks_oldest():
    print("\n--- test_find_next_pending_picks_oldest ---")
    entries = [
        _queue_entry("newest", ["x"], ts_ms=3000),
        _queue_entry("oldest", ["y"], ts_ms=1000),
        _queue_entry("middle", ["z"], ts_ms=2000, status="done"),  # skipped
        _queue_entry("medium", ["w"], ts_ms=2500),
    ]
    idx = m.find_next_pending(entries)
    _ok(f"picked the oldest pending (got idx={idx})",
        idx == 1, f"entries[idx]={entries[idx] if idx is not None else None}")


def test_find_next_pending_none_when_empty():
    print("\n--- test_find_next_pending_none_when_empty ---")
    _ok("no entries → None", m.find_next_pending([]) is None)
    done_only = [_queue_entry("a", ["x"], status="done")]
    _ok("no pending → None", m.find_next_pending(done_only) is None)


def test_simulate_tick_letter_reply():
    print("\n--- test_simulate_tick_letter_reply ---")
    with tempfile.TemporaryDirectory() as d:
        q = Path(d) / "q.jsonl"
        p = Path(d) / "p.jsonl"
        entries = [_queue_entry("how was lunch?",
                                  ["yeah", "fine", "Yes, very enjoyable."])]
        m.save_queue(q, entries)
        n_pairs, remaining = m.simulate_tick(q, p, "B")
        _ok(f"wrote 2 pairs (got {n_pairs})", n_pairs == 2)
        _ok("0 pending remaining", remaining == 0)
        # Verify the queue was updated atomically
        updated = m.load_queue(q)
        _ok("queue entry status=done", updated[0]["status"] == "done")
        _ok("done_ts_ms recorded", "done_ts_ms" in updated[0])
        _ok("pairs_written=2", updated[0].get("pairs_written") == 2)
        # Verify the pairs file
        pair_lines = [json.loads(l) for l in p.read_text().splitlines() if l.strip()]
        _ok(f"pairs file has 2 entries (got {len(pair_lines)})", len(pair_lines) == 2)
        _ok("every pair chosen='fine' (letter B)",
            all(pp["chosen"] == "fine" for pp in pair_lines))


def test_simulate_tick_freetext_reply():
    print("\n--- test_simulate_tick_freetext_reply ---")
    with tempfile.TemporaryDirectory() as d:
        q = Path(d) / "q.jsonl"
        p = Path(d) / "p.jsonl"
        entries = [_queue_entry("dinner?",
                                  ["yeah", "sure", "Yes, that works."])]
        m.save_queue(q, entries)
        n_pairs, remaining = m.simulate_tick(q, p, "actually, tomorrow works better")
        _ok(f"wrote 3 pairs (got {n_pairs})", n_pairs == 3)
        pair_lines = [json.loads(l) for l in p.read_text().splitlines() if l.strip()]
        _ok("every chosen is the freetext",
            all(pp["chosen"] == "actually, tomorrow works better"
                for pp in pair_lines))
        _ok("source tag = active_probe_freetext",
            all(pp["_source"] == "active_probe_freetext" for pp in pair_lines))


def test_simulate_tick_no_response_marks_sent():
    print("\n--- test_simulate_tick_no_response_marks_sent ---")
    with tempfile.TemporaryDirectory() as d:
        q = Path(d) / "q.jsonl"
        p = Path(d) / "p.jsonl"
        m.save_queue(q, [_queue_entry("hi", ["a", "b"])])
        n_pairs, remaining = m.simulate_tick(q, p, None)
        _ok("no response → 0 pairs", n_pairs == 0)
        _ok("0 pending remaining (status moved to sent)", remaining == 0)
        updated = m.load_queue(q)
        _ok("status updated to sent", updated[0]["status"] == "sent")
        _ok("sent_ts_ms recorded", "sent_ts_ms" in updated[0])
        _ok("pairs file not created (no pairs)", not p.exists())


def test_simulate_tick_no_pending_is_noop():
    print("\n--- test_simulate_tick_no_pending_is_noop ---")
    with tempfile.TemporaryDirectory() as d:
        q = Path(d) / "q.jsonl"
        p = Path(d) / "p.jsonl"
        m.save_queue(q, [_queue_entry("done already", ["a"], status="done")])
        n_pairs, remaining = m.simulate_tick(q, p, "A")
        _ok("no pending → 0 pairs", n_pairs == 0)
        _ok("no pairs file written", not p.exists())


def test_simulate_tick_malformed_entry():
    print("\n--- test_simulate_tick_malformed_entry ---")
    with tempfile.TemporaryDirectory() as d:
        q = Path(d) / "q.jsonl"
        p = Path(d) / "p.jsonl"
        # Entry with no candidates → malformed
        m.save_queue(q, [_queue_entry("orphan", [])])
        n_pairs, _ = m.simulate_tick(q, p, "A")
        _ok("malformed entry → 0 pairs", n_pairs == 0)
        updated = m.load_queue(q)
        _ok("status updated to malformed",
            updated[0]["status"] == "malformed")


def test_end_to_end_h3_writes_collector_reads():
    """The full integration: H3 writes a queue entry, collector consumes
    it, pairs file appears. Proves the queue contract between the two
    scripts is intact."""
    print("\n--- test_end_to_end_h3_writes_collector_reads ---")
    with tempfile.TemporaryDirectory() as d:
        corpus = Path(d) / "corpus.jsonl"
        records = [
            {"handle": "alice", "role": "user",
             "content": "you free saturday?", "ts_ms": 1000},
        ]
        corpus.write_text("\n".join(json.dumps(r) for r in records) + "\n")

        queue = Path(d) / "q.jsonl"
        pairs = Path(d) / "p.jsonl"

        # Step 1: H3 queues the probe (no --simulate-delivery → real queue write)
        result = subprocess.run(
            [sys.executable, str(PROBE),
             "--corpus", str(corpus),
             "--queue", str(queue),
             "--pairs-out", str(pairs),
             "--delivery", "queue",
             "--gateway-url", "http://127.0.0.1:1"],
            capture_output=True, text=True, timeout=20,
            env={**os.environ})
        _ok(f"H3 exits 0 (rc={result.returncode})",
            result.returncode == 0, f"stderr:\n{result.stderr}")
        _ok("queue file written by H3", queue.exists())
        q_entries = m.load_queue(queue)
        _ok(f"queue has 1 entry (got {len(q_entries)})", len(q_entries) == 1)
        if q_entries:
            _ok("queue entry has user_message",
                q_entries[0].get("user_message") == "you free saturday?")
            _ok("queue entry has candidates list",
                isinstance(q_entries[0].get("candidates"), list) and
                len(q_entries[0]["candidates"]) >= 1)

        # Step 2: collector picks it up and converts a simulated reply
        result = subprocess.run(
            [sys.executable, str(COLLECTOR),
             "--queue", str(queue),
             "--pairs-out", str(pairs),
             "--mode", "simulate-tick",
             "--simulate-response", "A"],
            capture_output=True, text=True, timeout=10)
        _ok(f"collector exits 0 (rc={result.returncode})",
            result.returncode == 0, f"stderr:\n{result.stderr}")
        _ok("pairs file appeared", pairs.exists())
        if pairs.exists():
            lines = [l for l in pairs.read_text().splitlines() if l.strip()]
            _ok(f"pairs file has ≥1 entry (got {len(lines)})", len(lines) >= 1)
        # Queue entry should now be 'done'
        updated = m.load_queue(queue)
        _ok("queue entry now status=done",
            updated and updated[0]["status"] == "done")


def test_dispatch_mode_without_operator_returns_3():
    """Without --operator-handle (and without M3_OPERATOR_HANDLE env),
    dispatch is informational + exits 3 — never accidentally sends."""
    print("\n--- test_dispatch_mode_without_operator_returns_3 ---")
    with tempfile.TemporaryDirectory() as d:
        q = Path(d) / "q.jsonl"
        m.save_queue(q, [_queue_entry("hi", ["a", "b"])])
        # Strip M3_OPERATOR_HANDLE from env so the arg default is ""
        env = {k: v for k, v in os.environ.items() if k != "M3_OPERATOR_HANDLE"}
        result = subprocess.run(
            [sys.executable, str(COLLECTOR),
             "--queue", str(q),
             "--pairs-out", str(Path(d) / "p.jsonl"),
             "--mode", "dispatch"],
            capture_output=True, text=True, timeout=10, env=env)
        _ok(f"no operator → exit 3 (got {result.returncode})",
            result.returncode == 3)
        _ok("output asks for --operator-handle",
            "--operator-handle" in result.stdout,
            f"stdout: {result.stdout[:200]}")


def test_poll_mode_without_chat_db_returns_3():
    """Without a real chat.db on the operator path, poll exits 3."""
    print("\n--- test_poll_mode_without_chat_db_returns_3 ---")
    with tempfile.TemporaryDirectory() as d:
        q = Path(d) / "q.jsonl"
        e = _queue_entry("hi", ["a", "b"])
        e["status"] = "sent"
        e["sent_ts_ms"] = 1000
        m.save_queue(q, [e])
        env = {k: v for k, v in os.environ.items() if k != "M3_OPERATOR_HANDLE"}
        result = subprocess.run(
            [sys.executable, str(COLLECTOR),
             "--queue", str(q),
             "--pairs-out", str(Path(d) / "p.jsonl"),
             "--chat-db", str(Path(d) / "nonexistent-chat.db"),
             "--mode", "poll"],
            capture_output=True, text=True, timeout=10, env=env)
        _ok(f"no operator → exit 3 (got {result.returncode})",
            result.returncode == 3)


def test_send_via_messages_app_dry_run():
    """Dry-run mode never invokes osascript — returns True deterministically.
    Safety net for any default-mode dispatch invocation."""
    print("\n--- test_send_via_messages_app_dry_run ---")
    ok, detail = m.send_via_messages_app("+15555550123", "hello world",
                                           dry_run=True)
    _ok("dry-run returns ok=True", ok is True)
    _ok("dry-run detail is 'dry-run'", detail == "dry-run")


def test_send_via_messages_app_live_calls_osascript():
    """Live mode invokes subprocess.run(['osascript', ...]). Mocked."""
    print("\n--- test_send_via_messages_app_live_calls_osascript ---")
    from unittest import mock
    fake = mock.Mock(returncode=0, stderr="", stdout="")
    with mock.patch.object(m.subprocess, "run", return_value=fake) as run_mock:
        ok, detail = m.send_via_messages_app("+15555550123",
                                               'say "hi"\nand things',
                                               dry_run=False)
    _ok("live returns ok=True on rc=0", ok is True)
    _ok("live detail is 'sent'", detail == "sent")
    _ok("subprocess.run was called once", run_mock.call_count == 1)
    call = run_mock.call_args
    _ok("called osascript", call.args[0][0] == "osascript")
    script = call.args[0][2]
    _ok("script targets Messages app", 'tell application "Messages"' in script)
    _ok("script contains escaped handle",
        "+15555550123" in script)
    # Quotes in the body should be backslash-escaped
    _ok("script escapes embedded quotes",
        '\\"hi\\"' in script,
        f"script:\n{script}")
    # Newlines should be turned into \\n (AppleScript string escape)
    _ok("script escapes newline as backslash-n",
        "\\n" in script.replace("\\n", "")[-50:] or "\\n" in script)


def test_send_via_messages_app_handles_osascript_failure():
    """When osascript exits non-zero (e.g. Messages.app not authorized),
    the function returns (False, error_detail)."""
    print("\n--- test_send_via_messages_app_handles_osascript_failure ---")
    from unittest import mock
    fake = mock.Mock(returncode=1, stderr="not authorized to send messages",
                     stdout="")
    with mock.patch.object(m.subprocess, "run", return_value=fake):
        ok, detail = m.send_via_messages_app("+15555550123", "x",
                                               dry_run=False)
    _ok("rc=1 → ok=False", ok is False)
    _ok("error detail propagated",
        "not authorized" in detail, f"detail: {detail!r}")


def test_dispatch_mode_dry_run_marks_nothing():
    """In default dry-run mode (no --confirm-real-send), queue must
    be unchanged. Critical safety contract."""
    print("\n--- test_dispatch_mode_dry_run_marks_nothing ---")
    with tempfile.TemporaryDirectory() as d:
        q = Path(d) / "q.jsonl"
        entries = [_queue_entry("hi", ["a", "b"])]
        m.save_queue(q, entries)
        rc = m.dispatch_mode(q, operator_handle="+15555550123",
                              confirm_real_send=False)
        _ok("dry-run returns 0", rc == 0)
        updated = m.load_queue(q)
        _ok("queue entry status STILL 'pending' (dry-run)",
            updated[0]["status"] == "pending")
        _ok("no sent_ts_ms recorded in dry-run",
            "sent_ts_ms" not in updated[0])


def test_dispatch_mode_live_marks_sent():
    """With --confirm-real-send (mocked osascript), entries flip to
    'sent' with sent_ts_ms + operator_handle recorded."""
    print("\n--- test_dispatch_mode_live_marks_sent ---")
    from unittest import mock
    with tempfile.TemporaryDirectory() as d:
        q = Path(d) / "q.jsonl"
        m.save_queue(q, [_queue_entry("hi", ["a", "b"]),
                          _queue_entry("yo", ["c", "d"], ts_ms=2000)])
        fake = mock.Mock(returncode=0, stderr="", stdout="")
        with mock.patch.object(m.subprocess, "run", return_value=fake):
            rc = m.dispatch_mode(q, operator_handle="+15555550123",
                                  confirm_real_send=True)
        _ok("live dispatch returns 0", rc == 0)
        updated = m.load_queue(q)
        _ok("all entries flipped to sent",
            all(e["status"] == "sent" for e in updated))
        _ok("all entries recorded operator_handle",
            all(e.get("operator_handle") == "+15555550123" for e in updated))
        _ok("all entries recorded sent_ts_ms",
            all("sent_ts_ms" in e for e in updated))


def test_dispatch_mode_live_handles_partial_failure():
    """If some sends fail, return code is 1 (partial)."""
    print("\n--- test_dispatch_mode_live_handles_partial_failure ---")
    from unittest import mock
    with tempfile.TemporaryDirectory() as d:
        q = Path(d) / "q.jsonl"
        m.save_queue(q, [_queue_entry("hi", ["a", "b"]),
                          _queue_entry("yo", ["c", "d"], ts_ms=2000)])
        # First call OK, second call fails
        results = [mock.Mock(returncode=0, stderr="", stdout=""),
                    mock.Mock(returncode=1, stderr="permission denied",
                              stdout="")]
        with mock.patch.object(m.subprocess, "run", side_effect=results):
            rc = m.dispatch_mode(q, operator_handle="+15555550123",
                                  confirm_real_send=True)
        _ok("partial failure → exit 1", rc == 1)


def test_poll_chat_db_returns_replies_after_since():
    """Poll filters chat.db by handle + date > since_ms, EXCLUDES probe
    echoes (PROBE_HEADER prefix), returns in ts_ms ASC order."""
    print("\n--- test_poll_chat_db_returns_replies_after_since ---")
    import sqlite3 as _sql
    with tempfile.TemporaryDirectory() as d:
        cdb = Path(d) / "chat.db"
        c = _sql.connect(str(cdb))
        c.execute("CREATE TABLE message (ROWID INTEGER PRIMARY KEY, text TEXT, "
                  "is_from_me INTEGER, date INTEGER, handle_id INTEGER, "
                  "attributedBody BLOB)")
        c.execute("CREATE TABLE handle (ROWID INTEGER PRIMARY KEY, id TEXT)")
        c.execute("INSERT INTO handle(ROWID,id) VALUES (1,'+15555550999')")
        # Mac-epoch ns. epoch=2001-01-01 → unix 978307200 sec.
        # 1 sec  = 1e9 ns  → unix ms = (1e9/1e9 + 978307200) * 1000 = 978307201000
        # 100 ns = 0 sec   → unix ms = 978307200000
        # Insert 4 messages:
        #  - The probe itself (PROBE_HEADER prefix) — must be FILTERED OUT
        #  - The actual reply (after probe) — must be RETURNED
        #  - An earlier message (before sent_ts) — must be FILTERED
        #  - A message from a different handle — must be FILTERED
        # Apple ns = (unix_sec - 978307200) * 1e9
        def to_ns(unix_sec): return (unix_sec - 978307200) * 1_000_000_000
        # since_ms in test = unix 978307210000ms = unix_sec 978307210
        rows = [
            ("🧠 [m3 probe]\nWhich would you send?",
              1, to_ns(978307215), 1),               # the probe itself
            ("yeah, B sounds good",
              1, to_ns(978307220), 1),               # the reply
            ("earlier message that's irrelevant",
              0, to_ns(978307200), 1),               # before since
        ]
        c.execute("CREATE TABLE handle2 (ROWID INTEGER PRIMARY KEY, id TEXT)")
        # Add a second handle to confirm cross-handle filter
        c.execute("INSERT INTO handle(ROWID,id) VALUES (2,'other@example.com')")
        rows.append(("from a different person",
                      0, to_ns(978307225), 2))
        for r in rows:
            c.execute("INSERT INTO message(text,is_from_me,date,handle_id) "
                      "VALUES (?,?,?,?)", r)
        c.commit(); c.close()

        replies = m.poll_chat_db_for_replies(
            cdb, "+15555550999", since_ms=978307210_000)
        _ok(f"exactly 1 reply (got {len(replies)})", len(replies) == 1,
            f"replies={replies}")
        if replies:
            ts, text, _ = replies[0]
            _ok("reply text is the non-probe message",
                text == "yeah, B sounds good", f"got {text!r}")


def test_poll_mode_end_to_end_with_fixture():
    """Full flow: sent entry → poll → reply → pairs → done."""
    print("\n--- test_poll_mode_end_to_end_with_fixture ---")
    import sqlite3 as _sql
    with tempfile.TemporaryDirectory() as d:
        cdb = Path(d) / "chat.db"
        c = _sql.connect(str(cdb))
        c.execute("CREATE TABLE message (ROWID INTEGER PRIMARY KEY, text TEXT, "
                  "is_from_me INTEGER, date INTEGER, handle_id INTEGER, "
                  "attributedBody BLOB)")
        c.execute("CREATE TABLE handle (ROWID INTEGER PRIMARY KEY, id TEXT)")
        c.execute("INSERT INTO handle(ROWID,id) VALUES (1,'+15555550777')")
        # Reply: "B" at unix_sec=978307220
        c.execute("INSERT INTO message(text,is_from_me,date,handle_id) "
                  "VALUES (?,?,?,?)",
                  ("B", 1, (978307220 - 978307200) * 1_000_000_000, 1))
        c.commit(); c.close()

        q = Path(d) / "q.jsonl"
        p = Path(d) / "p.jsonl"
        sent_entry = _queue_entry("how was today?",
                                    ["yeah", "fine", "Yes, lovely."],
                                    ts_ms=978307210_000)
        sent_entry["status"] = "sent"
        sent_entry["sent_ts_ms"] = 978307210_000
        sent_entry["operator_handle"] = "+15555550777"
        m.save_queue(q, [sent_entry])
        rc = m.poll_mode(q, p, cdb, "+15555550777")
        _ok(f"poll_mode returns 0 (got {rc})", rc == 0)
        # Pair file should have 2 entries (3 candidates, letter B → 2 pairs)
        if p.exists():
            lines = [l for l in p.read_text().splitlines() if l.strip()]
            _ok(f"2 pairs written (got {len(lines)})", len(lines) == 2)
        else:
            _ok("pair file exists", False, "missing")
        updated = m.load_queue(q)
        _ok("queue entry status=done", updated[0]["status"] == "done")
        _ok("operator_reply recorded",
            updated[0].get("operator_reply") == "B")


def test_poll_chat_db_corrupt_soft_fails():
    """Same FDA-revoked / corrupt failure mode as H1's iMessage extractor."""
    print("\n--- test_poll_chat_db_corrupt_soft_fails ---")
    with tempfile.TemporaryDirectory() as d:
        bad = Path(d) / "chat.db"
        bad.write_bytes(b"not a sqlite file\n" * 100)
        out = m.poll_chat_db_for_replies(bad, "+15555550123", since_ms=1)
        _ok("corrupt db → []", out == [])

    out = m.poll_chat_db_for_replies(Path("/tmp/missing-chat.db"),
                                       "+15555550123", since_ms=1)
    _ok("missing db → []", out == [])
    # Empty handle short-circuits before opening
    out = m.poll_chat_db_for_replies(Path("/dev/null"), "", since_ms=1)
    _ok("empty handle → []", out == [])


def main():
    print("M3 probe collector (H3b) verifier")
    test_load_queue_tolerates_malformed_lines()
    test_load_queue_missing_file()
    test_save_queue_is_atomic()
    test_find_next_pending_picks_oldest()
    test_find_next_pending_none_when_empty()
    test_simulate_tick_letter_reply()
    test_simulate_tick_freetext_reply()
    test_simulate_tick_no_response_marks_sent()
    test_simulate_tick_no_pending_is_noop()
    test_simulate_tick_malformed_entry()
    test_end_to_end_h3_writes_collector_reads()
    test_dispatch_mode_without_operator_returns_3()
    test_poll_mode_without_chat_db_returns_3()
    test_send_via_messages_app_dry_run()
    test_send_via_messages_app_live_calls_osascript()
    test_send_via_messages_app_handles_osascript_failure()
    test_dispatch_mode_dry_run_marks_nothing()
    test_dispatch_mode_live_marks_sent()
    test_dispatch_mode_live_handles_partial_failure()
    test_poll_chat_db_returns_replies_after_since()
    test_poll_mode_end_to_end_with_fixture()
    test_poll_chat_db_corrupt_soft_fails()
    print(f"\n--- Results: {_PASS} passed, {_FAIL} failed ---")
    return 0 if _FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
