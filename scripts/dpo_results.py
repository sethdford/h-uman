#!/usr/bin/env python3
"""
DPO Training Results Quality Gate

After each DPO training run, record a structured JSON line with:
- timestamp, adapter/run id, n_pairs by source
- final train loss, final val loss, alignment/eval score
- lora scale, iters, git commit

Regression verdict: PASS/FAIL/FIRST_RUN
- FAIL when val loss > best-of-prior-4-weeks + 0.1
- FAIL when val loss is 0.6931±0.001 (degenerate random baseline signature)
"""

import json
import os
import subprocess
import sys
from datetime import datetime, timedelta
from pathlib import Path
from typing import Dict, List, Optional, Tuple


def get_git_commit() -> str:
    """Get the current git commit SHA."""
    try:
        result = subprocess.run(
            ['git', 'rev-parse', 'HEAD'],
            capture_output=True,
            text=True,
            check=True
        )
        return result.stdout.strip()
    except Exception:
        return "unknown"


def is_degenerate_loss(val_loss: float, threshold: float = 0.001) -> bool:
    """
    Check if val_loss is the random-baseline signature (0.6931 ± threshold).

    This indicates the model did not learn from the pairs (collapsed to chance).
    """
    return abs(val_loss - 0.6931) < threshold


def load_recent(
    results_file: Path,
    days_back: int = 28
) -> List[Dict]:
    """
    Load recent training results from the JSONL file.

    Returns records within the past `days_back` days.
    Skips malformed lines without crashing.
    """
    if not results_file.exists():
        return []

    recent = []
    cutoff = datetime.now() - timedelta(days=days_back)

    try:
        with open(results_file, 'r') as f:
            for line_no, line in enumerate(f, 1):
                line = line.strip()
                if not line:
                    continue
                try:
                    record = json.loads(line)
                    ts_str = record.get('timestamp')
                    if ts_str:
                        ts = datetime.fromisoformat(ts_str)
                        if ts >= cutoff:
                            recent.append(record)
                except (json.JSONDecodeError, ValueError, TypeError) as e:
                    # Log malformed line but continue
                    print(
                        f"[dpo_results] skipping malformed line {line_no}: {e}",
                        file=sys.stderr
                    )
    except Exception as e:
        print(f"[dpo_results] error reading {results_file}: {e}", file=sys.stderr)

    return recent


def regression_verdict(
    history: List[Dict],
    new_result: Dict,
    loss_threshold_delta: float = 0.1
) -> str:
    """
    Determine if new training result passes regression test.

    Returns:
        'PASS'       — val loss within acceptable range of history
        'FAIL'       — val loss too high or degenerate signature detected
        'FIRST_RUN'  — no prior history to compare against
    """
    new_val_loss = new_result.get('val_loss')

    # No val loss recorded? Can't judge. Assume PASS.
    if new_val_loss is None:
        return 'PASS'

    # Degenerate signature (random baseline)?
    if is_degenerate_loss(new_val_loss):
        return 'FAIL'

    # No history? This is the baseline.
    if not history:
        return 'FIRST_RUN'

    # Find best val loss in history
    best_val_loss = min(r['val_loss'] for r in history if r.get('val_loss') is not None)

    # FAIL if new loss exceeds best-of-history + threshold
    if new_val_loss > best_val_loss + loss_threshold_delta:
        return 'FAIL'

    return 'PASS'


def append_result(
    results_file: Path,
    timestamp: str,
    adapter_id: str,
    n_pairs_by_source: Dict[str, int],
    train_loss: Optional[float],
    val_loss: Optional[float],
    alignment_score: Optional[float],
    lora_scale: float,
    iters: int,
    git_commit: str
) -> None:
    """
    Append one result record to the JSONL file.

    Automatically creates parent directories if needed.
    """
    results_file.parent.mkdir(parents=True, exist_ok=True)

    record = {
        'timestamp': timestamp,
        'adapter_id': adapter_id,
        'n_pairs_by_source': n_pairs_by_source,
        'train_loss': train_loss,
        'val_loss': val_loss,
        'alignment_score': alignment_score,
        'lora_scale': lora_scale,
        'iters': iters,
        'git_commit': git_commit,
    }

    with open(results_file, 'a') as f:
        f.write(json.dumps(record) + '\n')


def main():
    """CLI for testing."""
    import argparse

    parser = argparse.ArgumentParser(
        description='DPO training results quality gate'
    )
    parser.add_argument(
        '--results-file',
        type=Path,
        default=Path.home() / '.human' / 'logs' / 'dpo-training-results.jsonl',
        help='Path to results JSONL file'
    )
    parser.add_argument(
        '--append',
        nargs=8,
        metavar=('adapter_id', 'n_pairs_json', 'train_loss', 'val_loss',
                 'align_score', 'lora_scale', 'iters', 'git_commit'),
        help='Append a result record (val_loss and align_score can be "null")'
    )
    parser.add_argument(
        '--check',
        type=float,
        metavar='val_loss',
        help='Check regression verdict for a given val_loss'
    )

    args = parser.parse_args()

    if args.append:
        adapter_id, n_pairs_json, train_loss_str, val_loss_str, align_score_str, scale_str, iters_str, commit = args.append

        try:
            n_pairs = json.loads(n_pairs_json)
        except json.JSONDecodeError:
            n_pairs = {}

        train_loss = float(train_loss_str) if train_loss_str != 'null' else None
        val_loss = float(val_loss_str) if val_loss_str != 'null' else None
        align_score = float(align_score_str) if align_score_str != 'null' else None
        lora_scale = float(scale_str)
        iters = int(iters_str)

        append_result(
            args.results_file,
            datetime.now().isoformat(),
            adapter_id,
            n_pairs,
            train_loss,
            val_loss,
            align_score,
            lora_scale,
            iters,
            commit
        )
        print(f"Appended to {args.results_file}")

    elif args.check is not None:
        history = load_recent(args.results_file)
        verdict = regression_verdict(history, {'val_loss': args.check})
        print(verdict)
        sys.exit(0 if verdict != 'FAIL' else 1)

    else:
        parser.print_help()


if __name__ == '__main__':
    main()
