#!/usr/bin/env python3
"""
M3 holdout split verifier — pins the train/holdout split contract.

Tests:
  1. split_corpus returns (train, holdout) pairs from preceding user msg
  2. Same-handle constraint — no cross-contact pairing
  3. Deterministic split with the same seed → identical output
  4. Different seeds → different splits (sanity)
  5. Tiny buckets (≤2 pairs) go to train, never holdout (would leave train empty)
  6. End-to-end CLI: writes both files with right shape

Run: python3 scripts/test_m3_holdout_split.py
"""
from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SPLIT = REPO_ROOT / "scripts" / "m3_holdout_split.py"


def _load():
    spec = importlib.util.spec_from_file_location("m3_holdout_split", SPLIT)
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


def _write_corpus(path, records):
    path.write_text("\n".join(json.dumps(r) for r in records) + "\n")


def test_pairs_have_preceding_user_context():
    print("\n--- test_pairs_have_preceding_user_context ---")
    with tempfile.TemporaryDirectory() as d:
        corpus = Path(d) / "c.jsonl"
        # alice: user → Seth → user → Seth (2 pairs)
        # bob:   user → user → Seth (1 pair)
        records = [
            {"handle": "alice", "role": "user", "content": "hi", "ts_ms": 1},
            {"handle": "alice", "role": "assistant", "content": "hey", "ts_ms": 2},
            {"handle": "alice", "role": "user", "content": "how r u", "ts_ms": 3},
            {"handle": "alice", "role": "assistant", "content": "good", "ts_ms": 4},
            {"handle": "bob",   "role": "user", "content": "yo", "ts_ms": 5},
            {"handle": "bob",   "role": "user", "content": "u there?", "ts_ms": 6},
            {"handle": "bob",   "role": "assistant", "content": "yep", "ts_ms": 7},
        ]
        _write_corpus(corpus, records)
        train, hold = m.split_corpus(corpus, holdout_frac=0.0, seed=1)
        all_pairs = train + hold
        _ok(f"3 pairs derived total (got {len(all_pairs)})",
            len(all_pairs) == 3)
        # Alice has two pairs; bob has one
        h_counts = {}
        for p in all_pairs:
            h_counts[p["handle"]] = h_counts.get(p["handle"], 0) + 1
        _ok(f"alice: 2 pairs (got {h_counts.get('alice', 0)})",
            h_counts.get("alice") == 2)
        _ok(f"bob: 1 pair (got {h_counts.get('bob', 0)})",
            h_counts.get("bob") == 1)


def test_same_contact_constraint_no_cross_handle():
    print("\n--- test_same_contact_constraint_no_cross_handle ---")
    with tempfile.TemporaryDirectory() as d:
        corpus = Path(d) / "c.jsonl"
        # alice writes first; bob's Seth-reply should NOT pair with
        # alice's message (different handle)
        records = [
            {"handle": "alice", "role": "user", "content": "alice-msg", "ts_ms": 1},
            {"handle": "bob",   "role": "assistant", "content": "bob-reply", "ts_ms": 2},
        ]
        _write_corpus(corpus, records)
        train, hold = m.split_corpus(corpus, holdout_frac=0.0, seed=1)
        all_pairs = train + hold
        _ok(f"0 pairs (got {len(all_pairs)})", len(all_pairs) == 0,
            f"bob has no preceding user from himself; alice msg is not for bob")


def test_split_is_seeded_deterministic():
    print("\n--- test_split_is_seeded_deterministic ---")
    with tempfile.TemporaryDirectory() as d:
        corpus = Path(d) / "c.jsonl"
        records = []
        for i in range(20):
            records.append({"handle": "alice", "role": "user",
                            "content": f"q{i}", "ts_ms": 2*i})
            records.append({"handle": "alice", "role": "assistant",
                            "content": f"a{i}", "ts_ms": 2*i+1})
        _write_corpus(corpus, records)
        train1, hold1 = m.split_corpus(corpus, holdout_frac=0.3, seed=42)
        train2, hold2 = m.split_corpus(corpus, holdout_frac=0.3, seed=42)
        _ok("same seed → identical holdout",
            [p["reference"] for p in hold1] == [p["reference"] for p in hold2])
        _ok("same seed → identical train",
            [p["reference"] for p in train1] == [p["reference"] for p in train2])


def test_split_different_seeds_differ():
    print("\n--- test_split_different_seeds_differ ---")
    with tempfile.TemporaryDirectory() as d:
        corpus = Path(d) / "c.jsonl"
        records = []
        for i in range(20):
            records.append({"handle": "alice", "role": "user",
                            "content": f"q{i}", "ts_ms": 2*i})
            records.append({"handle": "alice", "role": "assistant",
                            "content": f"a{i}", "ts_ms": 2*i+1})
        _write_corpus(corpus, records)
        _, hold1 = m.split_corpus(corpus, holdout_frac=0.3, seed=1)
        _, hold2 = m.split_corpus(corpus, holdout_frac=0.3, seed=999)
        # With shuffling, two different seeds should produce different
        # holdout sets (could rarely match but very unlikely on 20 items)
        _ok("different seeds → different holdout",
            [p["reference"] for p in hold1] != [p["reference"] for p in hold2])


def test_tiny_bucket_goes_to_train():
    print("\n--- test_tiny_bucket_goes_to_train ---")
    with tempfile.TemporaryDirectory() as d:
        corpus = Path(d) / "c.jsonl"
        # alice: 1 pair (tiny)
        # bob:   10 pairs (large)
        records = [
            {"handle": "alice", "role": "user", "content": "hi", "ts_ms": 1},
            {"handle": "alice", "role": "assistant", "content": "hey", "ts_ms": 2},
        ]
        for i in range(10):
            records.append({"handle": "bob", "role": "user",
                            "content": f"q{i}", "ts_ms": 10+2*i})
            records.append({"handle": "bob", "role": "assistant",
                            "content": f"a{i}", "ts_ms": 11+2*i})
        _write_corpus(corpus, records)
        train, hold = m.split_corpus(corpus, holdout_frac=0.3, seed=1)
        alice_in_hold = [p for p in hold if p["handle"] == "alice"]
        _ok("alice's tiny bucket NOT in holdout",
            len(alice_in_hold) == 0,
            f"alice has only 2 pairs, can't spare for holdout")
        alice_in_train = [p for p in train if p["handle"] == "alice"]
        _ok("alice's pair preserved in train",
            len(alice_in_train) == 1)


def test_end_to_end_cli():
    print("\n--- test_end_to_end_cli ---")
    with tempfile.TemporaryDirectory() as d:
        corpus = Path(d) / "c.jsonl"
        records = []
        for h in ("alice", "bob"):
            for i in range(8):
                records.append({"handle": h, "role": "user",
                                "content": f"{h}-q{i}", "ts_ms": 100+2*i})
                records.append({"handle": h, "role": "assistant",
                                "content": f"{h}-a{i}", "ts_ms": 101+2*i})
        _write_corpus(corpus, records)
        train_out = Path(d) / "train.jsonl"
        hold_out = Path(d) / "hold.jsonl"
        result = subprocess.run(
            [sys.executable, str(SPLIT),
             "--corpus", str(corpus),
             "--train-out", str(train_out),
             "--holdout-out", str(hold_out),
             "--holdout-frac", "0.25",
             "--seed", "7"],
            capture_output=True, text=True, timeout=10)
        _ok(f"exits 0 (rc={result.returncode})", result.returncode == 0,
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
        if result.returncode == 0:
            train_lines = [l for l in train_out.read_text().splitlines()
                           if l.strip()]
            hold_lines = [l for l in hold_out.read_text().splitlines()
                          if l.strip()]
            _ok(f"train + holdout = 16 (got {len(train_lines)} + {len(hold_lines)})",
                len(train_lines) + len(hold_lines) == 16)
            _ok("each record has {handle, prompt, reference}",
                all({"handle", "prompt", "reference"} <= set(json.loads(l))
                    for l in train_lines + hold_lines))


def main():
    print("M3 holdout split verifier")
    test_pairs_have_preceding_user_context()
    test_same_contact_constraint_no_cross_handle()
    test_split_is_seeded_deterministic()
    test_split_different_seeds_differ()
    test_tiny_bucket_goes_to_train()
    test_end_to_end_cli()
    print(f"\n--- Results: {_PASS} passed, {_FAIL} failed ---")
    return 0 if _FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
