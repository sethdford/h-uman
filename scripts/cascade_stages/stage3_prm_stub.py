"""Sprint 11 / US-11.7 — Stage 3: Persona PRM (dormant).

Sprint 7 D3 dormancy contract:

  - Default behavior: emit a single visible SKIP line, produce NEUTRAL score
    (None — explicitly NOT 0.0 and NOT 1.0), never gate positive.
  - The `--stage3-stub <float>` CLI override injects a fixture score for
    testing Stage 4 logic only. Even when injected, Stage 3 NEVER produces
    REJECT and NEVER promotes-on-its-own — the orchestrator and Stage 4
    enforce that the dormant path can only abstain.

A null contribution from Stage 3 is excluded from Stage 4 min-aggregation
(it CANNOT pull a verdict up OR down). Documented in `pareto_picker.py`
`--stage-scores` mode and asserted in
`tests/test_pareto_gate.py::test_stage3_skip_does_not_promote_alone`.
"""
from __future__ import annotations

from typing import Optional


def run(
    adapter_path: Optional[str] = None,
    fixture_path: Optional[str] = None,
    stage3_stub: Optional[float] = None,
) -> dict:
    """Return the Stage 3 result dict.

    Args:
        adapter_path: ignored (PRM is not trained — Sprint 12).
        fixture_path: ignored (no PRM scoring path yet).
        stage3_stub: if not None, inject this score for Stage 4-only testing.
            The status becomes "PASS" with the injected score, but the
            orchestrator MUST NOT treat an injected stub as a load-bearing
            positive signal in production.

    Returns:
        Per-stage result dict.
    """
    if stage3_stub is not None:
        # Test seam: inject a fixture score for Stage 4 ensemble testing.
        # The orchestrator+ensemble must treat this exactly like any other
        # PASS-with-score; Stage 3 never produces REJECT on its own.
        return {
            "stage": 3,
            "name": "prm_stub",
            "status": "PASS",
            "score": float(stage3_stub),
            "reason": f"injected stub score {stage3_stub} (--stage3-stub)",
            "details": {"stubbed": True},
        }

    # D3 dormancy default. NEUTRAL means score=None; ensemble excludes
    # null contributions from min-aggregation per Risk 3 mitigation.
    return {
        "stage": 3,
        "name": "prm_stub",
        "status": "SKIP",
        "score": None,
        "reason": "PRM not trained — Sprint 12",
        "details": {"dormant": True},
    }
