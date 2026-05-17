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
    OR --input-schema yntp <yntp_output.json>  (Sprint 11 / US-11.6 AC-11.6.5)

Schemas:
    Default (positional quadruples): name delta pad_rate n_total ...

    --input-schema yntp: consume the JSON emitted by scripts/yntp_eval.py.
        Mapping (BINDING contract for Sprint 11 Wave 2 US-11.7):
            yntp.delta_ll  -> pareto.fidelity_delta
            yntp.pad_rate  -> pareto.pad_failure_rate
        All Pareto thresholds (PROMOTE/DEFER/REJECT) apply identically.
        The `--input-schema` flag is positionless wrt the rest of the CLI:
        it takes the JSON path immediately after itself.

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


def _rows_from_yntp_json(path: str) -> list:
    """Load a single YNTP-eval result JSON and map to the Pareto row shape.

    Sprint 11 / US-11.6 AC-11.6.5: BINDING contract for Wave 2 US-11.7.
    The mapping is intentionally minimal — every Pareto threshold applies
    identically to YNTP input as to the original positional-quadruple input.

      yntp.delta_ll  -> fidelity_delta (the Pareto "delta" axis)
      yntp.pad_rate  -> pad_failure_rate (the Pareto "pad" axis)

    `n_total` is taken from yntp.n_pairs. `name` defaults to the basename
    of `fixture_path` (or "yntp-eval" if absent) so the printed table is
    legible.

    Raises ValueError on missing required keys; SystemExit on file errors,
    matching the rest of the CLI's failure mode.
    """
    p = Path(path).expanduser()
    if not p.exists():
        sys.exit(f"yntp input not found: {path}")
    try:
        obj = json.loads(p.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        sys.exit(f"yntp input is not valid JSON ({path}): {exc}")

    required = ("delta_ll", "pad_rate", "n_pairs")
    missing = [k for k in required if k not in obj]
    if missing:
        sys.exit(
            f"yntp input missing required key(s) {missing}; got keys: "
            f"{sorted(obj.keys())}"
        )

    delta = float(obj["delta_ll"])
    pad_rate = float(obj["pad_rate"])
    n_total = int(obj["n_pairs"])
    fixture_path = obj.get("fixture_path", "")
    name = Path(fixture_path).name if fixture_path else "yntp-eval"
    return [{
        "name": name,
        "delta": delta,
        "pad_rate": pad_rate,
        "n_total": n_total,
        "pareto_score": pareto_score(delta, pad_rate),
        "verdict": classify(delta, pad_rate),
    }]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--input-schema",
        choices=("yntp",),
        default=None,
        help="If set, treat positional args as paths to YNTP-eval JSON output "
        "(scripts/yntp_eval.py --output X.json). Maps delta_ll -> "
        "fidelity_delta and pad_rate -> pad_failure_rate. BINDING contract "
        "for Sprint 11 Wave 2 US-11.7.",
    )
    ap.add_argument(
        "checkpoints",
        nargs="+",
        help=(
            "Default schema: quadruples 'name delta pad_rate n_total ...'.\n"
            "With --input-schema yntp: one or more JSON paths emitted by "
            "scripts/yntp_eval.py."
        ),
    )
    args = ap.parse_args()

    if args.input_schema == "yntp":
        rows = []
        for path in args.checkpoints:
            rows.extend(_rows_from_yntp_json(path))
    else:
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
