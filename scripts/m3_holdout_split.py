#!/usr/bin/env python3
"""
M3 holdout split (2026-05-19) — split corpus into train + held-out.

The behavioral eval needs prompts the trainer NEVER saw. This splits
the corpus deterministically (seeded) into:
  - train/m3-corpus-train.jsonl   (90% — feeds H2 counterfactuals)
  - holdout/m3-holdout-prompts.jsonl  (10% — feeds behavioral eval)

The split is per-(handle, role) bucket so both train and holdout have
representative Seth-authored turns from each contact. A contact with
3 turns will contribute 1 to holdout; a contact with 30 will contribute
3 — proportional sampling, never empty buckets.

Held-out shape: one record per Seth-authored turn, with the PRECEDING
user message as `prompt` and Seth's actual response as `reference`.
This is the same shape `response_to_pairs` produces.

Usage:
    python3 scripts/m3_holdout_split.py \\
        --corpus ~/.human/training-data/m3-corpus.jsonl \\
        --train-out ~/.human/training-data/m3-corpus-train.jsonl \\
        --holdout-out ~/.human/training-data/m3-holdout-prompts.jsonl \\
        --holdout-frac 0.10

Exit codes:
  0 — split produced
  2 — corpus missing or no Seth turns to split
"""
from __future__ import annotations

import argparse
import json
import random
import sys
from pathlib import Path

DEFAULT_CORPUS = Path.home() / ".human" / "training-data" / "m3-corpus.jsonl"
DEFAULT_TRAIN = Path.home() / ".human" / "training-data" / "m3-corpus-train.jsonl"
DEFAULT_HOLDOUT = Path.home() / ".human" / "training-data" / "m3-holdout-prompts.jsonl"


def split_corpus(corpus_path: Path, holdout_frac: float, seed: int
                  ) -> tuple[list[dict], list[dict]]:
    """Walk the corpus, build (prompt, response) pairs from Seth-authored
    assistant turns paired with their preceding user message from the
    SAME handle, then split into train and holdout."""
    if not corpus_path.exists():
        return [], []
    # Bucket by handle so the split is per-conversation
    by_handle: dict[str, list[dict]] = {}
    with open(corpus_path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            by_handle.setdefault(r.get("handle", ""), []).append(r)

    pairs = []
    for handle, records in by_handle.items():
        records.sort(key=lambda r: r.get("ts_ms", 0))
        for i, r in enumerate(records):
            if r.get("role") != "assistant":
                continue
            # Walk backward to find preceding user turn
            for j in range(i - 1, -1, -1):
                if records[j].get("role") == "user":
                    pairs.append({
                        "handle": handle,
                        "channel": r.get("channel", ""),
                        "prompt": records[j]["content"],
                        "reference": r["content"],
                        "ts_ms": r.get("ts_ms", 0),
                    })
                    break

    # Per-handle proportional split
    pairs_by_handle: dict[str, list[dict]] = {}
    for p in pairs:
        pairs_by_handle.setdefault(p["handle"], []).append(p)

    rng = random.Random(seed)
    train: list[dict] = []
    holdout: list[dict] = []
    for handle, h_pairs in pairs_by_handle.items():
        rng.shuffle(h_pairs)
        n_hold = max(1, int(len(h_pairs) * holdout_frac))
        if len(h_pairs) <= 2:
            # Tiny buckets: all go to train (can't spare for holdout)
            train.extend(h_pairs)
            continue
        holdout.extend(h_pairs[:n_hold])
        train.extend(h_pairs[n_hold:])
    return train, holdout


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS)
    ap.add_argument("--train-out", type=Path, default=DEFAULT_TRAIN)
    ap.add_argument("--holdout-out", type=Path, default=DEFAULT_HOLDOUT)
    ap.add_argument("--holdout-frac", type=float, default=0.10,
                    help="Fraction of pairs to hold out (default 0.10)")
    ap.add_argument("--seed", type=int, default=42,
                    help="RNG seed for reproducible split")
    ap.add_argument("--max-holdout", type=int, default=100,
                    help="Cap on held-out prompts (eval cost control)")
    args = ap.parse_args()

    print(f"\n{'='*60}")
    print(f"  M3 HOLDOUT SPLIT")
    print(f"{'='*60}")
    print(f"  Corpus:        {args.corpus}")
    print(f"  Holdout frac:  {args.holdout_frac}")
    print(f"  Max holdout:   {args.max_holdout}")
    print(f"  Seed:          {args.seed}")

    train, holdout = split_corpus(args.corpus, args.holdout_frac, args.seed)
    if not train and not holdout:
        print(f"  ERROR: no pairs derivable from {args.corpus}",
              file=sys.stderr)
        return 2

    # Cap holdout size for cost control (each held-out prompt = ~2 mlx
    # generation runs in the behavioral eval; keep it bounded)
    rng = random.Random(args.seed + 1)
    if len(holdout) > args.max_holdout:
        # Keep diversity: shuffle and take the first N
        rng.shuffle(holdout)
        spillover = holdout[args.max_holdout:]
        holdout = holdout[:args.max_holdout]
        train.extend(spillover)

    args.train_out.parent.mkdir(parents=True, exist_ok=True)
    args.holdout_out.parent.mkdir(parents=True, exist_ok=True)
    with open(args.train_out, "w") as f:
        for p in train:
            f.write(json.dumps(p, ensure_ascii=False) + "\n")
    with open(args.holdout_out, "w") as f:
        for p in holdout:
            f.write(json.dumps(p, ensure_ascii=False) + "\n")

    print(f"  Train:    {len(train)} pairs → {args.train_out}")
    print(f"  Holdout:  {len(holdout)} pairs → {args.holdout_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
