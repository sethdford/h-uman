#!/usr/bin/env python3
"""
Integration test for training_loop.py US-8 Phase C3 — `--source-jsonl` wiring.

Tests the flow:
  1. Parse JSONL outcomes from a temp file
  2. Resolve prompt hashes against a stub SQLite conversation DB
  3. Build an SFT batch from resolved outcomes
  4. Invoke mlx_lm.lora training (or dry-run if not available)
  5. Verify safetensors output exists and is well-formed

Fixtures:
  - Temp JSONL: 4 outcomes with hashes that match 4 DB rows
  - Temp SQLite DB: 4 message rows (2 user, 2 assistant)
  - Temp output adapter path
"""
from __future__ import annotations

import json
import sqlite3
import subprocess
import sys
import tempfile
from pathlib import Path

# Adjust path to find training_loop
SCRIPTS = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS.parent
sys.path.insert(0, str(SCRIPTS))

# Import from training_loop
from training_loop import (
    fnv1a_64,
    parse_outcomes_jsonl,
    resolve_hashes_against_db,
    write_sft_batch_jsonl,
    run_mlx_lora_training,
    summarize_outcomes,
    FNV_OFFSET_BASIS_64,
    FNV_PRIME_64,
    FNV_64_MOD,
)


def create_stub_db(db_path: Path) -> dict[str, int]:
    """Create a temporary SQLite DB with 4 test messages (2 user, 2 assistant).

    Returns a dict mapping message content to FNV-1a hash.
    """
    conn = sqlite3.connect(str(db_path))
    conn.execute("""
        CREATE TABLE messages (
            id INTEGER PRIMARY KEY,
            session_id TEXT,
            role TEXT,
            content BLOB,
            created_at INTEGER
        )
    """)

    # Create 4 test messages
    messages = [
        ("user", b"What's for dinner?"),
        ("assistant", b"I'm thinking tacos or pasta tonight."),
        ("user", b"Tell me a joke."),
        ("assistant", b"Why did the scarecrow win an award? He was outstanding in his field!"),
    ]

    content_to_hash = {}
    for role, content in messages:
        h = fnv1a_64(content)
        content_to_hash[content.decode("utf-8")] = h
        conn.execute(
            "INSERT INTO messages (role, content, created_at) VALUES (?, ?, ?)",
            (role, content, int(__import__("time").time() * 1000)),
        )

    conn.commit()
    conn.close()
    return content_to_hash


def create_outcomes_jsonl(jsonl_path: Path, content_hashes: dict[str, int]) -> None:
    """Create a temporary JSONL with 4 outcomes matching the DB hashes."""
    outcomes = [
        {
            "ph": content_hashes["What's for dinner?"],
            "rh": content_hashes["I'm thinking tacos or pasta tonight."],
            "t": 1716597849000,
            "l": 145,
            "pt": 12,
            "ct": 8,
            "m": 1,
            "a": 0,
            "g": 0,
        },
        {
            "ph": content_hashes["Tell me a joke."],
            "rh": content_hashes["Why did the scarecrow win an award? He was outstanding in his field!"],
            "t": 1716597950000,
            "l": 200,
            "pt": 8,
            "ct": 15,
            "m": 1,
            "a": 0,
            "g": 0,
        },
    ]

    with open(jsonl_path, "w") as f:
        for o in outcomes:
            f.write(json.dumps(o) + "\n")


def test_parsing():
    """Test AC-8.1: parse --source-jsonl flag + JSONL format."""
    print("\n=== Test 1: Parsing ===")
    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)
        db_path = tmpdir / "test.db"
        jsonl_path = tmpdir / "outcomes.jsonl"

        # Create fixtures
        content_hashes = create_stub_db(db_path)
        create_outcomes_jsonl(jsonl_path, content_hashes)

        # Parse
        outcomes = parse_outcomes_jsonl(jsonl_path)
        assert len(outcomes) == 2, f"Expected 2 outcomes, got {len(outcomes)}"
        print(f"  PASS: Parsed {len(outcomes)} outcomes")


def test_hash_resolution():
    """Test AC-8.2: resolve prompt hashes against conversation DB."""
    print("\n=== Test 2: Hash Resolution ===")
    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)
        db_path = tmpdir / "test.db"
        jsonl_path = tmpdir / "outcomes.jsonl"

        # Create fixtures
        content_hashes = create_stub_db(db_path)
        create_outcomes_jsonl(jsonl_path, content_hashes)

        # Parse and resolve
        outcomes = parse_outcomes_jsonl(jsonl_path)
        resolved, skipped = resolve_hashes_against_db(outcomes, db_path)

        assert len(resolved) == 2, f"Expected 2 resolved, got {len(resolved)}"
        assert skipped == 0, f"Expected 0 skipped, got {skipped}"
        assert resolved[0]["prompt_text"] == "What's for dinner?"
        assert resolved[0]["response_text"] == "I'm thinking tacos or pasta tonight."
        print(f"  PASS: Resolved {len(resolved)} hashes, skipped {skipped}")


def test_sft_batch():
    """Test AC-8.3: SFT batch writing."""
    print("\n=== Test 3: SFT Batch Writing ===")
    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)
        db_path = tmpdir / "test.db"
        jsonl_path = tmpdir / "outcomes.jsonl"

        # Create fixtures
        content_hashes = create_stub_db(db_path)
        create_outcomes_jsonl(jsonl_path, content_hashes)

        # Parse and resolve
        outcomes = parse_outcomes_jsonl(jsonl_path)
        resolved, _ = resolve_hashes_against_db(outcomes, db_path)

        # Write SFT batch
        sft_path = write_sft_batch_jsonl(resolved)
        sft_file = Path(sft_path)
        assert sft_file.exists(), f"SFT batch not created: {sft_path}"

        # Verify format
        with open(sft_file) as f:
            lines = f.readlines()
        assert len(lines) == 2, f"Expected 2 lines in SFT batch, got {len(lines)}"

        for line in lines:
            data = json.loads(line)
            assert "text" in data, f"Missing 'text' key: {data}"
            assert "\n" in data["text"], f"Expected newline separator: {data['text']}"

        print(f"  PASS: Created SFT batch with {len(lines)} samples")
        sft_file.unlink()


def test_dry_run_adapter():
    """Test AC-8.4 (dry-run mode): safetensors output exists."""
    print("\n=== Test 4: Dry-Run Adapter Output ===")
    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)
        db_path = tmpdir / "test.db"
        jsonl_path = tmpdir / "outcomes.jsonl"
        adapter_out = tmpdir / "adapter-out"

        # Create fixtures
        content_hashes = create_stub_db(db_path)
        create_outcomes_jsonl(jsonl_path, content_hashes)

        # Parse and resolve
        outcomes = parse_outcomes_jsonl(jsonl_path)
        resolved, skipped = resolve_hashes_against_db(outcomes, db_path)
        summary = summarize_outcomes(outcomes)

        # Import and call the dry-run writer
        from training_loop import write_dry_run_adapter
        write_dry_run_adapter(adapter_out, summary, len(resolved), skipped)

        assert adapter_out.exists(), f"Adapter output not created: {adapter_out}"
        size = adapter_out.stat().st_size
        assert size >= 8, f"Safetensors file too small: {size} bytes"
        print(f"  PASS: Dry-run adapter created ({size} bytes)")


def test_mlx_lora_training():
    """Test AC-8.5: mlx_lm.lora training (real mode, with test iters).

    This test is SLOW (involves actual training) and requires mlx_lm installed.
    Set environment variable SKIP_SLOW_TEST=1 to skip.
    """
    import os
    if os.environ.get("SKIP_SLOW_TEST"):
        print("\n=== Test 5: mlx_lm.lora Training [SKIPPED] ===")
        return

    print("\n=== Test 5: mlx_lm.lora Training ===")

    # Check if mlx_lm is available
    try:
        subprocess.run(
            [sys.executable, "-m", "mlx_lm.lora", "--help"],
            capture_output=True,
            timeout=5,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        print("  SKIP: mlx_lm not available (pip install mlx_lm)")
        return

    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)
        db_path = tmpdir / "test.db"
        jsonl_path = tmpdir / "outcomes.jsonl"
        adapter_out = tmpdir / "adapter-out"

        # Create fixtures
        content_hashes = create_stub_db(db_path)
        create_outcomes_jsonl(jsonl_path, content_hashes)

        # Parse and resolve
        outcomes = parse_outcomes_jsonl(jsonl_path)
        resolved, skipped = resolve_hashes_against_db(outcomes, db_path)

        # Run mlx_lm training with test iters (10, not 500).
        # run_mlx_lora_training returns a 3-TUPLE (exit_code, train_loss,
        # val_loss). This was previously assigned to a bare `rc` and compared
        # `rc == 0`, which is never true for a tuple — so this test always took
        # the failure branch, printed "Training failed with rc=(0, 11.82,
        # 9.669)", and asserted nothing. A live test that burns ~18 GB of GPU
        # and cannot fail is worse than no test: it reads as coverage.
        print(f"  Running mlx_lm.lora with {len(resolved)} resolved outcomes...")
        rc, train_loss, val_loss = run_mlx_lora_training(
            resolved, adapter_out, iters=10, scale=2.0)

        assert rc == 0, f"mlx_lm.lora training failed (rc={rc})"
        adapters_file = adapter_out / "adapters.safetensors"
        assert adapters_file.exists(), \
            f"training returned 0 but wrote no adapter at {adapters_file}"
        size = adapters_file.stat().st_size
        assert size > 0, f"adapter at {adapters_file} is empty (0 bytes)"

        # The e2e proof that the train/valid split actually works. mlx_lm only
        # emits "Val loss" lines when a valid.jsonl exists; before that split
        # landed, val_loss parsed as None and the regression gate had nothing to
        # judge — which is how "Regression verdict: PASS (val_loss=None)"
        # shipped on 2026-07-26. No unit pin can prove this: it requires a real
        # mlx_lm run to observe that the file we write is the file it reads.
        assert train_loss is not None, \
            "no train loss parsed from a real mlx_lm run — output format drifted?"
        assert val_loss is not None, (
            "no VAL loss parsed from a real mlx_lm run. The 90/10 split in "
            "run_mlx_lora_training is not producing a valid.jsonl mlx_lm reads, "
            "so the regression gate is judging on absent evidence.")

        print(f"  Training succeeded: {adapter_out} ({size} bytes, "
              f"train_loss={train_loss}, val_loss={val_loss})")
        print(f"  PASS: mlx_lm.lora training completed with real val evidence")


def main():
    """Run all tests."""
    print("=" * 60)
    print("  Test Suite: training_loop.py US-8 Phase C3 (--source-jsonl)")
    print("=" * 60)

    try:
        test_parsing()
        test_hash_resolution()
        test_sft_batch()
        test_dry_run_adapter()
        test_mlx_lora_training()

        print("\n" + "=" * 60)
        print("  All tests completed")
        print("=" * 60)
        return 0
    except AssertionError as e:
        print(f"\nTest FAILED: {e}")
        return 1
    except Exception as e:
        print(f"\nTest ERROR: {e}")
        import traceback
        traceback.print_exc()
        return 2


if __name__ == "__main__":
    sys.exit(main())
