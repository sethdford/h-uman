"""Sprint 11 / US-11.7 — Stage 2: Coherence judge.

A local Gemma-4-E2B-as-judge that scores coherence on a small prompt set.

Gate rules (per AC-11.7.2 + design §6 OQ-4 — both apply):
    REJECT if pad_rate >= 0.50 over coherence prompts
    REJECT if mean coherence score < 0.70

Three return states (per Risk 2 mitigation, design §4):
    PASS    — judge ran cleanly; both gates passed
    REJECT  — judge ran cleanly; at least one gate triggered
    ABSTAIN — judge itself failed (missing model, OOM, NaN). Stage 4
              receives no score; verdict can be at most DEFER.

Test seam — `HU_CASCADE_STAGE2_MOCK`:
    JSON env var with shape:
      {
        "scores": [<float>, ...],            # per-prompt coherence 0..1
        "pad_outputs": [<bool>, ...],         # per-prompt has_pad
      }
    OR a JSON array of floats (scores only, pad_outputs default to []).

Cascade fixture fallback: if the env var is unset, this stage reads
`coherence_scores` and `coherence_pad_outputs` arrays from the cascade
fixture JSON pointed to by `fixture_path`. This lets the end-to-end
scenario tests (sprint8_iter200, iter60_dirty, iter60_padfix) drive both
Stage 1 and Stage 2 from the same fixture.

In production (no env mock, no cascade fixture with coherence arrays),
this stage would load the local Gemma-4-E2B and run the G-Eval prompt.
That path is deferred — `_call_judge` raises NotImplementedError and the
orchestrator surfaces it as ABSTAIN.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
from typing import List, Optional, Tuple

#: AC-11.7.2 — pad rate >= 0.50 → REJECT.
PAD_RATE_REJECT_THRESHOLD = 0.50

#: Design §6 OQ-4 — mean coherence < 0.70 → REJECT.
COHERENCE_REJECT_THRESHOLD = 0.70


def _read_env_mock() -> Optional[Tuple[List[float], List[bool]]]:
    raw = os.environ.get("HU_CASCADE_STAGE2_MOCK")
    if not raw:
        return None
    try:
        obj = json.loads(raw)
    except json.JSONDecodeError:
        return None
    if isinstance(obj, list):
        # Array form: just scores.
        try:
            scores = [float(s) for s in obj]
        except (TypeError, ValueError):
            return None
        return scores, []
    if isinstance(obj, dict):
        scores_raw = obj.get("scores", [])
        pads_raw = obj.get("pad_outputs", [])
        try:
            scores = [float(s) for s in scores_raw]
            pads = [bool(p) for p in pads_raw]
        except (TypeError, ValueError):
            return None
        return scores, pads
    return None


def _read_cascade_fixture(fixture_path: str) -> Optional[Tuple[List[float], List[bool]]]:
    p = Path(fixture_path).expanduser()
    if not p.exists():
        return None
    try:
        obj = json.loads(p.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return None
    if not isinstance(obj, dict):
        return None
    scores_raw = obj.get("coherence_scores")
    pads_raw = obj.get("coherence_pad_outputs", [])
    if scores_raw is None:
        return None
    try:
        scores = [float(s) for s in scores_raw]
        pads = [bool(p) for p in pads_raw]
    except (TypeError, ValueError):
        return None
    return scores, pads


def _call_judge(
    adapter_path: Optional[str], fixture_path: Optional[str]
) -> Tuple[List[float], List[bool]]:
    """Resolve coherence scores using the test-seam precedence.

    Raises RuntimeError if no source is available — orchestrator maps to ABSTAIN.
    """
    env_pair = _read_env_mock()
    if env_pair is not None:
        return env_pair

    if fixture_path:
        from_fixture = _read_cascade_fixture(fixture_path)
        if from_fixture is not None:
            return from_fixture

    raise RuntimeError(
        "Stage 2 has no coherence source. Set HU_CASCADE_STAGE2_MOCK or "
        "provide a cascade fixture with `coherence_scores` / "
        "`coherence_pad_outputs` arrays. The real Gemma-4-E2B judge "
        "backend is deferred to a follow-on smoke script."
    )


def healthcheck() -> dict:
    """Verify the judge can be invoked. In CI/test paths this is a no-op
    that always returns OK because the mock seam never fails on its own.

    Returns:
        {"ok": bool, "reason": str}
    """
    return {"ok": True, "reason": "mock-seam: no live model required"}


def run(
    adapter_path: Optional[str] = None,
    fixture_path: Optional[str] = None,
) -> dict:
    """Score coherence and apply the AC-11.7.2 + coherence-mean gates."""
    health = healthcheck()
    if not health["ok"]:
        return {
            "stage": 2,
            "name": "coherence",
            "status": "ABSTAIN",
            "score": None,
            "reason": f"healthcheck failed: {health['reason']}",
            "details": {"healthcheck": health},
        }

    try:
        scores, pad_outputs = _call_judge(adapter_path, fixture_path)
    except RuntimeError as exc:
        return {
            "stage": 2,
            "name": "coherence",
            "status": "ABSTAIN",
            "score": None,
            "reason": str(exc),
            "details": {"source": "none"},
        }

    if not scores:
        return {
            "stage": 2,
            "name": "coherence",
            "status": "ABSTAIN",
            "score": None,
            "reason": "judge returned empty score list",
            "details": {"n_prompts": 0},
        }

    n = len(scores)
    # Validate scores are in-range; NaN/inf would slip past min/max.
    for s in scores:
        if not (isinstance(s, float) and s == s and -1e9 < s < 1e9):
            return {
                "stage": 2,
                "name": "coherence",
                "status": "ABSTAIN",
                "score": None,
                "reason": f"judge returned non-finite score {s}",
                "details": {"scores": scores},
            }

    mean_score = sum(scores) / n
    # Sprint 11 PR #115 / Bugbot HIGH fix: pad_rate denominator must be
    # len(pad_outputs), not n (=len(scores)). When pad_outputs has fewer
    # entries than scores, dividing by n silently dilutes the rate below
    # the 50% PAD_RATE_REJECT_THRESHOLD — letting an adapter with heavy
    # pad-token leakage pass the exact Sprint 8 regression-guard fixture
    # this gate is supposed to catch.
    #
    # Semantics:
    # - len(pad_outputs) == 0: no pad signal (array-form env mock, or
    #   fixture without pad column). Honest "no signal" → 0.0.
    # - len(pad_outputs) == n: full signal, compute rate normally.
    # - 0 < len(pad_outputs) < n: data integrity bug. ABSTAIN — refuse to
    #   compute a rate from mismatched arrays.
    if not pad_outputs:
        pad_rate = 0.0
    elif len(pad_outputs) != n:
        return {
            "stage": 2,
            "name": "coherence",
            "status": "ABSTAIN",
            "score": None,
            "reason": (
                f"pad_outputs length {len(pad_outputs)} != scores length {n}; "
                f"refusing to compute pad_rate from mismatched arrays"
            ),
            "details": {
                "n_scores": n,
                "n_pad_outputs": len(pad_outputs),
                "source": "env:HU_CASCADE_STAGE2_MOCK"
                if os.environ.get("HU_CASCADE_STAGE2_MOCK")
                else f"fixture:{fixture_path}",
            },
        }
    else:
        pad_rate = sum(1 for p in pad_outputs if p) / len(pad_outputs)

    details = {
        "source": "env:HU_CASCADE_STAGE2_MOCK"
        if os.environ.get("HU_CASCADE_STAGE2_MOCK")
        else f"fixture:{fixture_path}",
        "n_prompts": n,
        "mean_coherence": mean_score,
        "pad_rate": pad_rate,
        "pad_threshold": PAD_RATE_REJECT_THRESHOLD,
        "coherence_threshold": COHERENCE_REJECT_THRESHOLD,
    }

    if pad_rate >= PAD_RATE_REJECT_THRESHOLD:
        return {
            "stage": 2,
            "name": "coherence",
            "status": "REJECT",
            "score": None,
            "reason": (
                f"pad_rate={pad_rate:.3f} >= {PAD_RATE_REJECT_THRESHOLD} "
                "(AC-11.7.2 pad gate)"
            ),
            "details": details,
        }

    if mean_score < COHERENCE_REJECT_THRESHOLD:
        return {
            "stage": 2,
            "name": "coherence",
            "status": "REJECT",
            "score": None,
            "reason": (
                f"mean_coherence={mean_score:.3f} < "
                f"{COHERENCE_REJECT_THRESHOLD} (coherence floor)"
            ),
            "details": details,
        }

    return {
        "stage": 2,
        "name": "coherence",
        "status": "PASS",
        "score": mean_score,
        "reason": (
            f"mean_coherence={mean_score:.3f} >= {COHERENCE_REJECT_THRESHOLD} "
            f"and pad_rate={pad_rate:.3f} < {PAD_RATE_REJECT_THRESHOLD}"
        ),
        "details": details,
    }
