#!/usr/bin/env python3
"""
Phase C3 verifier — pins the training_loop.py JSONL-driven path.

Tests four contracts that the M3 driver depends on:

  1. fnv1a_64 matches the C side exactly (cross-language hash agreement
     is the foundation of hash resolution; if the algorithms drift the
     resolver finds zero matches in production).
  2. parse_outcomes_jsonl tolerates blank lines + malformed lines
     without poisoning the batch.
  3. summarize_outcomes computes the right aggregates (count, time
     range, latency stats, model id set).
  4. resolve_hashes_against_db actually finds the matching prompts in
     a fixture DB, AND correctly reports unresolved counts.

The dry-run artifact contract (a real safetensors-shaped file with a
metadata block) is verified in step 5.

Run:
  python3 scripts/test_m3_train_from_outcomes.py

Exit codes:
  0 — all assertions passed
  1 — at least one failure (printed inline)
"""
from __future__ import annotations

import importlib.util
import json
import sqlite3
import struct
import sys
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
TRAINING_LOOP = REPO_ROOT / "scripts" / "training_loop.py"


def _load_module():
    """Load training_loop.py as a module without invoking main()."""
    spec = importlib.util.spec_from_file_location("training_loop", TRAINING_LOOP)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


tl = _load_module()


_PASS = 0
_FAIL = 0


def _ok(name: str, cond: bool, detail: str = "") -> None:
    global _PASS, _FAIL
    if cond:
        _PASS += 1
        print(f"  PASS  {name}")
    else:
        _FAIL += 1
        print(f"  FAIL  {name}  {detail}")


def test_fnv1a_64_matches_known_vectors():
    """FNV-1a 64-bit reference vectors. These are STANDARD test vectors
    you can find in any FNV implementation, plus a custom Seth-flavored
    string so we'd notice if the implementation drifted.

    The C side's `hu_m3_outcome_hash_bytes` in src/ml/m3_frontier_adapter.c
    uses the same constants (0xcbf29ce484222325 offset basis, 0x100000001b3
    prime, and the "0 → 1" sentinel). If THIS test passes, hash resolution
    against C-produced outcomes will work."""
    print("\n--- test_fnv1a_64_matches_known_vectors ---")
    # Empty input → 0 (sentinel for "no value", documented in C side)
    _ok("empty bytes → 0", tl.fnv1a_64(b"") == 0)
    # Single byte 'a' — well-known FNV-1a 64-bit vector
    _ok("'a' → 0xaf63dc4c8601ec8c",
        tl.fnv1a_64(b"a") == 0xaf63dc4c8601ec8c,
        f"got {hex(tl.fnv1a_64(b'a'))}")
    # 'foobar' — another well-known vector
    _ok("'foobar' → 0x85944171f73967e8",
        tl.fnv1a_64(b"foobar") == 0x85944171f73967e8,
        f"got {hex(tl.fnv1a_64(b'foobar'))}")
    # Determinism — same input twice MUST be the same output
    h1 = tl.fnv1a_64(b"some prompt that an agent might see")
    h2 = tl.fnv1a_64(b"some prompt that an agent might see")
    _ok("determinism: same input → same hash", h1 == h2)
    # Different input → different output (with overwhelming probability)
    h3 = tl.fnv1a_64(b"some prompt that an agent might see!")
    _ok("different input → different hash", h1 != h3)


def test_parse_jsonl_tolerates_malformed_lines():
    print("\n--- test_parse_jsonl_tolerates_malformed_lines ---")
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "outcomes.jsonl"
        p.write_text(
            '{"t":1, "ph":100, "g":1}\n'
            '\n'  # blank line
            'not json at all\n'  # malformed
            '{"t":2, "ph":101, "g":1}\n'
            '   \n'  # whitespace-only
            '{"t":3, "ph":102, "g":2}\n'
        )
        outcomes = tl.parse_outcomes_jsonl(p)
        _ok(f"parsed 3 valid lines (got {len(outcomes)})", len(outcomes) == 3)
        _ok("first outcome has expected shape",
            outcomes[0]["ph"] == 100 and outcomes[0]["g"] == 1)


def test_summarize_outcomes_produces_correct_aggregates():
    print("\n--- test_summarize_outcomes_produces_correct_aggregates ---")
    outcomes = [
        {"t": 1000, "l": 100, "pt": 10, "ct": 20, "m": 1, "a": 0, "g": 1, "k": 1},
        {"t": 2000, "l": 200, "pt": 12, "ct": 22, "m": 1, "a": 0, "g": 1, "k": 1},
        {"t": 3000, "l": 300, "pt": 14, "ct": 24, "m": 2, "a": 0, "g": 2, "k": 2},
    ]
    s = tl.summarize_outcomes(outcomes)
    _ok("count = 3", s["count"] == 3)
    _ok("ts_min = 1000", s["ts_min"] == 1000)
    _ok("ts_max = 3000", s["ts_max"] == 3000)
    _ok("latency avg = 200", s["latency_avg_ms"] == 200)
    _ok("prompt_tokens_total = 36", s["prompt_tokens_total"] == 36)
    _ok("model_ids = [1, 2]", s["model_ids"] == [1, 2])
    _ok("guards = {1:2, 2:1}", s["guards"] == {1: 2, 2: 1})


def test_resolve_hashes_against_db_finds_matches():
    print("\n--- test_resolve_hashes_against_db_finds_matches ---")
    with tempfile.TemporaryDirectory() as d:
        db_path = Path(d) / "memory.db"
        # Build a fixture DB matching the production schema exactly.
        conn = sqlite3.connect(str(db_path))
        conn.execute(
            "CREATE TABLE messages("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "session_id TEXT NOT NULL,"
            "role TEXT NOT NULL,"
            "content TEXT NOT NULL,"
            "created_at TEXT DEFAULT(datetime('now')))"
        )
        prompts = [
            ("user", "What's the weather today?"),
            ("assistant", "Sunny, 72 degrees."),
            ("user", "Tell me a joke."),
            ("assistant", "Why did the C struct fail therapy? Lifetime issues."),
            ("user", "How do I read a man page?"),
        ]
        for role, content in prompts:
            conn.execute("INSERT INTO messages(session_id, role, content) VALUES (?, ?, ?)",
                         ("s1", role, content))
        conn.commit()
        conn.close()

        # Build outcomes from the prompts the C side WOULD have hashed.
        # Mixing real (known-to-resolve) and synthetic (no-match) hashes
        # exercises both the resolved + skipped paths.
        resolvable_text = "Tell me a joke."
        unresolvable_hash = 0xdeadbeefcafe1234
        outcomes = [
            {"t": 1, "ph": tl.fnv1a_64(resolvable_text.encode()),
             "rh": tl.fnv1a_64(b"Why did the C struct fail therapy? Lifetime issues."),
             "g": 1, "m": 1, "a": 0},
            {"t": 2, "ph": tl.fnv1a_64(b"What's the weather today?"),
             "rh": tl.fnv1a_64(b"Sunny, 72 degrees."), "g": 1, "m": 1, "a": 0},
            {"t": 3, "ph": unresolvable_hash, "rh": 0, "g": 1, "m": 1, "a": 0},
        ]

        resolved, skipped = tl.resolve_hashes_against_db(outcomes, db_path)
        _ok(f"resolved 2 of 3 outcomes (got {len(resolved)})", len(resolved) == 2)
        _ok("skipped 1 unresolvable hash", skipped == 1)
        _ok("resolved prompt text matches",
            any(r["prompt_text"] == resolvable_text for r in resolved))
        _ok("resolved response text matches",
            any(r["response_text"]
                and "Lifetime issues" in r["response_text"]
                for r in resolved))


def test_dry_run_writes_real_safetensors_header():
    print("\n--- test_dry_run_writes_real_safetensors_header ---")
    with tempfile.TemporaryDirectory() as d:
        adapter_out = Path(d) / "adapter.safetensors"
        summary = tl.summarize_outcomes([
            {"t": 1000, "l": 150, "pt": 10, "ct": 20, "m": 1, "a": 0, "g": 1, "k": 1},
            {"t": 2000, "l": 200, "pt": 12, "ct": 22, "m": 1, "a": 0, "g": 1, "k": 1},
        ])
        tl.write_dry_run_adapter(adapter_out, summary, resolved_count=2, skipped_count=0)
        _ok("adapter file exists", adapter_out.exists())
        _ok("adapter has nonzero size", adapter_out.stat().st_size > 0)

        # Parse as a safetensors file would: first 8 LE bytes = header length,
        # next `header_length` bytes = JSON header.
        with open(adapter_out, "rb") as f:
            header_len_bytes = f.read(8)
            header_len = struct.unpack("<Q", header_len_bytes)[0]
            header_json = json.loads(f.read(header_len).decode("utf-8"))
        meta = header_json.get("__metadata__", {})
        _ok("metadata block present", "__metadata__" in header_json)
        _ok("metadata has produced_by tag",
            meta.get("produced_by", "").endswith("--source-jsonl"))
        _ok("metadata records outcome_count=2", meta.get("outcome_count") == "2")
        _ok("metadata records resolved_count=2", meta.get("resolved_count") == "2")
        _ok("metadata records model_ids", meta.get("model_ids") == "1")


def main():
    print("M3 train-from-outcomes (C3) verifier")
    test_fnv1a_64_matches_known_vectors()
    test_parse_jsonl_tolerates_malformed_lines()
    test_summarize_outcomes_produces_correct_aggregates()
    test_resolve_hashes_against_db_finds_matches()
    test_dry_run_writes_real_safetensors_header()
    print(f"\n--- Results: {_PASS} passed, {_FAIL} failed ---")
    return 0 if _FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
