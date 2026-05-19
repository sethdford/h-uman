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


def test_dispatch_mode_stub_returns_3():
    print("\n--- test_dispatch_mode_stub_returns_3 ---")
    with tempfile.TemporaryDirectory() as d:
        q = Path(d) / "q.jsonl"
        m.save_queue(q, [_queue_entry("hi", ["a", "b"])])
        result = subprocess.run(
            [sys.executable, str(COLLECTOR),
             "--queue", str(q),
             "--pairs-out", str(Path(d) / "p.jsonl"),
             "--mode", "dispatch"],
            capture_output=True, text=True, timeout=10)
        _ok(f"dispatch stub exits 3 (got {result.returncode})",
            result.returncode == 3)
        _ok("dispatch output mentions STUB",
            "STUB" in result.stdout)


def test_poll_mode_stub_returns_3():
    print("\n--- test_poll_mode_stub_returns_3 ---")
    with tempfile.TemporaryDirectory() as d:
        q = Path(d) / "q.jsonl"
        # Mark one entry as 'sent' so poll has work to "do"
        e = _queue_entry("hi", ["a", "b"])
        e["status"] = "sent"
        m.save_queue(q, [e])
        result = subprocess.run(
            [sys.executable, str(COLLECTOR),
             "--queue", str(q),
             "--pairs-out", str(Path(d) / "p.jsonl"),
             "--mode", "poll"],
            capture_output=True, text=True, timeout=10)
        _ok(f"poll stub exits 3 (got {result.returncode})",
            result.returncode == 3)


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
    test_dispatch_mode_stub_returns_3()
    test_poll_mode_stub_returns_3()
    print(f"\n--- Results: {_PASS} passed, {_FAIL} failed ---")
    return 0 if _FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
