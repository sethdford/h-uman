#!/usr/bin/env python3
"""
KTO (Kahneman-Tversky Optimization) Training Data Exporter

Closes the reaction→train loop: single-sided preference signals (thumbs-up/down,
feedback labels) are exported from memory.db to JSONL format for KTO training.
Unlike DPO pairs (which require rejected alternatives), KTO trains on single-sided
signals where label=true (desirable) and label=false (undesirable).

Data sources:
  - feedback_signals: user feedback with label 0 (bad) / 1 (good)
  - (future) production_outcomes: tapback reactions (+1/-1) when alternatives available

Usage:
  python3 scripts/kto_export.py                  # export to default path
  python3 scripts/kto_export.py --dry-run        # show what would be written, no output
  python3 scripts/kto_export.py --db <path>      # use alternate database
  python3 scripts/kto_export.py --min-threshold N # require N examples, refuse if below

Exit codes:
  0  Success — data file written or below threshold (explicit refusal)
  1  Error — could not read database or write file
  2  Below threshold — no data file written; marker should not proceed to train
"""

import argparse
import json
import sqlite3
import sys
from datetime import datetime
from pathlib import Path


DB_DEFAULT = str(Path.home() / ".human" / "memory.db")
DATA_PATH_DEFAULT = str(Path.home() / ".human" / "lora-pairs.jsonl.kto.jsonl")
MIN_THRESHOLD_DEFAULT = 50  # refuse to train on fewer than this many examples


def fetch_feedback_signals(db_path: str, limit: int = 5000) -> list:
    """
    Read single-sided feedback signals from memory.db.

    Returns list of dicts: {id, prompt, response, label, source, timestamp}
    """
    try:
        con = sqlite3.connect(db_path)
        con.row_factory = sqlite3.Row
        rows = con.execute(
            "SELECT id, prompt, response, label, source, timestamp "
            "FROM feedback_signals "
            "WHERE prompt IS NOT NULL AND response IS NOT NULL "
            "ORDER BY id "
            "LIMIT ?",
            (limit,),
        ).fetchall()
        con.close()
        return [dict(r) for r in rows]
    except sqlite3.Error as e:
        print(f"[kto_export] ERROR: database error: {e}", file=sys.stderr)
        return None


def generate_kto_pairs(feedback_rows: list) -> list:
    """
    Convert feedback signals to KTO JSONL format.

    Each row becomes:
      {"prompt": "...", "completion": "...", "label": true/false}
    """
    pairs = []
    for row in feedback_rows:
        # label: 1 → true (desirable), 0 → false (undesirable)
        label = bool(row.get("label"))

        pairs.append({
            "prompt": row["prompt"],
            "completion": row["response"],
            "label": label,
        })

    return pairs


def write_pairs_jsonl(pairs: list, output_path: str) -> int:
    """
    Write KTO pairs to JSONL file (one JSON object per line).

    Returns count of pairs written.
    """
    try:
        output_file = Path(output_path)
        output_file.parent.mkdir(parents=True, exist_ok=True)

        with open(output_file, 'w') as f:
            for pair in pairs:
                f.write(json.dumps(pair) + '\n')

        return len(pairs)
    except (IOError, OSError) as e:
        print(f"[kto_export] ERROR: could not write {output_path}: {e}", file=sys.stderr)
        return None


def write_metadata(output_path: str, n_pairs: int, source_counts: dict) -> bool:
    """
    Write metadata marker alongside the data file.

    Creates <output_path>.kto.json with training metadata.
    """
    metadata_path = output_path.replace('.jsonl', '') + '.kto.json'
    metadata = {
        'timestamp': datetime.now().isoformat(),
        'data_path': output_path,
        'n_pairs': n_pairs,
        'n_pairs_by_source': source_counts,
    }

    try:
        with open(metadata_path, 'w') as f:
            json.dump(metadata, f, indent=2)
        return True
    except (IOError, OSError) as e:
        print(f"[kto_export] WARNING: could not write metadata {metadata_path}: {e}",
              file=sys.stderr)
        return False


def main():
    ap = argparse.ArgumentParser(
        description="Export KTO training data from memory.db"
    )
    ap.add_argument("--db", default=DB_DEFAULT,
                    help="Path to memory.db (READ-ONLY)")
    ap.add_argument("--output", default=DATA_PATH_DEFAULT,
                    help="Path to write JSONL data file")
    ap.add_argument("--min-threshold", type=int, default=MIN_THRESHOLD_DEFAULT,
                    help="Minimum examples required; refuse to write if below")
    ap.add_argument("--dry-run", action="store_true",
                    help="Show counts without writing output")
    ap.add_argument("--limit", type=int, default=5000,
                    help="Maximum rows to fetch from database")
    args = ap.parse_args()

    # Verify database exists and is readable
    db_path = Path(args.db)
    if not db_path.exists():
        print(f"[kto_export] ERROR: database not found: {args.db}", file=sys.stderr)
        sys.exit(1)

    # Fetch feedback signals
    feedback_rows = fetch_feedback_signals(args.db, limit=args.limit)
    if feedback_rows is None:
        sys.exit(1)

    if not feedback_rows:
        print("[kto_export] No eligible feedback signals found")
        sys.exit(0)

    # Count by source
    source_counts = {}
    for row in feedback_rows:
        src = row.get("source", "unknown")
        source_counts[src] = source_counts.get(src, 0) + 1

    # Log findings
    print(f"[kto_export] Found {len(feedback_rows)} feedback signals")
    for src, count in sorted(source_counts.items()):
        print(f"  {src:<30} {count:>4}")

    # Check threshold
    if len(feedback_rows) < args.min_threshold:
        print(f"[kto_export] Below threshold ({len(feedback_rows)} < {args.min_threshold})")
        print(f"[kto_export] Refusing to write data file; marker should not proceed")
        sys.exit(2)  # explicit refusal signal

    # Generate KTO pairs
    kto_pairs = generate_kto_pairs(feedback_rows)

    if args.dry_run:
        print(f"\n[kto_export] --dry-run: would write {len(kto_pairs)} pairs")
        if kto_pairs:
            print(f"Sample first pair:")
            sample = kto_pairs[0]
            for k in ("label",):
                print(f"  {k}: {sample[k]}")
            for k in ("prompt", "completion"):
                val = sample[k][:80] if isinstance(sample[k], str) else sample[k]
                print(f"  {k}: {val!r}")
        return

    # Write output
    n_written = write_pairs_jsonl(kto_pairs, args.output)
    if n_written is None:
        sys.exit(1)

    # Write metadata
    write_metadata(args.output, n_written, source_counts)

    print(f"[kto_export] Wrote {n_written} pairs to {args.output}")
    sys.exit(0)


if __name__ == "__main__":
    main()
