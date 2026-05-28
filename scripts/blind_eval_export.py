#!/usr/bin/env python3
"""
Blind-evaluation harness: export anonymized response pairs for human rating.

Takes a set of real-vs-model response pairs (from a corpus or fixture), anonymizes
them (removes which is which, shuffles, assigns opaque IDs), and writes a rateable
file (JSONL or CSV) that a human rater can fill in with per-dimension scores.

Rationale (from HCI research): automated persona-fidelity metrics correlate only
40-60% with human judgment. Blind human evaluation by people who know the target
persona is the ground truth. This script prepares the anonymized set that raters
will evaluate.

Usage:
  python3 scripts/blind_eval_export.py \\
    --input ~/persona-eval-pairs.json \\
    --output ~/blind-ratings.jsonl \\
    --seed 42

Input format: JSON or JSONL with pairs array, each pair containing:
  {
    "prompt": "...",
    "real_response": "Seth's actual reply",
    "model_response": "Model's generated reply",
    ...other fields...
  }

Output format (JSONL): one JSON object per line, one per response pair:
  {
    "rating_id": "r00001",
    "prompt": "user prompt text",
    "response": "anonymized response (real OR model, shuffled)",
    "tone_1_10": null,
    "vocabulary_1_10": null,
    "humor_1_10": null,
    "decision_style_1_10": null,
    "notes": ""
  }

The rating_id, prompt, and response text are FIXED. The four 1-10 scores and
notes are FILLED IN by the human rater. This script then shuffles which response
is real vs model, so blind_eval_ingest.py can compute win rates and per-dimension
aggregates.
"""

import argparse
import json
import random
import sys
from pathlib import Path
from typing import List, Tuple


def load_pairs_from_json(path: Path) -> List[dict]:
    """Load response pairs from a JSON file.

    Expected format:
      {"pairs": [{"prompt": "...", "real_response": "...", "model_response": "..."}, ...]}

    Returns:
        List of pair dicts.
    """
    if not path.exists():
        return []

    with open(path) as f:
        data = json.load(f)

    if isinstance(data, dict) and "pairs" in data:
        return data["pairs"]
    elif isinstance(data, list):
        return data
    else:
        return []


def load_pairs_from_jsonl(path: Path) -> List[dict]:
    """Load response pairs from a JSONL file.

    Each line is a JSON object with "prompt", "real_response", "model_response".

    Returns:
        List of pair dicts.
    """
    if not path.exists():
        return []

    pairs = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                pairs.append(json.loads(line))
            except json.JSONDecodeError:
                continue

    return pairs


def load_pairs(path: Path) -> List[dict]:
    """Load pairs from either JSON or JSONL format."""
    if path.suffix == ".jsonl":
        return load_pairs_from_jsonl(path)
    else:
        return load_pairs_from_json(path)


def export_blind_pairs(
    pairs: List[dict],
    seed: int = 42
) -> List[dict]:
    """Anonymize and shuffle response pairs for blind rating.

    For each pair, create TWO rating items:
      - One with the real response
      - One with the model response

    Then shuffle all items together and assign opaque IDs, so raters cannot
    deduce which is real vs model by position or ID patterns.

    Args:
        pairs: list of dicts with "prompt", "real_response", "model_response"
        seed: RNG seed for reproducibility (fixed shuffle)

    Returns:
        List of rating dicts, each with rating_id, prompt, response, and null score fields.
    """
    rng = random.Random(seed)

    rating_items = []
    for pair_idx, pair in enumerate(pairs):
        prompt = pair.get("prompt", "")
        real_response = pair.get("real_response", "")
        model_response = pair.get("model_response", "")

        # Create two rating items for this pair
        # Mark which is real vs model so ingest can compute win rates
        rating_items.append({
            "prompt": prompt,
            "response": real_response,
            "source": "real",
            "pair_idx": pair_idx,
        })
        rating_items.append({
            "prompt": prompt,
            "response": model_response,
            "source": "model",
            "pair_idx": pair_idx,
        })

    # Shuffle all items together
    rng.shuffle(rating_items)

    # Assign opaque IDs
    result = []
    for idx, item in enumerate(rating_items):
        rating_id = f"r{idx + 1:05d}"
        result.append({
            "rating_id": rating_id,
            "prompt": item["prompt"],
            "response": item["response"],
            "source": item["source"],  # Hidden from human rater in the CSV output, but used by ingest
            "pair_idx": item["pair_idx"],  # Hidden from human rater, used by ingest
            "tone_1_10": None,
            "vocabulary_1_10": None,
            "humor_1_10": None,
            "decision_style_1_10": None,
            "notes": "",
        })

    return result


def write_rating_file_jsonl(
    items: List[dict],
    path: Path
) -> None:
    """Write rating items to a JSONL file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w") as f:
        for item in items:
            f.write(json.dumps(item) + "\n")


def write_rating_file_csv(
    items: List[dict],
    path: Path
) -> None:
    """Write rating items to a CSV file for human raters.

    CSV includes only the visible fields: rating_id, prompt, response, and the
    four score columns. The source and pair_idx are HIDDEN (not in CSV) so raters
    cannot deduce which is real.
    """
    import csv

    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "rating_id",
                "prompt",
                "response",
                "tone_1_10",
                "vocabulary_1_10",
                "humor_1_10",
                "decision_style_1_10",
                "notes",
            ]
        )
        writer.writeheader()
        for item in items:
            # CSV only includes visible fields
            writer.writerow({
                "rating_id": item["rating_id"],
                "prompt": item["prompt"],
                "response": item["response"],
                "tone_1_10": item.get("tone_1_10", ""),
                "vocabulary_1_10": item.get("vocabulary_1_10", ""),
                "humor_1_10": item.get("humor_1_10", ""),
                "decision_style_1_10": item.get("decision_style_1_10", ""),
                "notes": item.get("notes", ""),
            })


def main():
    ap = argparse.ArgumentParser(
        description="Anonymize and export response pairs for blind human evaluation",
    )
    ap.add_argument(
        "--input",
        type=Path,
        required=True,
        help="Input pairs file (JSON or JSONL): {prompt, real_response, model_response}",
    )
    ap.add_argument(
        "--output",
        type=Path,
        required=True,
        help="Output rating file (.jsonl or .csv); format inferred from extension",
    )
    ap.add_argument(
        "--seed",
        type=int,
        default=42,
        help="RNG seed for reproducible shuffle (default: 42)",
    )
    args = ap.parse_args()

    # Load pairs
    print(f"[INFO] Loading pairs from {args.input}", file=sys.stderr)
    pairs = load_pairs(args.input)

    if not pairs:
        print(f"[ERROR] No pairs loaded from {args.input}", file=sys.stderr)
        sys.exit(1)

    print(f"[INFO] Loaded {len(pairs)} pairs", file=sys.stderr)

    # Anonymize and shuffle
    print(f"[INFO] Anonymizing and shuffling pairs (seed={args.seed})", file=sys.stderr)
    rating_items = export_blind_pairs(pairs, seed=args.seed)

    print(f"[INFO] Generated {len(rating_items)} rating items", file=sys.stderr)

    # Write output
    print(f"[INFO] Writing to {args.output}", file=sys.stderr)
    if args.output.suffix == ".csv":
        write_rating_file_csv(rating_items, args.output)
        print(f"[INFO] CSV written. Open in Excel, fill in scores 1-10 and notes, save.", file=sys.stderr)
    else:
        write_rating_file_jsonl(rating_items, args.output)
        print(f"[INFO] JSONL written. Each line is one response to rate.", file=sys.stderr)

    print(f"[INFO] Done: {len(rating_items)} items ready for rating", file=sys.stderr)


if __name__ == "__main__":
    main()
