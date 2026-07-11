#!/usr/bin/env python3
"""
Tests for kto_export.py

Creates a synthetic temporary SQLite database with test feedback_signals,
then validates export behavior against the contract.

Run: pytest scripts/test_kto_export.py -v (cwd-independent)
"""

import json
import sqlite3
import subprocess
import sys
import tempfile
from pathlib import Path

KTO_EXPORT = str(Path(__file__).resolve().parent / "kto_export.py")


def create_test_db(db_path: str, n_rows: int = 100, source: str = "user_feedback"):
    """
    Create a temporary test database with feedback_signals table.

    Args:
        db_path: Path to create the database
        n_rows: Number of rows to insert
        source: Source label for all rows
    """
    con = sqlite3.connect(db_path)
    con.execute("""
        CREATE TABLE feedback_signals(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            prompt TEXT NOT NULL,
            response TEXT NOT NULL,
            label INTEGER NOT NULL,
            source TEXT,
            timestamp INTEGER NOT NULL
        )
    """)

    now_ts = int(__import__('time').time())
    for i in range(n_rows):
        label = i % 2  # Alternate 0 and 1
        con.execute(
            "INSERT INTO feedback_signals (prompt, response, label, source, timestamp) "
            "VALUES (?, ?, ?, ?, ?)",
            (
                f"User: How are you today? (row {i})",
                f"I'm doing well, thanks for asking! (row {i})",
                label,
                source,
                now_ts + i,
            )
        )

    con.commit()
    con.close()


def test_export_happy_path():
    """Export rows above threshold should succeed."""
    with tempfile.TemporaryDirectory() as tmpdir:
        db_path = Path(tmpdir) / "test.db"
        output_path = Path(tmpdir) / "output.jsonl"

        # Create database with 100 rows (well above threshold of 50)
        create_test_db(str(db_path), n_rows=100)

        # Run export
        result = subprocess.run(
            [
                sys.executable,
                KTO_EXPORT,
                "--db", str(db_path),
                "--output", str(output_path),
                "--min-threshold", "50",
            ],
            capture_output=True,
            text=True,
        )

        assert result.returncode == 0, f"Export failed: {result.stderr}"
        assert output_path.exists(), "Output file not created"

        # Verify content
        with open(output_path) as f:
            lines = f.readlines()

        assert len(lines) == 100, f"Expected 100 lines, got {len(lines)}"

        # Validate format of first line
        first = json.loads(lines[0])
        assert "prompt" in first, "Missing 'prompt' field"
        assert "completion" in first, "Missing 'completion' field"
        assert "label" in first, "Missing 'label' field"
        assert isinstance(first["label"], bool), "Label should be boolean"

        print("✓ Happy path test passed")


def test_export_below_threshold():
    """Export with rows below threshold should refuse (exit code 2)."""
    with tempfile.TemporaryDirectory() as tmpdir:
        db_path = Path(tmpdir) / "test.db"
        output_path = Path(tmpdir) / "output.jsonl"

        # Create database with 30 rows (below threshold of 50)
        create_test_db(str(db_path), n_rows=30)

        # Run export
        result = subprocess.run(
            [
                sys.executable,
                KTO_EXPORT,
                "--db", str(db_path),
                "--output", str(output_path),
                "--min-threshold", "50",
            ],
            capture_output=True,
            text=True,
        )

        assert result.returncode == 2, f"Expected exit code 2 (refusal), got {result.returncode}"
        assert not output_path.exists(), "Output file should not be created when below threshold"

        print("✓ Below-threshold refusal test passed")


def test_export_idempotency():
    """Multiple exports should produce identical output (deterministic ordering)."""
    with tempfile.TemporaryDirectory() as tmpdir:
        db_path = Path(tmpdir) / "test.db"
        output1 = Path(tmpdir) / "output1.jsonl"
        output2 = Path(tmpdir) / "output2.jsonl"

        # Create database
        create_test_db(str(db_path), n_rows=100)

        # Export twice
        for output_path in [output1, output2]:
            result = subprocess.run(
                [
                    sys.executable,
                    KTO_EXPORT,
                    "--db", str(db_path),
                    "--output", str(output_path),
                    "--min-threshold", "50",
                ],
                capture_output=True,
                text=True,
            )
            assert result.returncode == 0

        # Compare files byte-for-byte
        content1 = output1.read_text()
        content2 = output2.read_text()

        assert content1 == content2, "Exports should be identical"

        print("✓ Idempotency test passed")


def test_export_format_validation():
    """Exported JSONL should parse as valid and match kto_mlx_train.py expectations."""
    with tempfile.TemporaryDirectory() as tmpdir:
        db_path = Path(tmpdir) / "test.db"
        output_path = Path(tmpdir) / "output.jsonl"

        create_test_db(str(db_path), n_rows=100)

        # Export
        result = subprocess.run(
            [
                sys.executable,
                KTO_EXPORT,
                "--db", str(db_path),
                "--output", str(output_path),
                "--min-threshold", "50",
            ],
            capture_output=True,
            text=True,
        )
        assert result.returncode == 0

        # Parse and validate
        with open(output_path) as f:
            for line_no, line in enumerate(f, 1):
                pair = json.loads(line)

                # kto_mlx_train.py expects exactly these fields
                assert "prompt" in pair, f"Line {line_no}: missing prompt"
                assert "completion" in pair, f"Line {line_no}: missing completion"
                assert "label" in pair, f"Line {line_no}: missing label"

                # Type checks
                assert isinstance(pair["prompt"], str), f"Line {line_no}: prompt not string"
                assert isinstance(pair["completion"], str), f"Line {line_no}: completion not string"
                assert isinstance(pair["label"], bool), f"Line {line_no}: label not boolean"

                # Content checks
                assert len(pair["prompt"]) > 0, f"Line {line_no}: empty prompt"
                assert len(pair["completion"]) > 0, f"Line {line_no}: empty completion"

        print("✓ Format validation test passed")


def test_export_dry_run():
    """--dry-run should print counts without writing output."""
    with tempfile.TemporaryDirectory() as tmpdir:
        db_path = Path(tmpdir) / "test.db"
        output_path = Path(tmpdir) / "output.jsonl"

        create_test_db(str(db_path), n_rows=75)

        # Run with --dry-run
        result = subprocess.run(
            [
                sys.executable,
                KTO_EXPORT,
                "--db", str(db_path),
                "--output", str(output_path),
                "--min-threshold", "50",
                "--dry-run",
            ],
            capture_output=True,
            text=True,
        )

        assert result.returncode is None or result.returncode == 0
        assert not output_path.exists(), "Output file should not be created with --dry-run"
        assert "would write 75 pairs" in result.stdout, "Should report what would be written"

        print("✓ Dry-run test passed")


def test_export_multiple_sources():
    """Export should count rows by source correctly."""
    with tempfile.TemporaryDirectory() as tmpdir:
        db_path = Path(tmpdir) / "test.db"
        output_path = Path(tmpdir) / "output.jsonl"

        con = sqlite3.connect(str(db_path))
        con.execute("""
            CREATE TABLE feedback_signals(
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                prompt TEXT NOT NULL,
                response TEXT NOT NULL,
                label INTEGER NOT NULL,
                source TEXT,
                timestamp INTEGER NOT NULL
            )
        """)

        now_ts = int(__import__('time').time())

        # Insert rows from multiple sources
        sources_and_counts = {"user_feedback": 40, "system_eval": 30, "other": 20}

        for source, count in sources_and_counts.items():
            for i in range(count):
                con.execute(
                    "INSERT INTO feedback_signals (prompt, response, label, source, timestamp) "
                    "VALUES (?, ?, ?, ?, ?)",
                    (f"Prompt {source} {i}", f"Response {source} {i}", i % 2, source, now_ts + i)
                )

        con.commit()
        con.close()

        # Export
        result = subprocess.run(
            [
                sys.executable,
                KTO_EXPORT,
                "--db", str(db_path),
                "--output", str(output_path),
                "--min-threshold", "50",
            ],
            capture_output=True,
            text=True,
        )

        assert result.returncode == 0
        assert output_path.exists()

        # Verify counts in output
        with open(output_path) as f:
            lines = f.readlines()

        assert len(lines) == 90, f"Expected 90 total rows, got {len(lines)}"

        # Check that output contains all sources
        for source in sources_and_counts:
            assert source in result.stdout, f"Source {source} not in output"

        print("✓ Multiple sources test passed")


def test_export_with_empty_db():
    """Export from empty database should exit code 0 (graceful no-op)."""
    with tempfile.TemporaryDirectory() as tmpdir:
        db_path = Path(tmpdir) / "test.db"
        output_path = Path(tmpdir) / "output.jsonl"

        # Create empty database
        con = sqlite3.connect(str(db_path))
        con.execute("""
            CREATE TABLE feedback_signals(
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                prompt TEXT NOT NULL,
                response TEXT NOT NULL,
                label INTEGER NOT NULL,
                source TEXT,
                timestamp INTEGER NOT NULL
            )
        """)
        con.commit()
        con.close()

        # Run export
        result = subprocess.run(
            [
                sys.executable,
                KTO_EXPORT,
                "--db", str(db_path),
                "--output", str(output_path),
                "--min-threshold", "50",
            ],
            capture_output=True,
            text=True,
        )

        assert result.returncode == 0
        assert not output_path.exists(), "Should not create file for empty result"

        print("✓ Empty database test passed")


if __name__ == "__main__":
    # Run all tests
    test_export_happy_path()
    test_export_below_threshold()
    test_export_idempotency()
    test_export_format_validation()
    test_export_dry_run()
    test_export_multiple_sources()
    test_export_with_empty_db()

    print("\n✅ All tests passed!")
