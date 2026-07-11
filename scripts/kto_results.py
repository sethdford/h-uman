#!/usr/bin/env python3
"""
KTO Training Results Quality Gate

After each KTO training run, record a structured JSON line with:
- timestamp, adapter/run id, n_pairs by source
- final train loss, final val loss
- lora scale, iters, git commit

This follows the same convention as dpo_results.py for consistency.
"""

import json
import os
import re
import subprocess
import sys
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Tuple


def parse_mlx_losses(stdout: str) -> Tuple[Optional[float], Optional[float]]:
    """
    Parse mlx_lm.lora training output to extract final train and val losses.

    Returns (train_loss, val_loss) from the LAST occurrence of each in the output.
    Returns (None, None) if neither is found.
    """
    if not stdout:
        return None, None

    train_loss = None
    val_loss = None

    # Pattern for training loss: "Iter N: ... loss X.XXX ..."
    train_pattern = re.compile(r'\bIter\s+\d+:.*\bloss\s+([0-9.e\-+]+)')

    # Pattern for validation loss: "Val loss X.XXX" or "Validation loss X.XXX"
    val_pattern = re.compile(r'\b(?:Val|Validation)\s+loss\s+([0-9.e\-+]+)')

    # Find LAST occurrence of each pattern
    for line in stdout.split('\n'):
        match = train_pattern.search(line)
        if match:
            try:
                train_loss = float(match.group(1))
            except ValueError:
                pass

        match = val_pattern.search(line)
        if match:
            try:
                val_loss = float(match.group(1))
            except ValueError:
                pass

    return train_loss, val_loss


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


def append_result(
    results_file: Path,
    timestamp: str,
    adapter_id: str,
    n_pairs_by_source: Dict[str, int],
    train_loss: Optional[float],
    val_loss: Optional[float],
    lora_scale: float,
    iters: int,
    git_commit: str
) -> None:
    """
    Append one KTO result record to the JSONL file.

    Automatically creates parent directories if needed.
    """
    results_file.parent.mkdir(parents=True, exist_ok=True)

    record = {
        'timestamp': timestamp,
        'adapter_id': adapter_id,
        'n_pairs_by_source': n_pairs_by_source,
        'train_loss': train_loss,
        'val_loss': val_loss,
        'lora_scale': lora_scale,
        'iters': iters,
        'git_commit': git_commit,
    }

    with open(results_file, 'a') as f:
        f.write(json.dumps(record) + '\n')


def main():
    """CLI for appending results."""
    import argparse

    parser = argparse.ArgumentParser(
        description='KTO training results quality gate'
    )
    parser.add_argument(
        '--results-file',
        type=Path,
        default=Path.home() / '.human' / 'logs' / 'kto-training-results.jsonl',
        help='Path to results JSONL file'
    )
    parser.add_argument(
        '--append',
        nargs=7,
        metavar=('adapter_id', 'n_pairs_json', 'train_loss', 'val_loss',
                 'lora_scale', 'iters', 'git_commit'),
        help='Append a result record (train_loss and val_loss can be "null")'
    )

    args = parser.parse_args()

    if args.append:
        adapter_id, n_pairs_json, train_loss_str, val_loss_str, scale_str, iters_str, commit = args.append

        try:
            n_pairs = json.loads(n_pairs_json)
        except json.JSONDecodeError:
            n_pairs = {}

        train_loss = float(train_loss_str) if train_loss_str != 'null' else None
        val_loss = float(val_loss_str) if val_loss_str != 'null' else None
        lora_scale = float(scale_str)
        iters = int(iters_str)

        append_result(
            args.results_file,
            datetime.now().isoformat(),
            adapter_id,
            n_pairs,
            train_loss,
            val_loss,
            lora_scale,
            iters,
            commit
        )
        print(f"Appended to {args.results_file}")
    else:
        parser.print_help()


if __name__ == '__main__':
    main()
