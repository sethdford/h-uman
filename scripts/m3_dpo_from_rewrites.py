#!/usr/bin/env python3
"""
Phase D7 (2026-05-18) — DPO trainer that consumes REWRITE preference
pairs captured by the C-side rewrite-capture module.

Input format (~/.human/training-data/m3-rewrite-pairs.jsonl):
  {"t":1779142521147,"ph":...,"k":1,"prompt":"...","rejected":"...","accepted":"..."}

This script is the bridge between the OUTCOMES side of the loop
(which captures preference pairs at the moment of guard-rewrite) and
the TRAINING side (DPO/IPO/KTO learning over those pairs).

What it does today:
  - Parse the rewrite-pairs JSONL
  - Filter for quality (non-empty fields, sane lengths)
  - Print a structured summary (count, length distribution, top
    rejected-pattern matches)
  - Emit an Alpaca-DPO JSONL ready to feed dpo_mlx_train.py or
    other downstream DPO trainers
  - Optionally invoke `human ml dpo-train` directly when --train
    is set (the existing C-side DPO trainer is in src/ml/dpo.c)

What it doesn't do:
  - Actually run DPO against gemma — that requires the MLX bridge
    (separate slice) AND a real LoRA on the served model
  - Self-judge whether the captured pairs are good training data
    (some rewrites may be marginal; production should add a quality
    filter at capture time, not consume time)

Usage:
    python3 scripts/m3_dpo_from_rewrites.py                       # summary
    python3 scripts/m3_dpo_from_rewrites.py --export-jsonl OUT    # export
    python3 scripts/m3_dpo_from_rewrites.py --train               # call dpo-train

Exit codes:
    0 — summary/export produced
    2 — input not found / parse failure on every line
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_PAIRS_JSONL = Path.home() / ".human" / "training-data" / "m3-rewrite-pairs.jsonl"


def parse_pairs(path: Path) -> list[dict]:
    """Read the rewrite pairs JSONL. Skips blank lines and malformed
    JSON (logged warning, not fatal — one bad line shouldn't poison
    a 1000-pair training set)."""
    if not path.exists():
        return []
    pairs = []
    with open(path) as f:
        for line_no, line in enumerate(f, start=1):
            line = line.strip()
            if not line:
                continue
            try:
                p = json.loads(line)
            except json.JSONDecodeError as e:
                print(f"  WARN: {path.name}:{line_no} malformed JSON ({e})")
                continue
            # Minimum-viable filter: all three text fields must be present
            # AND non-trivial (≥3 chars each so we don't train on noise).
            if (not p.get("prompt") or not p.get("rejected") or not p.get("accepted")
                    or len(p["prompt"]) < 3 or len(p["rejected"]) < 3
                    or len(p["accepted"]) < 3):
                continue
            pairs.append(p)
    return pairs


def summarize_pairs(pairs: list[dict]) -> dict:
    """Compute statistics for logging. Pure function."""
    if not pairs:
        return {"count": 0}
    p_lens = [len(p["prompt"]) for p in pairs]
    r_lens = [len(p["rejected"]) for p in pairs]
    a_lens = [len(p["accepted"]) for p in pairs]
    kinds: dict[int, int] = {}
    for p in pairs:
        k = p.get("k", 0)
        kinds[k] = kinds.get(k, 0) + 1
    # Length delta (accepted - rejected): negative means the guard
    # shortened the response (typical for AI-disclosure repairs);
    # positive means it added clarification.
    deltas = [a - r for a, r in zip(a_lens, r_lens)]
    return {
        "count": len(pairs),
        "prompt_avg_len": sum(p_lens) // len(p_lens),
        "rejected_avg_len": sum(r_lens) // len(r_lens),
        "accepted_avg_len": sum(a_lens) // len(a_lens),
        "delta_avg": sum(deltas) // len(deltas),
        "delta_min": min(deltas),
        "delta_max": max(deltas),
        "turn_kinds": kinds,
    }


def export_alpaca_dpo(pairs: list[dict], out_path: Path) -> None:
    """Write the pairs as Alpaca-DPO JSONL (chosen / rejected schema):
        {"prompt": "...", "chosen": "...", "rejected": "..."}
    This is the input format consumed by:
      - scripts/dpo_mlx_train.py (mlx-lm DPO trainer)
      - axolotl's DPO config
      - HuggingFace TRL's DPOTrainer

    Single canonical format so downstream tools don't need adapters."""
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w") as f:
        for p in pairs:
            record = {
                "prompt": p["prompt"],
                "chosen": p["accepted"],   # guard-approved
                "rejected": p["rejected"], # what the provider produced raw
            }
            f.write(json.dumps(record) + "\n")


def maybe_invoke_dpo_trainer(jsonl_path: Path, adapter_out: Path) -> int:
    """Invoke `human ml dpo-train` if available. Returns rc."""
    human_bin = os.environ.get("HUMAN_BIN") or str(REPO_ROOT / "build-release" / "human")
    if not Path(human_bin).exists():
        human_bin = str(REPO_ROOT / "build" / "human")
    if not Path(human_bin).exists():
        print("  WARN: no human binary found — skipping DPO training")
        return 0
    cmd = [human_bin, "ml", "dpo-train", "--help"]
    try:
        rc = subprocess.call(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if rc != 0:
            print("  WARN: human ml dpo-train not available (subcommand unknown)")
            return 0
    except FileNotFoundError:
        return 0
    # Real invocation. Argument shape will need to match dpo-train's actual
    # signature; this is a placeholder until the C side accepts a JSONL.
    # For now, we just confirm the trainer exists and skip the actual call
    # (it would expect a specific persona / data layout we haven't built).
    print(f"  NOTE: dpo-train subcommand available but JSONL ingestion is a "
          f"follow-up slice. Exported pairs sit at {jsonl_path}.")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--input", type=Path, default=DEFAULT_PAIRS_JSONL,
                    help="Rewrite pairs JSONL (default %(default)s)")
    ap.add_argument("--export-jsonl", type=Path,
                    help="Write Alpaca-DPO formatted pairs to this path")
    ap.add_argument("--train", action="store_true",
                    help="Attempt to invoke `human ml dpo-train` after export")
    ap.add_argument("--adapter-out", type=Path,
                    help="Where dpo-train should write its adapter (with --train)")
    args = ap.parse_args()

    print(f"\n{'='*60}")
    print(f"  M3 DPO FROM REWRITES (D7)")
    print(f"{'='*60}")
    print(f"  Input:  {args.input}")

    pairs = parse_pairs(args.input)
    summary = summarize_pairs(pairs)
    print(f"  Pairs:  {summary['count']}")

    if summary["count"] == 0:
        print(f"  Nothing to train on. The capture side fires only when the")
        print(f"  response_guard chain rewrites — generate some rewrite events")
        print(f"  by sending /v1/chat/completions through the daemon under load.")
        return 0

    print(f"  Avg prompt:   {summary['prompt_avg_len']} chars")
    print(f"  Avg rejected: {summary['rejected_avg_len']} chars")
    print(f"  Avg accepted: {summary['accepted_avg_len']} chars")
    print(f"  Delta range:  {summary['delta_min']:+d} to {summary['delta_max']:+d} "
          f"(avg {summary['delta_avg']:+d})")
    kind_names = {1: "stream", 2: "batch", 3: "proactive"}
    kk = ", ".join(f"{kind_names.get(k, '?')}={v}"
                   for k, v in sorted(summary["turn_kinds"].items()))
    print(f"  Turn kinds:   {kk}")
    print(f"{'='*60}")

    if args.export_jsonl:
        export_alpaca_dpo(pairs, args.export_jsonl)
        print(f"\n  Exported {len(pairs)} pairs → {args.export_jsonl}")
        print(f"  Format: Alpaca-DPO ({{prompt, chosen, rejected}} per line)")
        print(f"  Feed to: scripts/dpo_mlx_train.py / axolotl / TRL DPOTrainer")

    if args.train:
        adapter_out = args.adapter_out or (Path.home() / ".human" / "training-data" /
                                            "adapters" / "dpo-from-rewrites.bin")
        return maybe_invoke_dpo_trainer(args.export_jsonl or args.input, adapter_out)

    return 0


if __name__ == "__main__":
    sys.exit(main())
