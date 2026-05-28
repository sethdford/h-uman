#!/usr/bin/env python3
"""
Blind-evaluation harness: ingest human ratings and compute per-dimension aggregates.

Reads the filled-in rating file from a human rater (with 1-10 scores on
tone, vocabulary, humor, decision-style), joins back to the hidden real/model
labels, and emits per-dimension aggregate scores plus a real-vs-model win rate.

The win rate is computed as: for each dimension, count how many times the real
response outscored the model response (on the SAME prompt pair), then divide by
total pairs. A response pair "wins" if the median of its real-response scores
exceeds the median of its model-response scores for that dimension.

Usage:
  python3 scripts/blind_eval_ingest.py \\
    --input ~/blind-ratings-filled.jsonl \\
    --exported ~/blind-ratings.jsonl \\
    --output ~/eval-results.json

Output format (JSON):
  {
    "n_pairs": 10,
    "n_ratings": 20,
    "dimensions": {
      "tone": {
        "mean_real": 7.5,
        "mean_model": 6.8,
        "real_win_rate": 0.65,
        "pct": 65
      },
      "vocabulary": { ... },
      "humor": { ... },
      "decision_style": { ... }
    },
    "overall_real_win_rate": 0.63,
    "ratings": [  // Debug: per-pair median scores
      {
        "prompt": "...",
        "pair_idx": 0,
        "real": {"tone": 7, "vocabulary": 8, ...},
        "model": {"tone": 6, "vocabulary": 7, ...}
      },
      ...
    ]
  }
"""

import argparse
import csv
import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path
from typing import Dict, List, Optional, Tuple


def load_exported_jsonl(path: Path) -> Dict[str, dict]:
    """Load the exported anonymized ratings file to map rating_id to hidden metadata.

    Returns a dict mapping rating_id -> {source: "real"|"model", pair_idx: int, ...}
    """
    mapping = {}
    if not path.exists():
        return mapping

    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                item = json.loads(line)
                mapping[item["rating_id"]] = {
                    "source": item.get("source", "unknown"),
                    "pair_idx": item.get("pair_idx", -1),
                    "prompt": item.get("prompt", ""),
                }
            except json.JSONDecodeError:
                continue

    return mapping


def load_ratings_from_jsonl(path: Path) -> List[dict]:
    """Load filled-in ratings from a JSONL file.

    Expected format: one JSON object per line with rating_id and four score fields.
    """
    ratings = []
    if not path.exists():
        return ratings

    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                ratings.append(json.loads(line))
            except json.JSONDecodeError:
                continue

    return ratings


def load_ratings_from_csv(path: Path) -> List[dict]:
    """Load filled-in ratings from a CSV file."""
    ratings = []
    if not path.exists():
        return ratings

    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            # Convert string scores to int/None
            def score_or_none(s):
                if s is None or s == "" or s == "None":
                    return None
                try:
                    return int(s)
                except ValueError:
                    return None

            ratings.append({
                "rating_id": row.get("rating_id", ""),
                "tone_1_10": score_or_none(row.get("tone_1_10")),
                "vocabulary_1_10": score_or_none(row.get("vocabulary_1_10")),
                "humor_1_10": score_or_none(row.get("humor_1_10")),
                "decision_style_1_10": score_or_none(row.get("decision_style_1_10")),
                "notes": row.get("notes", ""),
            })

    return ratings


def load_ratings(path: Path) -> List[dict]:
    """Load ratings from either JSONL or CSV format."""
    if path.suffix == ".csv":
        return load_ratings_from_csv(path)
    else:
        return load_ratings_from_jsonl(path)


def ingest_ratings(
    ratings: List[dict],
    exported_mapping: Dict[str, dict]
) -> Tuple[Dict, List]:
    """Join ratings to hidden metadata and compute per-dimension aggregates.

    Returns:
        (aggregates_dict, per_pair_list) where aggregates_dict has structure:
        {
          "tone": {"mean_real": ..., "mean_model": ..., "real_win_rate": ...},
          ...
        }
        and per_pair_list is the debug breakdown by pair.
    """
    # Group ratings by pair_idx, tracking real vs model
    pairs_data = defaultdict(lambda: {"real": {}, "model": {}})

    for rating in ratings:
        rating_id = rating.get("rating_id", "")
        if rating_id not in exported_mapping:
            print(f"[WARN] Rating ID not found in exported mapping: {rating_id}", file=sys.stderr)
            continue

        meta = exported_mapping[rating_id]
        source = meta.get("source", "unknown")
        pair_idx = meta.get("pair_idx", -1)

        if pair_idx < 0:
            print(f"[WARN] Invalid pair_idx for {rating_id}", file=sys.stderr)
            continue

        # Extract the four dimensions
        scores = {
            "tone": rating.get("tone_1_10"),
            "vocabulary": rating.get("vocabulary_1_10"),
            "humor": rating.get("humor_1_10"),
            "decision_style": rating.get("decision_style_1_10"),
        }

        # Store under real or model
        if source in ["real", "model"]:
            pairs_data[pair_idx][source].update(scores)

    # Compute per-dimension aggregates
    dimensions = ["tone", "vocabulary", "humor", "decision_style"]
    aggregates = {}

    per_pair = []
    for pair_idx in sorted(pairs_data.keys()):
        pair_entry = pairs_data[pair_idx]
        real_scores = pair_entry["real"]
        model_scores = pair_entry["model"]

        # Build per-pair medians for each dimension
        pair_result = {
            "pair_idx": pair_idx,
            "real": {},
            "model": {},
        }

        for dim in dimensions:
            real_vals = real_scores.get(dim)
            model_vals = model_scores.get(dim)
            if real_vals is not None:
                pair_result["real"][dim] = real_vals
            if model_vals is not None:
                pair_result["model"][dim] = model_vals

        per_pair.append(pair_result)

    # Aggregate across all pairs and raters
    for dim in dimensions:
        real_scores = []
        model_scores = []

        for pair_idx in pairs_data:
            pair_entry = pairs_data[pair_idx]
            real_val = pair_entry["real"].get(dim)
            model_val = pair_entry["model"].get(dim)

            if real_val is not None:
                real_scores.append(real_val)
            if model_val is not None:
                model_scores.append(model_val)

        mean_real = statistics.mean(real_scores) if real_scores else 0.0
        mean_model = statistics.mean(model_scores) if model_scores else 0.0

        # Win rate: for each pair, real wins if its median score > model's median score
        real_wins = 0
        total_pairs = 0

        for pair_idx in pairs_data:
            pair_entry = pairs_data[pair_idx]
            real_vals = []
            model_vals = []

            # Collect ALL ratings for real and model for this dimension
            for rating in ratings:
                rating_id = rating.get("rating_id", "")
                if rating_id not in exported_mapping:
                    continue
                meta = exported_mapping[rating_id]
                if meta.get("pair_idx") != pair_idx:
                    continue

                score = rating.get(f"{dim}_1_10")
                if score is not None:
                    if meta.get("source") == "real":
                        real_vals.append(score)
                    elif meta.get("source") == "model":
                        model_vals.append(score)

            if real_vals and model_vals:
                real_median = statistics.median(real_vals)
                model_median = statistics.median(model_vals)
                if real_median > model_median:
                    real_wins += 1
                total_pairs += 1

        real_win_rate = real_wins / total_pairs if total_pairs > 0 else 0.0

        aggregates[dim] = {
            "mean_real": round(mean_real, 2),
            "mean_model": round(mean_model, 2),
            "real_win_rate": round(real_win_rate, 2),
            "pct": round(real_win_rate * 100, 0),
        }

    # Overall win rate (across all dimensions)
    all_real_wins = 0
    all_total = 0
    for dim in dimensions:
        win_rate = aggregates[dim].get("real_win_rate", 0.0)
        # Approximate: count pairs where this dimension's real > model
        for pair_idx in pairs_data:
            pair_entry = pairs_data[pair_idx]
            real_val = pair_entry["real"].get(dim)
            model_val = pair_entry["model"].get(dim)
            if real_val is not None and model_val is not None:
                if real_val > model_val:
                    all_real_wins += 1
                all_total += 1

    overall_win_rate = all_real_wins / all_total if all_total > 0 else 0.0

    return (
        {
            "dimensions": aggregates,
            "overall_real_win_rate": round(overall_win_rate, 2),
            "overall_real_win_pct": round(overall_win_rate * 100, 0),
        },
        per_pair,
    )


def main():
    ap = argparse.ArgumentParser(
        description="Ingest blind human ratings and compute per-dimension aggregates"
    )
    ap.add_argument(
        "--input",
        type=Path,
        required=True,
        help="Filled-in ratings file (.jsonl or .csv)",
    )
    ap.add_argument(
        "--exported",
        type=Path,
        required=True,
        help="Original exported JSONL (with hidden source/pair_idx metadata)",
    )
    ap.add_argument(
        "--output",
        type=Path,
        required=True,
        help="Output JSON with per-dimension aggregates and win rates",
    )
    args = ap.parse_args()

    # Load exported mapping (hidden metadata)
    print(f"[INFO] Loading exported metadata from {args.exported}", file=sys.stderr)
    exported_mapping = load_exported_jsonl(args.exported)
    print(f"[INFO] Mapped {len(exported_mapping)} rating IDs", file=sys.stderr)

    # Load filled-in ratings
    print(f"[INFO] Loading ratings from {args.input}", file=sys.stderr)
    ratings = load_ratings(args.input)
    print(f"[INFO] Loaded {len(ratings)} ratings", file=sys.stderr)

    if not ratings:
        print(f"[ERROR] No ratings loaded", file=sys.stderr)
        sys.exit(1)

    # Ingest and compute aggregates
    print(f"[INFO] Computing per-dimension aggregates", file=sys.stderr)
    aggregates, per_pair = ingest_ratings(ratings, exported_mapping)

    n_pairs = len(per_pair) if per_pair else 0
    print(f"[INFO] Computed aggregates for {n_pairs} pairs", file=sys.stderr)

    # Write output
    result = {
        "n_pairs": n_pairs,
        "n_ratings": len(ratings),
        **aggregates,
        "per_pair": per_pair,
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2))
    print(f"[INFO] Results written to {args.output}", file=sys.stderr)

    # Summary to stdout
    print(f"\n=== BLIND EVAL RESULTS ===")
    print(f"Pairs: {n_pairs}, Ratings: {len(ratings)}")
    print(f"Overall real win rate: {aggregates.get('overall_real_win_rate', 0.0)}")
    print(f"\nPer-dimension:")
    for dim, scores in aggregates.get("dimensions", {}).items():
        print(f"  {dim:20} real={scores['mean_real']:.1f} model={scores['mean_model']:.1f} win_rate={scores['real_win_rate']:.1%}")


if __name__ == "__main__":
    main()
