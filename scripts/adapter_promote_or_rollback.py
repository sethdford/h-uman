#!/usr/bin/env python3
"""
Decide whether to promote a candidate LoRA adapter over the incumbent.

Reads two eval-fidelity verdicts (produced by scripts/eval_fidelity_nightly.py)
and outputs a PROMOTE / KEEP / ROLLBACK verdict with reasoning. This is the
decision-only half of the active-learning loop: it does NOT run the eval
itself (that's the nightly job) and does NOT mutate adapter symlinks
(operator action, scoped by the verdict). What it does is replace
"eyeball-compare two JSON files and guess" with a calibrated rule.

Spec context: docs/plans/2026-05-26-m3-dispatch-unification/STATUS.md
"What this sprint did NOT do" → "A/B framework for LoRA adapter
promotion". This script closes that item as a *decision tool*; wiring
into a nightly cron + auto-symlink-flip is a future operational step.

### Decision matrix

Given incumbent verdict I and candidate verdict C:

  C.verdict == FAIL                          → ROLLBACK
    The eval-fidelity gate already failed C in isolation. No
    comparison needed; do not promote.

  C.verdict == SKIP                          → KEEP
    Eval couldn't run (missing fixture, model unavailable, etc.).
    No data; preserve the incumbent.

  I.verdict == FAIL AND C.verdict == PASS    → PROMOTE
    The incumbent itself fails eval-fidelity; the candidate passes.
    Easy win; promote.

  Both PASS, with statistical comparison:
    C.delta.ci_lower > I.delta.ci_upper      → PROMOTE
      Confidence intervals don't overlap → statistically significant
      improvement.

    C.delta.mean > I.delta.mean + tolerance  → PROMOTE
      The candidate's mean is meaningfully higher (default 5% absolute).
      The tolerance defends against ping-ponging on noise.

    abs(C.delta.mean - I.delta.mean) < tolerance → KEEP
      Within noise band. Don't churn; "first to ship, last to remove"
      stability wins ties.

    C.delta.mean < I.delta.mean - tolerance  → KEEP (do NOT promote)
      The candidate is meaningfully worse. Keep incumbent.

### Why this decision-only design

The eval-fidelity script already produces calibrated verdicts. The
remaining question is operator policy: WHEN does a passing candidate
deserve promotion? Different deployments want different aggressiveness:
  - High-stakes: only promote on non-overlapping CIs (very conservative).
  - Fast iteration: promote on any mean improvement (very aggressive).

Making this a small Python script means operators can read the rules,
trust them, and adjust thresholds without recompiling the daemon. The
script is also CI-friendly (exit code 0 = decision made, exit 1 = bad
input).

Usage:
  python3 scripts/adapter_promote_or_rollback.py \\
    --incumbent ~/.human/logs/eval-fidelity-incumbent.json \\
    --candidate ~/.human/logs/eval-fidelity-candidate.json \\
    [--tolerance 0.05] [--output-json /tmp/promote-decision.json]

Exit codes:
  0 = decision made (verdict in JSON)
  1 = bad input (missing files, malformed JSON, etc.)
"""
from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


# ── Verdict enum (matches eval-fidelity-nightly's vocabulary) ────────
VERDICT_PASS = "PASS"
VERDICT_FAIL = "FAIL"
VERDICT_SKIP = "SKIP"

# ── Decision verdicts ────────────────────────────────────────────────
DECISION_PROMOTE = "PROMOTE"
DECISION_KEEP = "KEEP"
DECISION_ROLLBACK = "ROLLBACK"


def load_verdict(path: Path) -> dict[str, Any]:
    """Read an eval-fidelity verdict JSON. Raises ValueError with a
    human-readable message on any kind of malformation — the script's
    contract is decision-or-bad-input, never silent garbage."""
    if not path.exists():
        raise ValueError(f"file not found: {path}")
    try:
        data = json.loads(path.read_text())
    except json.JSONDecodeError as e:
        raise ValueError(f"{path}: malformed JSON ({e})") from e
    if not isinstance(data, dict):
        raise ValueError(f"{path}: expected JSON object at top level, got {type(data).__name__}")
    if "verdict" not in data:
        raise ValueError(f"{path}: missing 'verdict' field")
    return data


def get_delta_mean(v: dict[str, Any]) -> float | None:
    """Pull delta.mean from a verdict. Returns None if absent (SKIP/FAIL
    verdicts often have no delta block)."""
    delta = v.get("delta")
    if not isinstance(delta, dict):
        return None
    m = delta.get("mean")
    return float(m) if isinstance(m, (int, float)) else None


def get_delta_ci(v: dict[str, Any]) -> tuple[float, float] | None:
    """Pull (ci_lower, ci_upper) from a verdict. Returns None if either
    field is missing — comparing CIs requires both."""
    delta = v.get("delta")
    if not isinstance(delta, dict):
        return None
    lo = delta.get("ci_lower")
    hi = delta.get("ci_upper")
    if not isinstance(lo, (int, float)) or not isinstance(hi, (int, float)):
        return None
    return (float(lo), float(hi))


def decide(
    incumbent: dict[str, Any],
    candidate: dict[str, Any],
    tolerance: float = 0.05,
) -> tuple[str, str]:
    """Return (decision, reasoning). Decision is one of the DECISION_*
    constants; reasoning is a one-sentence operator-readable string.

    Tolerance is the absolute-delta band within which we KEEP the
    incumbent. Default 5% — anything smaller is below the noise floor
    of the bootstrap CI in production data."""

    c_verdict = candidate.get("verdict")
    i_verdict = incumbent.get("verdict")

    # FAIL on candidate → never promote, regardless of incumbent.
    if c_verdict == VERDICT_FAIL:
        return (
            DECISION_ROLLBACK,
            f"candidate verdict={c_verdict} (eval-fidelity gate failed in isolation); "
            f"do not promote.",
        )

    # SKIP on candidate → no data to compare; preserve incumbent.
    if c_verdict == VERDICT_SKIP:
        return (
            DECISION_KEEP,
            f"candidate verdict=SKIP ({candidate.get('reason', 'unspecified')}); "
            f"no data; keep incumbent.",
        )

    # Asymmetric special case: incumbent fails, candidate passes →
    # easy promotion (candidate strictly dominates).
    if i_verdict == VERDICT_FAIL and c_verdict == VERDICT_PASS:
        return (
            DECISION_PROMOTE,
            "incumbent verdict=FAIL, candidate verdict=PASS; candidate "
            "strictly dominates; promote.",
        )

    # SKIP on incumbent + PASS on candidate → promote (no data
    # for incumbent means we have nothing to defend it with, and the
    # candidate is independently certified).
    if i_verdict == VERDICT_SKIP and c_verdict == VERDICT_PASS:
        return (
            DECISION_PROMOTE,
            "incumbent verdict=SKIP (no comparison data) and candidate "
            "verdict=PASS in isolation; promote.",
        )

    # Both PASS → statistical comparison.
    if i_verdict == VERDICT_PASS and c_verdict == VERDICT_PASS:
        i_mean = get_delta_mean(incumbent)
        c_mean = get_delta_mean(candidate)
        if i_mean is None or c_mean is None:
            return (
                DECISION_KEEP,
                f"both PASS but delta.mean missing on at least one verdict "
                f"(incumbent={i_mean}, candidate={c_mean}); keep incumbent "
                f"as conservative default.",
            )

        i_ci = get_delta_ci(incumbent)
        c_ci = get_delta_ci(candidate)

        # Non-overlapping CIs — strongest signal. Promote.
        if i_ci and c_ci and c_ci[0] > i_ci[1]:
            return (
                DECISION_PROMOTE,
                f"candidate CI [{c_ci[0]:.3f}, {c_ci[1]:.3f}] is strictly "
                f"above incumbent CI [{i_ci[0]:.3f}, {i_ci[1]:.3f}]; "
                f"statistically significant improvement; promote.",
            )

        # Tolerance-based comparison on means.
        delta = c_mean - i_mean
        if delta > tolerance:
            return (
                DECISION_PROMOTE,
                f"candidate mean ({c_mean:+.3f}) exceeds incumbent "
                f"({i_mean:+.3f}) by {delta:+.3f} > tolerance {tolerance}; "
                f"promote.",
            )
        if delta < -tolerance:
            return (
                DECISION_KEEP,
                f"candidate mean ({c_mean:+.3f}) is below incumbent "
                f"({i_mean:+.3f}) by {delta:+.3f} (worse than -{tolerance} "
                f"tolerance); keep incumbent.",
            )
        return (
            DECISION_KEEP,
            f"candidate mean ({c_mean:+.3f}) vs incumbent ({i_mean:+.3f}) "
            f"delta {delta:+.3f} within ±{tolerance} tolerance band; "
            f"stability wins ties; keep incumbent.",
        )

    # Fallback: any combination we haven't enumerated → conservative.
    return (
        DECISION_KEEP,
        f"unhandled verdict combination (incumbent={i_verdict}, "
        f"candidate={c_verdict}); conservative default keeps incumbent.",
    )


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Decide whether to promote a LoRA adapter candidate over the incumbent."
    )
    ap.add_argument(
        "--incumbent",
        type=Path,
        required=True,
        help="Path to incumbent adapter's eval-fidelity verdict JSON.",
    )
    ap.add_argument(
        "--candidate",
        type=Path,
        required=True,
        help="Path to candidate adapter's eval-fidelity verdict JSON.",
    )
    ap.add_argument(
        "--tolerance",
        type=float,
        default=0.05,
        help="Absolute-delta band within which we KEEP the incumbent (default 0.05).",
    )
    ap.add_argument(
        "--output-json",
        type=Path,
        default=None,
        help="Write decision verdict JSON to this path (default: stdout only).",
    )
    args = ap.parse_args()

    if args.tolerance < 0:
        print(f"[error] tolerance must be non-negative: {args.tolerance}", file=sys.stderr)
        return 1

    try:
        incumbent = load_verdict(args.incumbent)
        candidate = load_verdict(args.candidate)
    except ValueError as e:
        print(f"[error] {e}", file=sys.stderr)
        return 1

    decision, reasoning = decide(incumbent, candidate, args.tolerance)

    verdict = {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "decision": decision,
        "reasoning": reasoning,
        "incumbent_verdict": incumbent.get("verdict"),
        "candidate_verdict": candidate.get("verdict"),
        "incumbent_mean": get_delta_mean(incumbent),
        "candidate_mean": get_delta_mean(candidate),
        "tolerance": args.tolerance,
    }

    # Pretty-print to stdout for operator + write JSON if requested.
    print(f"[{decision}] {reasoning}")
    print(f"  incumbent: verdict={incumbent.get('verdict')}, "
          f"mean={get_delta_mean(incumbent)}")
    print(f"  candidate: verdict={candidate.get('verdict')}, "
          f"mean={get_delta_mean(candidate)}")

    if args.output_json:
        args.output_json.parent.mkdir(parents=True, exist_ok=True)
        args.output_json.write_text(json.dumps(verdict, indent=2))
        print(f"  decision JSON written to {args.output_json}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
