"""Sprint 11 / US-11.7 — 4-stage Pareto gate cascade stages.

Each module exposes a `run(adapter_path, fixture_path, **kwargs) -> dict`
function returning a per-stage result with shape:

    {
      "stage": int,
      "name": str,
      "status": "PASS" | "REJECT" | "SKIP" | "ABSTAIN",
      "score": float | None,
      "reason": str,
      "details": dict,
    }

The orchestrator (`scripts/stage_cascade.py`) consumes these dicts and
short-circuits on the first REJECT.
"""
