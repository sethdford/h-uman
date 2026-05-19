#!/usr/bin/env python3
"""
Phase H3 verifier — pins active-learning probe behavior.

Tests:
  1. response_to_pairs: single letter ('A'/'B'/'C') → (K-1) pairs
  2. response_to_pairs: letter with paren/space → still parses
  3. response_to_pairs: free text → all candidates become rejected
  4. response_to_pairs: empty/whitespace → no pairs
  5. format_probe_question includes the sentinel header + all letters
  6. deliver_probe simulate mode → returns 'simulated', no file written
  7. deliver_probe queue mode → appends one JSONL entry to queue file
  8. pick_eligible_user_message prefers unanswered messages
  9. pick_eligible_user_message returns None for empty corpus
 10. End-to-end --simulate-delivery --simulate-response=A writes pairs

Run: python3 scripts/test_m3_active_probe.py
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
PROBE = REPO_ROOT / "scripts" / "m3_active_probe.py"


def _load():
    spec = importlib.util.spec_from_file_location("m3_active_probe", PROBE)
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


def test_response_letter_picks_one_chosen():
    print("\n--- test_response_letter_picks_one_chosen ---")
    cands = ["yeah", "sure thing", "Absolutely, let me look into it"]
    pairs = m.response_to_pairs("how's it going", cands, "B")
    _ok(f"3-candidate 'B' → 2 pairs (got {len(pairs)})", len(pairs) == 2)
    if pairs:
        _ok("chosen is candidate B for every pair",
            all(p["chosen"] == cands[1] for p in pairs))
        _ok("rejecteds are the other two candidates",
            {p["rejected"] for p in pairs} == {cands[0], cands[2]})
        _ok("source tagged active_probe",
            all(p["_source"] == "active_probe" for p in pairs))


def test_response_letter_with_trailing_chars():
    print("\n--- test_response_letter_with_trailing_chars ---")
    cands = ["one", "two", "three"]
    # "A)" and "A " and "A." should all parse as letter A
    for resp in ("A)", "A ", "A.", "A"):
        pairs = m.response_to_pairs("hi", cands, resp)
        _ok(f"{resp!r} → 2 pairs", len(pairs) == 2)
        if pairs:
            _ok(f"{resp!r} → chosen=one", all(p["chosen"] == "one" for p in pairs))


def test_response_freetext_all_candidates_rejected():
    print("\n--- test_response_freetext_all_candidates_rejected ---")
    cands = ["yeah", "sure", "ok"]
    pairs = m.response_to_pairs("hi", cands, "lol nope, can't make it")
    _ok(f"freetext → 3 pairs (got {len(pairs)})", len(pairs) == 3)
    if pairs:
        _ok("every pair has chosen=freetext",
            all(p["chosen"] == "lol nope, can't make it" for p in pairs))
        _ok("every candidate appears as rejected exactly once",
            {p["rejected"] for p in pairs} == set(cands))
        _ok("source tagged active_probe_freetext",
            all(p["_source"] == "active_probe_freetext" for p in pairs))


def test_response_empty_no_pairs():
    print("\n--- test_response_empty_no_pairs ---")
    _ok("empty → []", m.response_to_pairs("hi", ["a", "b"], "") == [])
    _ok("whitespace-only → []",
        m.response_to_pairs("hi", ["a", "b"], "   \n  ") == [])
    _ok("None → []", m.response_to_pairs("hi", ["a", "b"], None) == [])


def test_format_probe_includes_sentinel_and_letters():
    print("\n--- test_format_probe_includes_sentinel_and_letters ---")
    q = m.format_probe_question("how was lunch", ["yeah good", "fine", "meh"],
                                  "abc12345")
    _ok("question contains PROBE_HEADER sentinel",
        m.PROBE_HEADER in q)
    _ok("question contains 'A)' label", "A)" in q)
    _ok("question contains 'B)' label", "B)" in q)
    _ok("question contains 'C)' label", "C)" in q)
    _ok("question quotes the user's message",
        "how was lunch" in q)
    _ok("question references the handle",
        "abc12345" in q)
    _ok("question prompts for letter or freetext",
        "letter" in q.lower())


def test_deliver_simulate_returns_simulated():
    print("\n--- test_deliver_simulate_returns_simulated ---")
    with tempfile.TemporaryDirectory() as d:
        q = Path(d) / "queue.jsonl"
        status = m.deliver_probe("hello\nworld", "simulate", q)
        _ok("simulate mode returns 'simulated'", status == "simulated")
        _ok("simulate mode writes no queue file",
            not q.exists())


def test_deliver_queue_appends_entry():
    print("\n--- test_deliver_queue_appends_entry ---")
    with tempfile.TemporaryDirectory() as d:
        q = Path(d) / "queue.jsonl"
        status = m.deliver_probe("first probe", "queue", q)
        _ok("queue mode returns 'queued'", status == "queued")
        _ok("queue file exists", q.exists())
        m.deliver_probe("second probe", "queue", q)
        lines = [l for l in q.read_text().splitlines() if l.strip()]
        _ok(f"queue has 2 entries (got {len(lines)})", len(lines) == 2)
        if len(lines) == 2:
            entries = [json.loads(l) for l in lines]
            _ok("each entry has ts_ms + question + status",
                all({"ts_ms", "question", "status"} <= set(e) for e in entries))
            _ok("first entry quotes 'first probe'",
                "first probe" in entries[0]["question"])
            _ok("each entry status=pending",
                all(e["status"] == "pending" for e in entries))


def test_pick_eligible_prefers_unanswered():
    print("\n--- test_pick_eligible_prefers_unanswered ---")
    import random as _r
    with tempfile.TemporaryDirectory() as d:
        corpus = Path(d) / "corpus.jsonl"
        DAY = 24 * 3600 * 1000
        records = [
            # Alice — answered (won't be picked as primary candidate)
            {"handle": "alice", "role": "user",
             "content": "hi", "ts_ms": 1000},
            {"handle": "alice", "role": "assistant",
             "content": "hey", "ts_ms": 1000 + 60_000},
            # Bob — UNANSWERED (Seth never replied)
            {"handle": "bob", "role": "user",
             "content": "unanswered question here", "ts_ms": 2000},
            # Carol — answered but >24h later (counts as unanswered)
            {"handle": "carol", "role": "user",
             "content": "stale", "ts_ms": 3000},
            {"handle": "carol", "role": "assistant",
             "content": "late reply", "ts_ms": 3000 + 2 * DAY},
        ]
        corpus.write_text("\n".join(json.dumps(r) for r in records) + "\n")
        # Use a fixed seed so the choice is deterministic
        picked = m.pick_eligible_user_message(corpus, _r.Random(42))
        _ok(f"picked something (got {picked})", picked is not None)
        if picked:
            # Should be from bob or carol (both unanswered within 24h)
            _ok(f"picked an unanswered msg (handle={picked.get('handle')})",
                picked.get("handle") in {"bob", "carol"})
            _ok("picked role=user",
                picked.get("role") == "user")


def test_pick_eligible_empty_corpus_returns_none():
    print("\n--- test_pick_eligible_empty_corpus_returns_none ---")
    import random as _r
    with tempfile.TemporaryDirectory() as d:
        corpus = Path(d) / "empty.jsonl"
        corpus.write_text("")
        _ok("empty corpus → None",
            m.pick_eligible_user_message(corpus, _r.Random()) is None)
    _ok("nonexistent corpus → None",
        m.pick_eligible_user_message(Path("/tmp/nope.jsonl"),
                                       __import__("random").Random()) is None)


def test_synthetic_candidates_shape():
    print("\n--- test_synthetic_candidates_shape ---")
    cs = m.synthetic_candidates("hi", 3)
    _ok(f"3 candidates (got {len(cs)})", len(cs) == 3)
    _ok("all distinct", len(set(cs)) == 3)
    # Should span a length spread (terse → verbose)
    _ok("length spans terse→verbose",
        len(cs[0]) < len(cs[1]) < len(cs[2]),
        f"lengths: {[len(c) for c in cs]}")


def test_end_to_end_simulate_writes_pair():
    print("\n--- test_end_to_end_simulate_writes_pair ---")
    with tempfile.TemporaryDirectory() as d:
        corpus = Path(d) / "corpus.jsonl"
        records = [
            {"handle": "dave", "role": "user",
             "content": "you free for coffee?", "ts_ms": 1000},
        ]
        corpus.write_text("\n".join(json.dumps(r) for r in records) + "\n")

        out = Path(d) / "pairs.jsonl"
        queue = Path(d) / "queue.jsonl"
        result = subprocess.run(
            [sys.executable, str(PROBE),
             "--corpus", str(corpus),
             "--pairs-out", str(out),
             "--queue", str(queue),
             "--simulate-delivery",
             "--simulate-response=A",
             "--gateway-url", "http://127.0.0.1:1"],  # unreachable → synthetic
            capture_output=True, text=True, timeout=20,
            env={**os.environ, "HUMAN_GATEWAY_URL": "http://127.0.0.1:1"})
        _ok(f"probe exits 0 (rc={result.returncode})",
            result.returncode == 0,
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
        if out.exists():
            lines = [l for l in out.read_text().splitlines() if l.strip()]
            _ok(f"pairs written (got {len(lines)})", len(lines) == 2)
            if lines:
                recs = [json.loads(l) for l in lines]
                _ok("every record has prompt/chosen/rejected",
                    all({"prompt", "chosen", "rejected"} <= set(r) for r in recs))
                _ok("every prompt is the user message",
                    all(r["prompt"] == "you free for coffee?" for r in recs))
                _ok("every chosen is the first synthetic candidate (terse 'yeah')",
                    all(r["chosen"] == "yeah" for r in recs))
        else:
            _ok("pairs file written", False, "out file missing")


def test_end_to_end_simulate_freetext():
    print("\n--- test_end_to_end_simulate_freetext ---")
    with tempfile.TemporaryDirectory() as d:
        corpus = Path(d) / "corpus.jsonl"
        records = [
            {"handle": "eve", "role": "user",
             "content": "lunch?", "ts_ms": 1000},
        ]
        corpus.write_text("\n".join(json.dumps(r) for r in records) + "\n")

        out = Path(d) / "pairs.jsonl"
        queue = Path(d) / "queue.jsonl"
        result = subprocess.run(
            [sys.executable, str(PROBE),
             "--corpus", str(corpus),
             "--pairs-out", str(out),
             "--queue", str(queue),
             "--simulate-delivery",
             "--simulate-response=can't, busy then. Tomorrow?",
             "--gateway-url", "http://127.0.0.1:1"],
            capture_output=True, text=True, timeout=20,
            env={**os.environ, "HUMAN_GATEWAY_URL": "http://127.0.0.1:1"})
        _ok(f"probe exits 0 (rc={result.returncode})",
            result.returncode == 0,
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
        if out.exists():
            recs = [json.loads(l) for l in out.read_text().splitlines() if l.strip()]
            _ok(f"freetext → 3 pairs (got {len(recs)})", len(recs) == 3)
            if recs:
                _ok("freetext source tag",
                    all(r["_source"] == "active_probe_freetext" for r in recs))


def test_empty_corpus_exits_2():
    print("\n--- test_empty_corpus_exits_2 ---")
    with tempfile.TemporaryDirectory() as d:
        corpus = Path(d) / "empty.jsonl"
        corpus.write_text("")
        out = Path(d) / "pairs.jsonl"
        queue = Path(d) / "queue.jsonl"
        result = subprocess.run(
            [sys.executable, str(PROBE),
             "--corpus", str(corpus),
             "--pairs-out", str(out),
             "--queue", str(queue),
             "--simulate-delivery"],
            capture_output=True, text=True, timeout=10)
        _ok(f"empty corpus → exit 2 (got {result.returncode})",
            result.returncode == 2)


def main():
    print("M3 active probe (H3) verifier")
    test_response_letter_picks_one_chosen()
    test_response_letter_with_trailing_chars()
    test_response_freetext_all_candidates_rejected()
    test_response_empty_no_pairs()
    test_format_probe_includes_sentinel_and_letters()
    test_deliver_simulate_returns_simulated()
    test_deliver_queue_appends_entry()
    test_pick_eligible_prefers_unanswered()
    test_pick_eligible_empty_corpus_returns_none()
    test_synthetic_candidates_shape()
    test_end_to_end_simulate_writes_pair()
    test_end_to_end_simulate_freetext()
    test_empty_corpus_exits_2()
    print(f"\n--- Results: {_PASS} passed, {_FAIL} failed ---")
    return 0 if _FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
