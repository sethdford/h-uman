"""Sprint 11 / US-11.7 — Stage 1: PPL floor.

The free, deterministic stage that catches the Sprint 8 iter-200
pad-token collapse for $0. Order is load-bearing — see
`sprints/sprint-11/designs/US-11.7.md` §4 Risk 1.

PPL floor (AC-11.7.1):
    REJECT if adapter_ppl > 3.0 * base_ppl

PPL is derived from per-token NLL:
    PPL = exp(-mean_ll)
(yntp_eval emits mean_ll as per-token log-likelihood, so negating gives NLL.)

Test seams (in order of precedence):
    1. `HU_CASCADE_STAGE1_MOCK_PPL=<adapter_ppl,base_ppl>` env var — direct mock.
    2. `--mock-from-yntp <path>` (caller-supplied) — read a YNTP eval JSON
       output emitted by `scripts/yntp_eval.py --output`.
    3. Cascade fixture file (the JSON passed as `fixture_path`) — for
       end-to-end cascade tests. Reads `adapter_ppl` and `base_ppl` keys.
    4. Real path: invoke `yntp_eval.py` against the adapter; UNIMPLEMENTED
       in this story (the bridge is deferred to US-11.6 step 8 / US-11.7.1).
"""
from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Optional, Tuple

#: AC-11.7.1 threshold. PPL above this multiple of base PPL triggers REJECT.
PPL_FLOOR_RATIO = 3.0


def _read_cascade_fixture(fixture_path: str) -> Optional[Tuple[float, float]]:
    """Try to read (adapter_ppl, base_ppl) from a cascade fixture JSON.

    Returns None if the file isn't a cascade fixture (missing keys).
    """
    p = Path(fixture_path).expanduser()
    if not p.exists():
        return None
    try:
        obj = json.loads(p.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return None
    if not isinstance(obj, dict):
        return None
    if "adapter_ppl" not in obj or "base_ppl" not in obj:
        return None
    try:
        return float(obj["adapter_ppl"]), float(obj["base_ppl"])
    except (TypeError, ValueError):
        return None


def _read_env_mock() -> Optional[Tuple[float, float]]:
    raw = os.environ.get("HU_CASCADE_STAGE1_MOCK_PPL")
    if not raw:
        return None
    try:
        adapter_s, base_s = raw.split(",", 1)
        return float(adapter_s.strip()), float(base_s.strip())
    except (ValueError, AttributeError):
        return None


def _resolve_ppl(
    adapter_path: Optional[str], fixture_path: Optional[str]
) -> Tuple[float, float, str]:
    """Resolve (adapter_ppl, base_ppl, source) using the test-seam precedence."""
    env_pair = _read_env_mock()
    if env_pair is not None:
        return env_pair[0], env_pair[1], "env:HU_CASCADE_STAGE1_MOCK_PPL"

    if fixture_path:
        from_fixture = _read_cascade_fixture(fixture_path)
        if from_fixture is not None:
            return from_fixture[0], from_fixture[1], f"fixture:{fixture_path}"

    # Real MLX path not wired in this story (see docstring).
    raise RuntimeError(
        "Stage 1 has no PPL source. Provide a cascade fixture with "
        "`adapter_ppl` and `base_ppl` keys, or set "
        "HU_CASCADE_STAGE1_MOCK_PPL=<adapter_ppl>,<base_ppl>. Real MLX "
        "inference is deferred to US-11.7.1 / M3 frontier-model bridge."
    )


def run(
    adapter_path: Optional[str] = None,
    fixture_path: Optional[str] = None,
) -> dict:
    """Apply the 3x PPL floor."""
    try:
        adapter_ppl, base_ppl, source = _resolve_ppl(adapter_path, fixture_path)
    except RuntimeError as exc:
        return {
            "stage": 1,
            "name": "ppl_floor",
            "status": "ABSTAIN",
            "score": None,
            "reason": str(exc),
            "details": {"source": "none"},
        }

    if base_ppl <= 0:
        return {
            "stage": 1,
            "name": "ppl_floor",
            "status": "ABSTAIN",
            "score": None,
            "reason": f"base_ppl={base_ppl} is non-positive; cannot apply ratio gate",
            "details": {"source": source, "adapter_ppl": adapter_ppl, "base_ppl": base_ppl},
        }

    ratio = adapter_ppl / base_ppl
    # Stage 4 ensemble score for PPL: higher is better. Use 1 - ratio/floor,
    # clipped to [0, 1]. A perfect adapter (ratio=1) scores ~0.67; one at
    # the floor (ratio=3) scores 0.0; above-floor is REJECT and never feeds
    # into the ensemble.
    score = max(0.0, 1.0 - (ratio / PPL_FLOOR_RATIO))

    details = {
        "source": source,
        "adapter_ppl": adapter_ppl,
        "base_ppl": base_ppl,
        "ratio": ratio,
        "floor_ratio": PPL_FLOOR_RATIO,
    }

    if ratio > PPL_FLOOR_RATIO:
        return {
            "stage": 1,
            "name": "ppl_floor",
            "status": "REJECT",
            "score": None,
            "reason": (
                f"adapter_ppl={adapter_ppl:.3f} / base_ppl={base_ppl:.3f} = "
                f"{ratio:.3f}x > {PPL_FLOOR_RATIO}x (AC-11.7.1 PPL floor)"
            ),
            "details": details,
        }

    return {
        "stage": 1,
        "name": "ppl_floor",
        "status": "PASS",
        "score": score,
        "reason": (
            f"adapter_ppl={adapter_ppl:.3f} / base_ppl={base_ppl:.3f} = "
            f"{ratio:.3f}x <= {PPL_FLOOR_RATIO}x"
        ),
        "details": details,
    }
