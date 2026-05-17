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


def ensemble_min_aggregate(stage_scores: dict) -> dict:
    """Min-aggregate orthogonal judge scores (Sprint 11 / US-11.7 AC-11.7.5).

    Per the cascade design (sprints/sprint-11/designs/US-11.7.md §4 Risk 3),
    null scores are EXCLUDED from min-aggregation: a dormant Stage 3 must
    not pull a verdict up OR down. Each remaining score is mapped to a
    per-judge verdict via `classify_score`; the overall verdict is the WORST
    of the per-judge verdicts (REJECT < DEFER < PROMOTE).

    Args:
        stage_scores: dict of {judge_name: score_or_None}. Typical keys:
            "lexical" (pareto delta * (1-pad_rate) or fidelity_delta),
            "coherence" (mean coherence score 0..1),
            "nll" (clipped delta_ll, positive = better),
            "prm" (may be None when Stage 3 is dormant).

    Returns:
        {
          "per_judge": {name: {"score": float|None, "verdict": str}, ...},
          "n_contributing": int,            # judges that contributed a score
          "n_null": int,                    # judges that produced null
          "min_judge": str | None,          # name of the worst contributing
          "verdict": "PROMOTE"|"DEFER"|"REJECT",
        }
    """
    # Verdict ordering: lower index = worse. Min-aggregate => the worst wins.
    _ORDER = {"REJECT": 0, "DEFER": 1, "PROMOTE": 2}
    _REVERSE = {0: "REJECT", 1: "DEFER", 2: "PROMOTE"}

    per_judge: dict = {}
    contributing: list = []
    n_null = 0

    for name, score in stage_scores.items():
        if score is None:
            per_judge[name] = {"score": None, "verdict": "SKIP"}
            n_null += 1
            continue
        verdict = _classify_score(name, float(score))
        per_judge[name] = {"score": float(score), "verdict": verdict}
        contributing.append((name, verdict))

    if not contributing:
        # All judges abstained or were null. Cannot promote without evidence;
        # mirror the cron's "SKIP-only outputs are DEFER, never PROMOTE" rule.
        return {
            "per_judge": per_judge,
            "n_contributing": 0,
            "n_null": n_null,
            "min_judge": None,
            "verdict": "DEFER",
        }

    # Min over the verdict ordering = worst verdict wins.
    contributing.sort(key=lambda nv: _ORDER[nv[1]])
    min_name, min_verdict = contributing[0]
    return {
        "per_judge": per_judge,
        "n_contributing": len(contributing),
        "n_null": n_null,
        "min_judge": min_name,
        "verdict": min_verdict,
    }


def _classify_score(judge_name: str, score: float) -> str:
    """Map a per-judge score to PROMOTE / DEFER / REJECT.

    Each judge has its own thresholds, but the ordering invariant is shared:
    a judge that produces a score outside its DEFER band is REJECT; inside
    DEFER but not PROMOTE is DEFER; at/above PROMOTE threshold is PROMOTE.

    Default thresholds (configurable per judge — see docstring constants):
      lexical:  PROMOTE>=0.03, DEFER>=0.01, else REJECT  (matches Pareto floor)
      coherence: PROMOTE>=0.80, DEFER>=0.70, else REJECT
      nll:      PROMOTE>=0.02, DEFER>=0.00, else REJECT  (delta_ll is signed)
      prm:      PROMOTE>=0.75, DEFER>=0.60, else REJECT  (Sprint 12 placeholder)
      ppl:      PROMOTE>=0.50, DEFER>=0.10, else REJECT  (score = 1 - ratio/floor)
    """
    name = judge_name.lower()
    if name == "lexical":
        if score >= PROMOTION_DELTA_FLOOR:
            return "PROMOTE"
        if score >= DEFER_DELTA_FLOOR:
            return "DEFER"
        return "REJECT"
    if name == "coherence":
        if score >= 0.80:
            return "PROMOTE"
        if score >= 0.70:
            return "DEFER"
        return "REJECT"
    if name == "nll":
        if score >= 0.02:
            return "PROMOTE"
        if score >= 0.00:
            return "DEFER"
        return "REJECT"
    if name == "prm":
        if score >= 0.75:
            return "PROMOTE"
        if score >= 0.60:
            return "DEFER"
        return "REJECT"
    if name == "ppl":
        if score >= 0.50:
            return "PROMOTE"
        if score >= 0.10:
            return "DEFER"
        return "REJECT"
    # Unknown judge: conservative — anything >= 0.5 is PROMOTE, >= 0 is DEFER.
    if score >= 0.5:
        return "PROMOTE"
    if score >= 0.0:
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
        "--stage-scores",
        type=str,
        default=None,
        help=(
            "Sprint 11 / US-11.7 ensemble mode. JSON object mapping judge "
            "name to score (or null for SKIP), e.g. "
            '\'{"lexical": 0.04, "coherence": 0.82, "nll": 0.01, "prm": null}\'. '
            "Applies min-aggregation per AC-11.7.5: the final verdict is the "
            "WORST per-judge verdict among contributing (non-null) judges. "
            "When set, positional checkpoints are ignored."
        ),
    )
    ap.add_argument(
        "checkpoints",
        nargs="*",
        help=(
            "Default schema: quadruples 'name delta pad_rate n_total ...'.\n"
            "With --input-schema yntp: one or more JSON paths emitted by "
            "scripts/yntp_eval.py."
        ),
    )
    args = ap.parse_args()

    # ── Sprint 11 / US-11.7 stage-scores ensemble mode ────────────────────
    if args.stage_scores is not None:
        try:
            scores_obj = json.loads(args.stage_scores)
        except json.JSONDecodeError as exc:
            sys.exit(f"--stage-scores: invalid JSON ({exc})")
        if not isinstance(scores_obj, dict):
            sys.exit("--stage-scores must be a JSON object {judge: score}")
        agg = ensemble_min_aggregate(scores_obj)

        print("=" * 90)
        print(f"{'Judge':<14} {'Score':<14} {'Verdict':<10}")
        print("-" * 90)
        for name, info in agg["per_judge"].items():
            score_s = f"{info['score']:+.4f}" if info["score"] is not None else "null"
            print(f"{name:<14} {score_s:<14} {info['verdict']:<10}")
        print("=" * 90)
        print(
            f"Ensemble (min-aggregation): {agg['n_contributing']} contributing, "
            f"{agg['n_null']} null"
        )
        if agg["min_judge"] is not None:
            print(f"Worst-judge: {agg['min_judge']} -> {agg['verdict']}")
        else:
            print(f"No contributing judges; default verdict: {agg['verdict']}")
        print(f"VERDICT: {agg['verdict']}")
        # JSON line for downstream consumers (stage_cascade.py).
        print(json.dumps({"ensemble": agg}, sort_keys=True))

        if agg["verdict"] == "PROMOTE":
            return 0
        if agg["verdict"] == "DEFER":
            return 1
        return 2

    if not args.checkpoints:
        sys.exit(
            "no checkpoints given (and --stage-scores not set). Pass "
            "quadruples or --input-schema yntp paths."
        )

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
