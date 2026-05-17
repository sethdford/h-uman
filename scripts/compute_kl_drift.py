#!/usr/bin/env python3
"""US-11.8 — KL drift sanity gate (mean_i KL(base_i || candidate_i)).

Invoked from `src/ml/lora_ema.c::hu_lora_compute_kl_drift` AFTER the
Pareto gate passes but BEFORE the slow symlink advances. If the
materialized `slow_new` has drifted too far from the base model's output
distribution on the probe set, the EMA is rejected and the previous
slow adapter is preserved.

Inputs:
    --base       path to base model directory (HF format)
    --candidate  path to candidate LoRA safetensors
    --probe-set  path to JSONL where each line has {"prompt": "..."}

Output:
    Single-line JSON: {"kl_nats": <float>, "n_prompts": <int>}

Under HU_IS_TEST the C side never invokes this script (the runner's
subprocess seam returns a mocked stdout). In production smoke runs the
real model is loaded; that path is intentionally out of scope for
Sprint 11 per the design doc §1 KL drift section.

When the base model is unavailable (no GPU, no MLX, no HF transformers),
this script emits kl_nats=0.0 with a debug field so the surrounding
gate logic remains exercisable end-to-end. The C side has no concept
of "skipped KL"; it relies on the upstream caller to set kl_tau high
enough to ignore the result, or to inspect the debug field manually.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Dict


def _emit(payload: Dict[str, Any], exit_code: int = 0) -> int:
    sys.stdout.write(json.dumps(payload))
    sys.stdout.write("\n")
    sys.stdout.flush()
    return exit_code


def _count_prompts(probe_path: str) -> int:
    """Cheap JSONL line count, robust to trailing newlines."""
    try:
        with open(probe_path, "r", encoding="utf-8") as f:
            return sum(1 for line in f if line.strip())
    except OSError:
        return 0


def _try_real_kl(base: str, candidate: str, probe_path: str) -> Any:
    """Best-effort real KL inference.

    Returns:
      None if the dependencies (torch, transformers) are not available
        — the caller falls through to the stub path emitting
        `source: "stub"`.
      Raises NotImplementedError if dependencies ARE available but the
        real inference path is not yet built — this matches
        `scripts/yntp_eval.py::_real_compute_logprob` and surfaces the
        scope gap loudly to operators who DO have torch installed.

    Sprint 11 PR #115 / Bugbot MED fix: previously this function
    silently returned None even when torch+transformers imported
    successfully, masking the scope gap. An operator with torch
    installed would see `source: "stub"` (the dependency-missing
    label) and infer the gate was working — but it was actually a
    fall-through, not a measurement.

    Sprint 12 US-12.3 closes this gap by implementing the real
    `KL(base || candidate)` path against the 200-prompt probe set.
    """
    try:
        # Import lazily so the script can be invoked without the heavy
        # ML stack present (test paths, CI).
        import importlib

        torch = importlib.import_module("torch")  # noqa: F841
        transformers = importlib.import_module("transformers")  # noqa: F841
    except Exception:
        # Honest "no signal" — caller emits source: stub for the C-side
        # gate-stubbed event path.
        return None
    # Dependencies present but real inference not yet wired. Sprint 12
    # US-12.3 will implement this. Raising NotImplementedError makes
    # the runner's subprocess hit `lora_retrain_kl_gate_error` (non-zero
    # exit) rather than silently falling through to the stub path —
    # operators with torch installed see the real status.
    raise NotImplementedError(
        "Real KL drift inference is not yet wired. Sprint 12 US-12.3 "
        "will implement KL(base || candidate) against the 200-prompt "
        "probe set. Until then, this script emits `source: stub` only "
        "in environments without torch/transformers; environments with "
        "those packages installed will hit this NotImplementedError to "
        "make the scope gap visible."
    )


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="compute_kl_drift.py", description=__doc__)
    ap.add_argument("--base", default="", help="base model path or HF id")
    ap.add_argument("--candidate", required=True, help="candidate LoRA safetensors path")
    ap.add_argument("--probe-set", required=True, help="JSONL probe set")
    args = ap.parse_args(argv)

    n_prompts = _count_prompts(args.probe_set)
    if n_prompts == 0:
        return _emit({"ok": False, "reason": "probe_set_empty",
                      "kl_nats": 0.0, "n_prompts": 0}, exit_code=1)

    try:
        real = _try_real_kl(args.base, args.candidate, args.probe_set)
    except NotImplementedError as exc:
        # Sprint 11 PR #115 / Bugbot MED: torch+transformers available
        # but real path not yet wired (Sprint 12 US-12.3). Emit a
        # distinct `source: "not_implemented"` JSON + non-zero exit so
        # the C runner records `lora_retrain_kl_gate_error` instead of
        # silently treating us as stubbed. The kl_nats: 0.0 keeps the
        # JSON parseable; the runner's sentinel + event surfaces the
        # gap.
        return _emit(
            {
                "ok": False,
                "kl_nats": 0.0,
                "n_prompts": n_prompts,
                "source": "not_implemented",
                "reason": str(exc),
            },
            exit_code=2,
        )

    if real is not None and isinstance(real, dict) and "kl_nats" in real:
        return _emit({"ok": True, "kl_nats": float(real["kl_nats"]),
                      "n_prompts": n_prompts, "source": "real"})

    # Stub path: torch/transformers NOT installed. Emit 0.0 nats with
    # source: stub so the C side's lora_ema_parse_kl_is_stub() detection
    # fires and records `lora_retrain_kl_gate_stubbed`. Operators on
    # production deployments without the ML stack see "gate disabled"
    # in dashboards rather than a fake clean PASS.
    return _emit({"ok": True, "kl_nats": 0.0, "n_prompts": n_prompts,
                  "source": "stub"})


if __name__ == "__main__":
    sys.exit(main())
