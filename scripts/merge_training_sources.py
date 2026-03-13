#!/usr/bin/env python3
"""
Merge training data from multiple sources (iMessage, Facebook, etc.)
into a single unified dataset for LoRA fine-tuning.

Reads from:
    data/imessage/training_pairs.jsonl
    data/facebook/training_pairs.jsonl
    (any future data/*/training_pairs.jsonl)

Outputs:
    data/merged/training_pairs.jsonl — deduplicated, shuffled, merged training data
    data/merged/stats.json — per-source statistics

Usage:
    python scripts/merge_training_sources.py [--augmented]
"""

import json
import os
import random
import statistics
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DATA_ROOT = os.path.join(SCRIPT_DIR, "..", "data")
OUT_DIR = os.path.join(DATA_ROOT, "merged")

SOURCES = {
    "imessage": os.path.join(DATA_ROOT, "imessage", "training_pairs.jsonl"),
    "facebook": os.path.join(DATA_ROOT, "facebook", "training_pairs.jsonl"),
}

AUGMENTED_PATH = os.path.join(DATA_ROOT, "imessage", "augmented_pairs.jsonl")


def load_pairs(path, source_name):
    if not os.path.exists(path):
        return []
    pairs = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                pair = json.loads(line)
                if "metadata" not in pair:
                    pair["metadata"] = {}
                if "source" not in pair["metadata"]:
                    pair["metadata"]["source"] = source_name
                pair["metadata"]["origin_file"] = source_name
                pairs.append(pair)
            except json.JSONDecodeError:
                continue
    return pairs


def extract_fingerprint(pair):
    """Create a fingerprint for deduplication based on the last assistant message."""
    msgs = pair.get("messages", [])
    assistant_msgs = [m["content"] for m in msgs if m["role"] == "assistant"]
    if not assistant_msgs:
        return None
    last = assistant_msgs[-1].strip().lower()
    if len(last) < 3:
        return None
    return last


def deduplicate(pairs):
    """Remove near-duplicate training pairs based on assistant response content."""
    seen = set()
    unique = []
    dupes = 0
    for pair in pairs:
        fp = extract_fingerprint(pair)
        if fp is None:
            unique.append(pair)
            continue
        if fp in seen:
            dupes += 1
            continue
        seen.add(fp)
        unique.append(pair)
    return unique, dupes


def compute_stats(pairs):
    by_source = {}
    for pair in pairs:
        source = pair.get("metadata", {}).get("origin_file", "unknown")
        if source not in by_source:
            by_source[source] = []
        by_source[source].append(pair)

    stats = {}
    for source, source_pairs in by_source.items():
        lengths = []
        for p in source_pairs:
            msgs = p.get("messages", [])
            for m in msgs:
                if m["role"] == "assistant":
                    lengths.append(len(m["content"]))

        stats[source] = {
            "count": len(source_pairs),
            "avg_reply_length": round(statistics.mean(lengths), 1) if lengths else 0,
            "median_reply_length": round(statistics.median(lengths), 1) if lengths else 0,
        }

    return stats


def main():
    import argparse
    parser = argparse.ArgumentParser(description="Merge training data from multiple sources")
    parser.add_argument("--augmented", action="store_true", help="Include augmented/synthetic pairs")
    parser.add_argument("--seed", type=int, default=42, help="Random seed for shuffling")
    args = parser.parse_args()

    all_pairs = []

    for source_name, path in SOURCES.items():
        pairs = load_pairs(path, source_name)
        if pairs:
            print(f"  {source_name}: {len(pairs)} pairs from {path}")
            all_pairs.extend(pairs)
        else:
            print(f"  {source_name}: no data found at {path}")

    if args.augmented and os.path.exists(AUGMENTED_PATH):
        augmented = load_pairs(AUGMENTED_PATH, "augmented")
        print(f"  augmented: {len(augmented)} pairs from {AUGMENTED_PATH}")
        all_pairs.extend(augmented)

    if not all_pairs:
        print("ERROR: No training data found from any source.")
        sys.exit(1)

    print(f"\nTotal before dedup: {len(all_pairs)}")
    all_pairs, dupes = deduplicate(all_pairs)
    print(f"Removed {dupes} duplicates")
    print(f"Total after dedup:  {len(all_pairs)}")

    random.seed(args.seed)
    random.shuffle(all_pairs)

    os.makedirs(OUT_DIR, exist_ok=True)

    out_path = os.path.join(OUT_DIR, "training_pairs.jsonl")
    with open(out_path, "w") as f:
        for pair in all_pairs:
            f.write(json.dumps(pair) + "\n")
    print(f"\nWrote {len(all_pairs)} merged pairs -> {out_path}")

    stats = compute_stats(all_pairs)
    stats["_total"] = {
        "count": len(all_pairs),
        "duplicates_removed": dupes,
    }
    stats_path = os.path.join(OUT_DIR, "stats.json")
    with open(stats_path, "w") as f:
        json.dump(stats, f, indent=2)
    print(f"Wrote stats -> {stats_path}")

    print("\n--- Per-Source Stats ---")
    for source, s in stats.items():
        if source.startswith("_"):
            continue
        print(f"  {source}: {s['count']} pairs, "
              f"avg reply {s['avg_reply_length']} chars, "
              f"median reply {s['median_reply_length']} chars")


if __name__ == "__main__":
    main()
