"""Sprint 11 US-11.1 — Sprint 8 regression guard for the Pareto gate.

Confirms that:
  * AC-11.1.4 (regression): Sprint 8's iter-200 "broken adapter" (pad_rate=80%
    on a lexical delta of +0.046) is REJECTED by the gate, despite its
    positive delta. This proves the Pareto multiplicative penalty cannot be
    fooled by the broken-output-with-high-fingerprint failure mode that
    Sprint 8 surfaced.
  * AC-11.1.3 baseline: Sprint 8's iter-60 (the best empirical checkpoint,
    delta=+0.019 with 40% pad) is classified as DEFER — not REJECT solely
    due to pad rate. This pins the gate's calibration.
  * AC-11.1.3 post-patch: a synthetic post-US-11.1 row (delta=+0.04,
    pad_rate=0.05) is classified as PROMOTE. The synthetic row is the
    operational signal that "the patch worked"; an actual training-loop
    measurement is out of scope for US-11.1 (deferred to US-11.6).
"""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parent.parent
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

# pareto_picker is a hyphen-free filename so a normal import works after
# the sys.path manipulation above. We import from the `scripts` package.
from scripts.pareto_picker import classify  # noqa: E402


_FIXTURE = (
    Path(__file__).resolve().parent / "fixtures" / "sprint8_sweep.json"
)


@pytest.fixture(autouse=True)
def _hu_is_test_env(monkeypatch):
    """AC-11.1.5: this is pure JSON+classify math; no model loading possible."""
    monkeypatch.setenv("HU_IS_TEST", "1")


@pytest.fixture
def sweep():
    with _FIXTURE.open() as f:
        return json.load(f)


def _row(sweep: dict, name: str) -> dict:
    for r in sweep["checkpoints"]:
        if r["name"] == name:
            return r
    raise KeyError(name)


# ── AC-11.1.4: Sprint 8 iter-200 broken adapter must FAIL the gate ───────
def test_sprint8_iter200_broken_adapter_fails_gate(sweep):
    """Iter-200 has delta=+0.046 (crosses PROMOTE delta floor of +0.03) but
    pad_rate=80% (far above the 10% PROMOTE ceiling AND the 50% DEFER ceiling).
    The Pareto gate must REJECT it.
    """
    row = _row(sweep, "iter200_broken")
    assert row["fidelity_delta"] >= 0.03, (
        "fixture invariant: iter-200 delta must still be above the PROMOTE "
        "floor; otherwise this test is no longer guarding the gameable case"
    )
    assert row["pad_failure_rate"] >= 0.5, (
        "fixture invariant: iter-200 pad rate must still be above the DEFER "
        "ceiling for this guard to be meaningful"
    )

    verdict = classify(row["fidelity_delta"], row["pad_failure_rate"])
    assert verdict == "REJECT", (
        f"Sprint 8 iter-200 broken adapter must be REJECTED by the Pareto "
        f"gate (it has high delta but 80% pad). Got verdict={verdict!r}."
    )


# ── AC-11.1.3 baseline: iter-60 is DEFER (not REJECT solely due to pad) ──
def test_sprint8_iter60_best_classified_as_defer_or_reject(sweep):
    """Iter-60 (Smoke #3 best) has delta=+0.019 (in [0.01, 0.03)) and
    pad_rate=40% (under 50% DEFER ceiling). Expected: DEFER.
    Critically: NOT REJECT-solely-due-to-pad.
    """
    row = _row(sweep, "iter60")
    verdict = classify(row["fidelity_delta"], row["pad_failure_rate"])
    assert verdict == "DEFER", (
        f"Sprint 8 iter-60 best checkpoint must be classified DEFER "
        f"(delta=+0.019, pad=40%). Got verdict={verdict!r}."
    )


# ── AC-11.1.3 post-patch: simulated US-11.1+US-11.4 run promotes ────────
def test_post_patch_simulated_run_promotes():
    """A synthetic post-US-11.1 row (delta=+0.04, pad_rate=0.05) is the
    operational signal that the length-normalization patch combined with
    Wave 1's DPOP loss is on track. We don't actually train here (that's
    US-11.6's job); we just assert the classifier wires through.
    """
    verdict = classify(0.04, 0.05)
    assert verdict == "PROMOTE", (
        f"A delta=+0.04 / pad=5% adapter must be PROMOTE. Got {verdict!r}."
    )


# ── Negative: a delta-only positive with no pad ceiling guard must NOT promote ──
def test_high_delta_alone_does_not_promote_when_pad_high():
    """Belt-and-braces: explicitly assert the Pareto gate's pad ceiling is
    load-bearing. delta=+0.10 (5× promotion floor) with pad=60% must still
    REJECT — pad ceiling is non-negotiable.
    """
    verdict = classify(0.10, 0.60)
    assert verdict == "REJECT", (
        f"delta alone cannot promote when pad rate exceeds the DEFER "
        f"ceiling. Got verdict={verdict!r}."
    )
