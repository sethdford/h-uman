#!/usr/bin/env python3
"""Sprint 11 / US-11.7 — 4-stage Pareto gate cascade orchestrator.

Composes the cascade:

    Stage 1: PPL floor          (cascade_stages/stage1_ppl.py)
    Stage 2: Coherence judge    (cascade_stages/stage2_coherence.py)
    Stage 3: Persona PRM (D3)   (cascade_stages/stage3_prm_stub.py)
    Stage 4: Pareto ensemble    (pareto_picker.ensemble_min_aggregate)

Fail-fast semantics:

  - Stages 1 and 2 short-circuit on REJECT — Stages 3 and 4 are NOT invoked
    when an upstream stage REJECTs. The output JSON marks them
    `"status": "skipped_due_to_short_circuit"` so the per-stage breakdown
    stays informative (operators can see exactly where the cascade stopped).
  - Stage 3 is dormant (Sprint 7 D3): SKIP by default, contributes null to
    Stage 4 min-aggregation. The `--stage3-stub <float>` flag injects a
    fixture score for Stage 4-only testing.
  - Stage 2 ABSTAIN (judge crash) is NOT a REJECT — it allows Stages 3-4
    to proceed but Stage 4 receives no coherence score, capping the final
    verdict at DEFER.

Cascade order is load-bearing — re-ordering Stages 1 and 2 will fail
AC-11.7.3 (the Sprint 8 iter-200 regression guard test asserts the
rejection happens AT STAGE 1 specifically). See
sprints/sprint-11/designs/US-11.7.md §4 Risk 1.

Exit codes:
    0 = PROMOTE
    1 = DEFER
    2 = REJECT

Output: a single JSON object on stdout with the per-stage breakdown.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import sys
from pathlib import Path
from typing import List, Optional

_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parent


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# Lazy-load the stage modules + pareto_picker. Avoids `sys.path` mutation
# while keeping imports explicit and order-of-loading deterministic.
_STAGE1 = _load("stage1_ppl", _HERE / "cascade_stages" / "stage1_ppl.py")
_STAGE2 = _load("stage2_coherence", _HERE / "cascade_stages" / "stage2_coherence.py")
_STAGE3 = _load("stage3_prm_stub", _HERE / "cascade_stages" / "stage3_prm_stub.py")
_PARETO = _load("pareto_picker", _HERE / "pareto_picker.py")


# Cascade order is load-bearing — see module docstring + Risk 1 in design doc.
# DO NOT reorder the three calls in `run_cascade` (line ~100, ~116, ~131) — Stage 1
# (PPL floor) is the only stage that catches the Sprint 8 iter-200 pad collapse
# deterministically and for ¢0. AC-11.7.3 asserts the rejection happens AT STAGE 1
# specifically (not "somewhere in the cascade"), so reordering will fail
# `test_sprint8_iter200_rejected_by_gate` + `test_stage1_short_circuits_stage2_not_invoked`.
#
# Sprint 11 / US-11.7 critic-HIGH #2 fix: the previous version of this file declared
# a `_CASCADE_ORDER = ("stage1_ppl", ...)` tuple here and the design doc Risk 1
# mitigation pointed at it as the single source of truth for stage order. That was a
# lie — the tuple was never used by `run_cascade`, which hardcoded the order
# imperatively. Editing the tuple would have changed a dead string and shipped a
# reordered cascade silently. The tuple is removed; ordering is enforced exclusively
# by the call sequence in `run_cascade` and pinned by the AC-11.7.3 tests.


def _skipped_stage(stage_num: int, name: str, reason: str) -> dict:
    return {
        "stage": stage_num,
        "name": name,
        "status": "skipped_due_to_short_circuit",
        "score": None,
        "reason": reason,
        "details": {},
    }


def run_cascade(
    adapter_path: Optional[str],
    fixture_path: Optional[str],
    stage3_stub: Optional[float] = None,
) -> dict:
    """Run the full cascade and return the result dict.

    Returns:
        {
          "adapter_path": ...,
          "fixture_path": ...,
          "stages": [stage1, stage2, stage3, stage4],
          "final_verdict": "PROMOTE"|"DEFER"|"REJECT",
          "exit_code": 0|1|2,
        }
    """
    stages: List[dict] = []

    # ── Stage 1: PPL floor ────────────────────────────────────────────
    s1 = _STAGE1.run(adapter_path=adapter_path, fixture_path=fixture_path)
    stages.append(s1)

    # Sprint 11 / US-11.7 critic-CRITICAL #1 fix: Stage 1 ABSTAIN must NOT
    # fall through. PPL is the cheapest and most deterministic guard — if it
    # cannot run (no `base_ppl`/`adapter_ppl` in fixture, no env mock), we
    # have zero evidence about pad-token collapse and MUST refuse to promote.
    # The previous version short-circuited only on `REJECT`, which meant a
    # malformed fixture with no PPL data but a passing coherence judge could
    # produce a PROMOTE verdict with Stage 1 having never observed the
    # adapter. Treating Stage 1 ABSTAIN as a hard REJECT is symmetric with
    # the Sprint 8 regression guard's intent — Stage 1 is where pad-token
    # collapse MUST be caught, and we cannot promote past a stage that
    # never ran.
    if s1["status"] in ("REJECT", "ABSTAIN"):
        short_reason = (
            "Stage 1 REJECTed" if s1["status"] == "REJECT"
            else "Stage 1 ABSTAINed (no PPL evidence — promoted to REJECT per critic-CRITICAL #1)"
        )
        stages.append(_skipped_stage(2, "coherence", short_reason))
        stages.append(_skipped_stage(3, "prm_stub", short_reason))
        stages.append(_skipped_stage(4, "ensemble", short_reason))
        return {
            "adapter_path": adapter_path,
            "fixture_path": fixture_path,
            "stages": stages,
            "final_verdict": "REJECT",
            "exit_code": 2,
        }

    # ── Stage 2: Coherence judge ──────────────────────────────────────
    s2 = _STAGE2.run(adapter_path=adapter_path, fixture_path=fixture_path)
    stages.append(s2)

    if s2["status"] == "REJECT":
        stages.append(_skipped_stage(3, "prm_stub", "Stage 2 REJECTed"))
        stages.append(_skipped_stage(4, "ensemble", "Stage 2 REJECTed"))
        return {
            "adapter_path": adapter_path,
            "fixture_path": fixture_path,
            "stages": stages,
            "final_verdict": "REJECT",
            "exit_code": 2,
        }

    # ── Stage 3: Persona PRM (dormant by default) ─────────────────────
    s3 = _STAGE3.run(
        adapter_path=adapter_path,
        fixture_path=fixture_path,
        stage3_stub=stage3_stub,
    )
    stages.append(s3)

    # ── Stage 4: Pareto ensemble (min-aggregation) ────────────────────
    # Assemble per-judge scores. Convention:
    #   "ppl"       -> Stage 1 score (1 - ratio/floor), None if ABSTAIN
    #   "coherence" -> Stage 2 mean coherence score, None if ABSTAIN
    #   "prm"       -> Stage 3 stubbed score, or None when SKIP/dormant
    stage_scores = {
        "ppl": s1.get("score") if s1["status"] == "PASS" else None,
        "coherence": s2.get("score") if s2["status"] == "PASS" else None,
        "prm": s3.get("score") if s3["status"] == "PASS" else None,
    }
    agg = _PARETO.ensemble_min_aggregate(stage_scores)

    # Stage 2 ABSTAIN policy: cap final verdict at DEFER. Per Risk 2 (design §4),
    # the judge never silently passes nor silently rejects. If Stage 2 abstained
    # (judge crash), Stage 4 receives no coherence score and the verdict is at
    # most DEFER, regardless of how favorable other judges are.
    if s2["status"] == "ABSTAIN" and agg["verdict"] == "PROMOTE":
        agg = {**agg, "verdict": "DEFER"}
        agg["per_judge"] = dict(agg["per_judge"])
        agg["per_judge"]["__stage2_abstain_cap__"] = {
            "score": None,
            "verdict": "CAP",
        }

    s4 = {
        "stage": 4,
        "name": "ensemble",
        "status": agg["verdict"],  # PROMOTE/DEFER/REJECT (verbatim per-stage status)
        "score": None,
        "reason": (
            f"min-aggregation over {agg['n_contributing']} contributing judges "
            f"({agg['n_null']} null); worst={agg.get('min_judge')}"
        ),
        "details": agg,
    }
    stages.append(s4)

    final = agg["verdict"]
    exit_code = {"PROMOTE": 0, "DEFER": 1, "REJECT": 2}[final]
    return {
        "adapter_path": adapter_path,
        "fixture_path": fixture_path,
        "stages": stages,
        "final_verdict": final,
        "exit_code": exit_code,
    }


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        prog="stage_cascade.py",
        description=(
            "Sprint 11 / US-11.7 — 4-stage Pareto gate cascade orchestrator. "
            "Composes PPL floor + coherence judge + persona PRM stub + "
            "ensemble min-aggregation. Fail-fast on the first REJECT."
        ),
    )
    ap.add_argument(
        "--adapter",
        type=str,
        default=None,
        help=(
            "Path to adapter for production runs. In CI/test paths the "
            "cascade fixture (--fixture) supplies all stage inputs."
        ),
    )
    ap.add_argument(
        "--fixture",
        type=str,
        default=None,
        help=(
            "Cascade fixture JSON (e.g. tests/fixtures/cascade/sprint8_iter200.json) "
            "providing adapter_ppl/base_ppl + coherence arrays for deterministic "
            "end-to-end tests. Test seam — see "
            "sprints/sprint-11/designs/US-11.7.md §3."
        ),
    )
    ap.add_argument(
        "--stage3-stub",
        type=float,
        default=None,
        help=(
            "Inject a Stage 3 PRM stub score for testing Stage 4 ensemble "
            "logic only. Default (no flag): Stage 3 emits SKIP and contributes "
            "null to the ensemble (Sprint 7 D3 dormancy)."
        ),
    )
    ap.add_argument(
        "--output",
        type=str,
        default=None,
        help="Optional path to write the result JSON. Default: stdout.",
    )
    args = ap.parse_args(argv)

    if not args.adapter and not args.fixture:
        print(
            "ERROR: --adapter or --fixture is required.",
            file=sys.stderr,
        )
        return 2

    result = run_cascade(
        adapter_path=args.adapter,
        fixture_path=args.fixture,
        stage3_stub=args.stage3_stub,
    )
    payload = json.dumps(result, indent=2, sort_keys=True)
    if args.output:
        Path(args.output).expanduser().write_text(payload + "\n", encoding="utf-8")
    else:
        print(payload)
    return result["exit_code"]


if __name__ == "__main__":
    sys.exit(main())
