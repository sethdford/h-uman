"""Sprint 11 US-11.1 — Unit tests for the patched `compute_score`.

These tests run under `HU_IS_TEST=1` and never load real model weights.
The patched callable is pure tensor math; all inputs are hand-crafted
`mx.array` fixtures.

AC coverage:
  * AC-11.1.1 — pad positions excluded from the score sum
  * AC-11.1.2 — length normalization: 10-vs-50 token batch divides by non-pad
  * AC-11.1.5 — `HU_IS_TEST=1` short-circuits any real model load
"""
from __future__ import annotations

import importlib
import os
import sys
from pathlib import Path

import pytest

# Make `scripts.mlx_lora_patch` importable as a top-level package.
_REPO_ROOT = Path(__file__).resolve().parent.parent
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

mx = pytest.importorskip("mlx.core")
pytest.importorskip("mlx_lm_lora")

from scripts import mlx_lora_patch  # noqa: E402


# ── HU_IS_TEST discipline ────────────────────────────────────────────────
@pytest.fixture(autouse=True)
def _hu_is_test_env(monkeypatch):
    """AC-11.1.5: tests run with HU_IS_TEST=1 and no real model load is reached."""
    monkeypatch.setenv("HU_IS_TEST", "1")
    # Defensive: if any code path tries to load a real model via mlx_lm.load,
    # explode loudly so the test fails closed.
    try:
        import mlx_lm  # type: ignore

        def _exploder(*_a, **_kw):
            raise AssertionError(
                "mlx_lm.load() called during HU_IS_TEST=1 — US-11.1 unit "
                "tests must not load real model weights."
            )

        monkeypatch.setattr(mlx_lm, "load", _exploder, raising=False)
    except ImportError:
        pass


# ── Patch application ────────────────────────────────────────────────────
@pytest.fixture
def patched_compute_score():
    """Return the patched `compute_score` callable."""
    mlx_lora_patch.apply_length_norm_patch()
    from mlx_lm_lora.trainer import dpo_trainer  # type: ignore

    return dpo_trainer.compute_score


# ── AC-11.1.1: pad positions excluded from score sum ─────────────────────
def test_pad_positions_excluded_from_score_sum(patched_compute_score):
    """Sentinel-perturbation check: pad positions cannot influence the output.

    We build a `scores` tensor with -999.0 at pad positions and finite values
    at real positions, then perturb the pad positions by ±1e6 and confirm
    the output is unchanged within 1e-7.
    """
    # Single-row batch: 6 positions, first 4 real, last 2 pad.
    real = [-1.0, -2.0, -3.0, -4.0]
    pad_a = [-999.0, -999.0]
    pad_b = [+1e6, -1e6]  # perturbed pad values

    scores_a = mx.array([real + pad_a])
    scores_b = mx.array([real + pad_b])
    mask = mx.array([[1.0, 1.0, 1.0, 1.0, 0.0, 0.0]])

    out_a = patched_compute_score(scores_a, mask, "sigmoid")
    out_b = patched_compute_score(scores_b, mask, "sigmoid")

    # The result should equal mean(real) = -2.5, regardless of pad values.
    expected = sum(real) / len(real)
    assert abs(float(out_a[0]) - expected) < 1e-7
    assert abs(float(out_b[0]) - expected) < 1e-7
    # Perturbing pads did NOT change the result.
    assert abs(float(out_a[0]) - float(out_b[0])) < 1e-7


# ── AC-11.1.2: length normalization on 10-vs-50 token batch ──────────────
def test_length_normalization_chosen10_rejected50(patched_compute_score):
    """Chosen seq=10 real tokens, rejected seq=50 positions (10 real + 40 pad).

    Per-token score is constant -2.0 on real positions, arbitrary garbage on
    pad. After length-normalization the resulting margin should be 0 — the
    margin is no longer biased toward the shorter sequence.
    """
    chosen_real = [-2.0] * 10
    chosen_scores = mx.array([chosen_real])
    chosen_mask = mx.array([[1.0] * 10])

    rejected_real = [-2.0] * 10
    rejected_pad = [+777.0] * 40  # garbage values that mask zeroes
    rejected_scores = mx.array([rejected_real + rejected_pad])
    rejected_mask = mx.array([[1.0] * 10 + [0.0] * 40])

    chosen_score = patched_compute_score(chosen_scores, chosen_mask, "sigmoid")
    rejected_score = patched_compute_score(rejected_scores, rejected_mask, "sigmoid")

    # Both should equal -2.0 (per-token avg over real positions).
    assert abs(float(chosen_score[0]) - (-2.0)) < 1e-5
    assert abs(float(rejected_score[0]) - (-2.0)) < 1e-5

    # And therefore the DPO margin (chosen - rejected) is 0, NOT length-biased.
    margin = float(chosen_score[0]) - float(rejected_score[0])
    assert abs(margin) < 1e-5


# ── Regression: IPO path unchanged (already normalized upstream) ─────────
def test_ipo_path_unchanged(patched_compute_score):
    """The patch must not regress the IPO loss type, which was already
    normalizing by non-pad count upstream. With our patch, the formula is the
    same (sum/count); we verify the numerical result still matches expectation.
    """
    real = [-1.0, -2.0, -3.0]
    pad = [-999.0]
    scores = mx.array([real + pad])
    mask = mx.array([[1.0, 1.0, 1.0, 0.0]])

    out = patched_compute_score(scores, mask, "ipo")
    expected = sum(real) / len(real)  # -2.0
    assert abs(float(out[0]) - expected) < 1e-7


# ── Safety: empty mask must not divide by zero ───────────────────────────
def test_empty_mask_does_not_divide_by_zero(patched_compute_score):
    """All-zero mask is degenerate (empty sequence). Output must be finite
    (0.0 in this case) — never NaN/Inf — and must not raise.
    """
    scores = mx.array([[1.0, 2.0, 3.0]])
    mask = mx.array([[0.0, 0.0, 0.0]])

    out = patched_compute_score(scores, mask, "sigmoid")
    val = float(out[0])
    # Result is finite (could be 0.0 because we multiply scores by mask first).
    assert val == val, f"NaN produced on empty mask: {val}"  # NaN != NaN
    # Specifically, (scores * 0).sum() / max(0, 1.0) == 0 / 1 == 0
    assert abs(val) < 1e-7


# ── Safety: patch is idempotent and version-pinned ───────────────────────
def test_patch_idempotent_and_version_pinned(monkeypatch):
    """Calling `apply_length_norm_patch` twice is a no-op the second time.
    Raises RuntimeError if `mlx_lm_lora.__version__` drifts off the supported set.
    """
    # First call applies; second call short-circuits via the sentinel.
    mlx_lora_patch.apply_length_norm_patch()
    assert mlx_lora_patch.is_patched()

    from mlx_lm_lora.trainer import dpo_trainer  # type: ignore

    first = dpo_trainer.compute_score
    mlx_lora_patch.apply_length_norm_patch()
    second = dpo_trainer.compute_score
    assert first is second, "second apply_length_norm_patch() must be a no-op"

    # Version pin: simulate an unsupported upstream by overwriting __version__.
    import mlx_lm_lora  # type: ignore

    monkeypatch.setattr(mlx_lm_lora, "__version__", "99.99.99", raising=False)
    # Force re-application by clearing the sentinel and asking again.
    monkeypatch.setattr(
        dpo_trainer.compute_score,
        mlx_lora_patch._PATCH_SENTINEL,
        False,
        raising=False,
    )
    with pytest.raises(RuntimeError, match="not in the US-11.1 supported"):
        mlx_lora_patch.apply_length_norm_patch()
