"""Sprint 11 US-11.1 — Entry wrapper that applies the length-normalization
patch and then dispatches to `mlx_lm_lora.train.main()`.

This module replaces `python3 -m mlx_lm_lora.train` as the subprocess entry
point used by `scripts/finetune-gemma.py`. The wrapper:

  1. Imports `scripts.mlx_lora_patch` and calls `apply_length_norm_patch()`,
     which monkey-patches `mlx_lm_lora.trainer.dpo_trainer.compute_score` to
     length-normalize for ALL loss types (US-11.1 fix).
  2. Re-exports `sys.argv` unchanged so `mlx_lm_lora.train` sees the same
     CLI as before.
  3. Calls `mlx_lm_lora.train.main()` (or `runpy.run_module` if `main` is
     not exposed) so the upstream training loop runs end-to-end with the
     patched loss.

Invoke as::

    python3 -m scripts.mlx_lora_entry --model ... --train --train-mode dpo ...

The argv shape is identical to what would have been passed to
`mlx_lm_lora.train` directly; only the `-m` target changes.
"""
from __future__ import annotations

import os
import runpy
import sys


def main() -> int:
    # The patch module also auto-applies on import via the HU_DPO_LENGTH_NORM
    # env flag; calling apply_length_norm_patch() here is the explicit-and-
    # idempotent path that surfaces version errors loudly instead of swallowed.
    os.environ.setdefault("HU_DPO_LENGTH_NORM", "1")
    from scripts.mlx_lora_patch import apply_length_norm_patch, is_patched

    apply_length_norm_patch()
    assert is_patched(), "US-11.1 patch failed to apply; aborting before training"

    # Dispatch to mlx_lm_lora.train as if invoked with `-m mlx_lm_lora.train`.
    # `runpy.run_module` is the canonical way to do this and preserves
    # `if __name__ == "__main__":` semantics for the target module.
    runpy.run_module("mlx_lm_lora.train", run_name="__main__", alter_sys=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
