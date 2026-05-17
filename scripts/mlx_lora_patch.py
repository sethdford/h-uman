"""Sprint 11 US-11.1 — Monkey-patch mlx_lm_lora.trainer.dpo_trainer.compute_score
so that ALL loss types (sigmoid/hinge/dpop/ipo) divide the masked per-token
log-probability sum by the non-pad token count, not just the IPO path.

Why this exists
---------------
Upstream `mlx_lm_lora==2.1.0` ships this `compute_score`::

    def compute_score(scores, mask, loss_type):
        token_count = mask.sum(-1)
        return scores.sum(-1) / token_count if loss_type == "ipo" else scores.sum(-1)

For `loss_type == "sigmoid"` (Sprint 8's configuration), the per-token
(pad-masked) log-probabilities are summed without dividing by non-pad count.
The DPO margin therefore compares total log-probability over the sequence,
which is monotonically more negative for longer non-pad sequences. The
optimizer's cheapest move is to make rejected sequences "effectively shorter"
by pushing real tokens to near-zero and letting the pad mask zero them — which
it learns by emitting EOS/pad early at generation time. This is the
length-bias-into-pad-collapse pathway flagged by Meng et al. (SimPO §3) and
the Sprint 11 SOTA-DPO synthesis (sota-dpo.md §4).

The fix is a one-line change: divide by `mx.maximum(mask.sum(-1), 1.0)` for
ALL loss types, not just IPO. We apply this via monkey-patch at import time
rather than vendoring the whole library, so the unwind path is to delete this
file.

Tracking discipline
-------------------
- Pinned to `mlx_lm_lora.__version__ == "2.1.0"`. Bump the supported set in
  `_SUPPORTED_VERSIONS` once we have tested newer upstream against the same
  Sprint 8 regression fixture (US-11.1 AC-11.1.4).
- The patch is idempotent: calling `apply_length_norm_patch()` twice is a
  no-op the second time. A sentinel attribute `_hu_us_11_1_patched` on the
  patched callable makes this safe under repeated imports.
"""
from __future__ import annotations

import os
from typing import Any, Optional

_SUPPORTED_VERSIONS = {"2.1.0"}

# Sentinel attribute name written onto the patched callable to make the patch
# idempotent and externally inspectable (used by tests + diagnostics).
_PATCH_SENTINEL = "_hu_us_11_1_patched"


def _patched_compute_score(scores: Any, mask: Any, loss_type: str) -> Any:
    """Length-normalized replacement for `compute_score`.

    Behavior::

        score = (scores * mask).sum(-1) / max(mask.sum(-1), 1.0)

    Notes:
      * Multiplying by `mask` explicitly is defensive — the upstream caller
        (`get_token_scores`) already zeros pad positions, but doing it again
        here guarantees the contract holds even if a future upstream change
        leaks non-zero pad values into `scores`. The cost is one extra
        elementwise multiply on a tensor that's already resident.
      * `mx.maximum(token_count, 1.0)` floors the divisor at 1.0 to prevent
        NaN on the degenerate empty-mask case. The test
        `test_empty_mask_does_not_divide_by_zero` pins this.
      * IPO's prior behavior (sum/count) is preserved exactly; the only
        change is that sigmoid/hinge/dpop now also divide by count.
    """
    import mlx.core as mx  # type: ignore  # lazy import keeps test discovery fast

    token_count = mask.sum(-1)
    safe_count = mx.maximum(token_count, 1.0)
    return (scores * mask).sum(-1) / safe_count


def apply_length_norm_patch() -> None:
    """Apply the length-normalization patch to `mlx_lm_lora.trainer.dpo_trainer`.

    Raises:
      RuntimeError: if `mlx_lm_lora.__version__` is not in the supported set.
        This is fail-fast against silent upstream drift.

    Idempotent: subsequent calls are no-ops.
    """
    try:
        import mlx_lm_lora  # type: ignore
    except ImportError as exc:  # pragma: no cover - exercised manually
        raise RuntimeError(
            "mlx_lm_lora is not installed; cannot apply US-11.1 patch. "
            "Install it via `pip install mlx-lm-lora==2.1.0`."
        ) from exc

    version = getattr(mlx_lm_lora, "__version__", None)
    if version not in _SUPPORTED_VERSIONS:
        raise RuntimeError(
            f"mlx_lm_lora version {version!r} is not in the US-11.1 supported "
            f"set {_SUPPORTED_VERSIONS!r}. Re-validate the patch against the "
            f"Sprint 8 fixture (tests/fixtures/sprint8_sweep.json) before "
            f"bumping the supported set."
        )

    from mlx_lm_lora.trainer import dpo_trainer  # type: ignore

    # Idempotency guard.
    current = getattr(dpo_trainer, "compute_score", None)
    if current is not None and getattr(current, _PATCH_SENTINEL, False):
        return

    # Tag the replacement so subsequent calls become no-ops.
    setattr(_patched_compute_score, _PATCH_SENTINEL, True)
    dpo_trainer.compute_score = _patched_compute_score


def is_patched(module: Optional[Any] = None) -> bool:
    """Return True iff `compute_score` on the target module has been patched."""
    if module is None:
        from mlx_lm_lora.trainer import dpo_trainer as module  # type: ignore
    fn = getattr(module, "compute_score", None)
    return bool(fn is not None and getattr(fn, _PATCH_SENTINEL, False))


# Apply on import if the env flag is set — used by mlx_lora_entry.py so the
# patch lands before mlx_lm_lora.train.main() runs.
if os.environ.get("HU_DPO_LENGTH_NORM", "1") == "1":
    try:
        apply_length_norm_patch()
    except Exception:  # noqa: BLE001 — surfaces in the entry wrapper
        # We deliberately swallow here so `import scripts.mlx_lora_patch`
        # from inside tests does not require mlx_lm_lora to be installed.
        # The entry wrapper re-runs and surfaces the error.
        pass
