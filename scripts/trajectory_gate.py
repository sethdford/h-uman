#!/usr/bin/env python3
"""
Pure curve-gate for the longitudinal on-device personalization trajectory.

This module is the heart of the SOTA claim and is deliberately PURE — no I/O,
no model, no clock — so every branch is unit-testable without running a 31B
model (mirrors the security-predicate-extraction discipline: the *decision* is
a pure function; the *measurement* lives elsewhere).

A "generation" is one adapter in the trajectory:
    gen 0  = the base model (no adapter)
    gen 1.. = successive adapters retrained from accumulated implicit feedback

The gate answers ONE question: "is this trajectory a credible, guard-railed
improvement, or not?" It checks three orthogonal axes and PASSES iff all hold:

  1. Curve rising (AC-2): fidelity is monotone-non-decreasing *within CI
     tolerance*. No generation may drop more than 1 stderr below the running
     maximum fidelity seen so far. Strict monotonicity is the WRONG gate —
     20-30-fixture fidelity is noisy; within-CI monotonicity is the honest one.

  2. Base-capability preserved (AC-3): every generation's general-ability score
     stays >= gen0 - epsilon. This is the guard that makes a voice-gain bought
     by instruction-following collapse a FAIL, not SOTA. It encodes the
     2026-05-25 scale=20 incident (lora-scale-default-or-die.md) as a gate.

  3. Final-generation floor (AC-4): the newest generation's fidelity is at or
     above a published threshold (default 0.80).

See docs/plans/2026-05-28-longitudinal-personalization-sota/design.md (C3).
"""

from __future__ import annotations

from dataclasses import dataclass, field


# ── Configuration ──────────────────────────────────────────────────────────

DEFAULT_BASE_CAPABILITY_EPSILON = 0.05  # AC-3: allowed base-capability slack
DEFAULT_FINAL_FIDELITY_FLOOR = 0.80     # AC-4: newest gen must clear this
DEFAULT_STDERR_TOLERANCE = 1.0          # AC-2: # of stderrs below running max


@dataclass
class TrajectoryGateConfig:
    base_capability_epsilon: float = DEFAULT_BASE_CAPABILITY_EPSILON
    final_fidelity_floor: float = DEFAULT_FINAL_FIDELITY_FLOOR
    stderr_tolerance: float = DEFAULT_STDERR_TOLERANCE


@dataclass
class TrajectoryGateResult:
    verdict: str                       # "PASS" | "FAIL" | "SKIP"
    failing_axis: str | None = None    # "curve" | "base_capability" | "final_floor"
    failing_gen: int | None = None     # index of the generation that failed
    details: list[str] = field(default_factory=list)

    def to_dict(self) -> dict:
        return {
            "verdict": self.verdict,
            "failing_axis": self.failing_axis,
            "failing_gen": self.failing_gen,
            "details": self.details,
        }


def _stderr_of(gen: dict) -> float:
    """Best-effort stderr for a generation.

    Prefers an explicit "fidelity_stderr"; else derives it from a
    "fidelity_ci" [lo, hi] as (hi - lo) / (2 * 1.96) — matching the
    convention in eval_fidelity_nightly.py. Falls back to 0.0 (treat the
    point estimate as exact) when neither is present.
    """
    if "fidelity_stderr" in gen and gen["fidelity_stderr"] is not None:
        return max(0.0, float(gen["fidelity_stderr"]))
    ci = gen.get("fidelity_ci")
    if isinstance(ci, (list, tuple)) and len(ci) == 2 and ci[0] is not None and ci[1] is not None:
        width = float(ci[1]) - float(ci[0])
        if width > 0:
            return width / (2 * 1.96)
    return 0.0


def evaluate_trajectory_gate(
    generations: list[dict],
    cfg: TrajectoryGateConfig | None = None,
) -> TrajectoryGateResult:
    """Evaluate the trajectory gate over an ordered list of generations.

    Each generation dict must carry at least:
        "gen"              : int (generation index; gen 0 is base)
        "fidelity_mean"    : float in [0, 1]
        "base_capability"  : float in [0, 1]
    and optionally "fidelity_ci": [lo, hi] or "fidelity_stderr": float.

    Returns a TrajectoryGateResult. Verdict is SKIP when there are fewer than
    two generations (no curve to judge yet). The first failing axis encountered
    short-circuits, so the result names the *earliest* problem — base-capability
    is checked before the curve so a scale=20-style collapse surfaces as a
    base_capability failure rather than being masked by a fidelity rise.
    """
    cfg = cfg or TrajectoryGateConfig()

    if not generations:
        return TrajectoryGateResult(
            verdict="SKIP", details=["no generations recorded yet"]
        )

    # Order defensively by gen index so callers can't perturb the gate by
    # appending out of order.
    gens = sorted(generations, key=lambda g: int(g["gen"]))

    if len(gens) < 2:
        return TrajectoryGateResult(
            verdict="SKIP",
            details=[f"only {len(gens)} generation(s); need >=2 for a curve"],
        )

    base_capability_0 = float(gens[0]["base_capability"])
    base_floor = base_capability_0 - cfg.base_capability_epsilon
    details: list[str] = []

    # ── Axis 1 (checked first): base-capability preserved at EVERY gen ──
    # Checked before the curve so a fidelity-up / capability-down adapter
    # (the scale=20 failure mode) is reported as the base_capability failure
    # it actually is, not hidden behind a passing curve.
    for g in gens:
        bc = float(g["base_capability"])
        if bc < base_floor:
            return TrajectoryGateResult(
                verdict="FAIL",
                failing_axis="base_capability",
                failing_gen=int(g["gen"]),
                details=[
                    f"gen {g['gen']} base_capability {bc:.3f} < floor "
                    f"{base_floor:.3f} (gen0 {base_capability_0:.3f} - eps "
                    f"{cfg.base_capability_epsilon}). Voice gain bought by "
                    f"base-capability collapse is a regression, not SOTA "
                    f"(lora-scale-default-or-die.md)."
                ],
            )
    details.append(
        f"base_capability preserved at all {len(gens)} gens "
        f"(floor {base_floor:.3f})"
    )

    # ── Axis 2: curve rising within CI tolerance ──
    running_max = float(gens[0]["fidelity_mean"])
    for g in gens[1:]:
        fid = float(g["fidelity_mean"])
        tol = cfg.stderr_tolerance * _stderr_of(g)
        if fid < running_max - tol:
            return TrajectoryGateResult(
                verdict="FAIL",
                failing_axis="curve",
                failing_gen=int(g["gen"]),
                details=[
                    f"gen {g['gen']} fidelity {fid:.3f} dropped more than "
                    f"{cfg.stderr_tolerance:g} stderr ({tol:.3f}) below the "
                    f"running max {running_max:.3f} — curve not rising."
                ],
            )
        running_max = max(running_max, fid)
    details.append(
        f"curve non-decreasing within {cfg.stderr_tolerance:g} stderr "
        f"(running max {running_max:.3f})"
    )

    # ── Axis 3: newest generation clears the published floor ──
    final = gens[-1]
    final_fid = float(final["fidelity_mean"])
    if final_fid < cfg.final_fidelity_floor:
        return TrajectoryGateResult(
            verdict="FAIL",
            failing_axis="final_floor",
            failing_gen=int(final["gen"]),
            details=details
            + [
                f"final gen {final['gen']} fidelity {final_fid:.3f} < "
                f"published floor {cfg.final_fidelity_floor:.3f}"
            ],
        )
    details.append(
        f"final gen {final['gen']} fidelity {final_fid:.3f} >= floor "
        f"{cfg.final_fidelity_floor:.3f}"
    )

    return TrajectoryGateResult(verdict="PASS", details=details)
