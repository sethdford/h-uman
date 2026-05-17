#!/usr/bin/env python3
"""
pareto_picker.py — Auto-select the best DPO checkpoint from a sweep.

Given a sweep result (output of ab_sweep.py or equivalent), score each
checkpoint by the *Pareto-frontier* of (fidelity_delta, coherence_rate)
and recommend the best one for promotion.

The scoring rule:
    pareto_score = fidelity_delta * (1 - pad_failure_rate)

This addresses Sprint 7 auditor §2 finding (synthetic fingerprint scores
broken output higher than coherent output): the multiplicative penalty
ensures that a high fidelity delta on broken output (e.g. iter 200's
+0.046 with 80% pad-fail) loses to a smaller delta on cleaner output
(iter 60's +0.019 with 40% pad-fail):
    iter 200:  +0.046 * (1 - 0.80) = +0.0092
    iter  60:  +0.019 * (1 - 0.40) = +0.0114  <-- WINS

Inputs:
    --sweep-json <path>  JSON with shape: {"sft_dir": ..., "checkpoints": [
                            {"name": ..., "fidelity_delta": ..., "pad_failure_rate": ...}
                         ]}
    OR positional args:  sft_dir checkpoint1 checkpoint2 ...  (re-runs sweep)

Outputs:
    Prints the Pareto-ranked table and recommends:
    PROMOTE: <best_checkpoint>  delta=X  pad_rate=Y  pareto_score=Z
    DEFER: ...
    REJECT: ...

Exit codes:
    0 = a checkpoint passed all gates (recommended for promotion)
    1 = best checkpoint is below the promotion floor (defer; no promotion)
    2 = no checkpoint produced positive delta (reject; investigate)
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


# Production gate (calibrated against Smoke Run #3 evidence):
#   PROMOTION_DELTA_FLOOR — minimum fidelity delta to consider promotion
#   PROMOTION_PAD_CEILING — max pad-leakage rate to consider promotion
# A checkpoint that meets BOTH gates is promotable.
PROMOTION_DELTA_FLOOR = 0.03
PROMOTION_PAD_CEILING = 0.10

# "Defer" gate (interesting enough to track, not yet promotable):
DEFER_DELTA_FLOOR = 0.01
DEFER_PAD_CEILING = 0.50


def pareto_score(delta: float, pad_rate: float) -> float:
    """Multiplicative penalty: only positive deltas on clean output score well."""
    if delta <= 0:
        return delta  # negative deltas score negative
    return delta * (1.0 - pad_rate)


def classify(delta: float, pad_rate: float) -> str:
    """One of PROMOTE / DEFER / REJECT."""
    if delta >= PROMOTION_DELTA_FLOOR and pad_rate <= PROMOTION_PAD_CEILING:
        return "PROMOTE"
    if delta >= DEFER_DELTA_FLOOR and pad_rate <= DEFER_PAD_CEILING:
        return "DEFER"
    return "REJECT"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "checkpoints",
        nargs="+",
        help="Quadruples: name delta pad_rate n_total. e.g. iter60 0.019 0.40 30",
    )
    args = ap.parse_args()

    if len(args.checkpoints) % 4 != 0:
        sys.exit("Pass quadruples: name delta pad_rate n_total (groups of 4)")

    rows = []
    for i in range(0, len(args.checkpoints), 4):
        name = args.checkpoints[i]
        delta = float(args.checkpoints[i + 1])
        pad_rate = float(args.checkpoints[i + 2])
        n_total = int(args.checkpoints[i + 3])
        score = pareto_score(delta, pad_rate)
        verdict = classify(delta, pad_rate)
        rows.append({
            "name": name,
            "delta": delta,
            "pad_rate": pad_rate,
            "n_total": n_total,
            "pareto_score": score,
            "verdict": verdict,
        })

    # Sort by pareto score descending
    rows.sort(key=lambda r: r["pareto_score"], reverse=True)

    print("=" * 90)
    print(f"{'Checkpoint':<22} {'Δ':<10} {'pad rate':<12} {'Pareto':<12} {'Verdict':<10}")
    print("-" * 90)
    for r in rows:
        print(
            f"{r['name']:<22} "
            f"{r['delta']:+.4f}    "
            f"{r['pad_rate']:>4.0%}        "
            f"{r['pareto_score']:+.4f}     "
            f"{r['verdict']}"
        )
    print("=" * 90)
    print(f"Promotion gate: Δ ≥ {PROMOTION_DELTA_FLOOR:+.2f} AND pad rate ≤ {PROMOTION_PAD_CEILING:.0%}")
    print(f"Defer gate:     Δ ≥ {DEFER_DELTA_FLOOR:+.2f} AND pad rate ≤ {DEFER_PAD_CEILING:.0%}")

    best = rows[0]
    print()
    print(f"BEST PARETO: {best['name']}  Δ={best['delta']:+.4f}  pad={best['pad_rate']:.0%}  score={best['pareto_score']:+.4f}")
    print(f"VERDICT:     {best['verdict']}")

    # Exit codes for CI integration
    if best["verdict"] == "PROMOTE":
        return 0
    if best["verdict"] == "DEFER":
        return 1
    return 2


if __name__ == "__main__":
    sys.exit(main())
