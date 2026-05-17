#!/usr/bin/env python3
"""US-11.8 — OFS-DPO dual fast/slow LoRA EMA helper.

Computes `slow_new = alpha * slow + (1 - alpha) * fast` per-tensor over
the LoRA A/B matrices (and, for DoRA, the magnitude vector m), writing
the result atomically via tmp + rename. Validates compatibility metadata
(rank / target modules / base model hash) before touching tensors.

Modes:

  Cold start (no prior slow):
    lora_ema.py --cold-start --fast <fast.st> --out <slow_out.st>

    Action: copy `fast` to `out` atomically. Emits {"ok": true,
    "cold_start": true}.

  Warm EMA:
    lora_ema.py --slow <slow.st> --fast <fast.st> --alpha 0.95 --out <out.st>

    Action: per-tensor weighted average. Emits {"ok": true} on success
    or {"ok": false, "reason": "<...>"} on compat mismatch.

Exit codes:
    0 = success ({"ok": true})
    1 = compat / IO failure ({"ok": false, "reason": ...})

The actual tensor read/write uses the `safetensors` Python package when
available; when not installed, we treat any .safetensors file as a
filesystem blob and the cold-start path still works as a copy. The warm
EMA path requires `safetensors` and errors out with reason="safetensors_unavailable"
when missing.

This script is invoked from C via the runner's subprocess seam. Under
HU_IS_TEST the C side mocks the subprocess entirely; this script is
only exercised end-to-end in manual smoke runs.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path
from typing import Any, Dict, Optional, Tuple


def _emit(payload: Dict[str, Any], exit_code: int = 0) -> int:
    """Write a single-line JSON payload to stdout and return exit code."""
    sys.stdout.write(json.dumps(payload))
    sys.stdout.write("\n")
    sys.stdout.flush()
    return exit_code


def _atomic_copy(src: str, dst: str) -> None:
    """Copy src to dst via tmp + rename, fsync the data, then rename."""
    dst_path = Path(dst)
    dst_path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_path = tempfile.mkstemp(
        prefix=dst_path.name + ".tmp.", dir=str(dst_path.parent)
    )
    try:
        with os.fdopen(fd, "wb") as out_f, open(src, "rb") as in_f:
            shutil.copyfileobj(in_f, out_f)
            out_f.flush()
            os.fsync(out_f.fileno())
        os.rename(tmp_path, str(dst_path))
    except Exception:
        try:
            os.unlink(tmp_path)
        except OSError:
            pass
        raise


def _try_import_safetensors() -> Tuple[Optional[object], Optional[object]]:
    """Best-effort import of safetensors + numpy (returns (None, None) when
    unavailable). The C-side test seam never exercises this path."""
    try:
        import numpy as np  # noqa: F401
        from safetensors import safe_open  # noqa: F401
        from safetensors.numpy import save_file  # noqa: F401

        return safe_open, save_file
    except ImportError:
        return None, None


def _compat_check(slow_path: str, fast_path: str) -> Optional[str]:
    """Compare tensor shapes/dtypes/keys across the two safetensors files.

    Returns None on compatible; a sentinel reason string otherwise:
      "rank_mismatch:<key>"
      "shape_mismatch:<key>"
      "target_modules_mismatch"
      "base_model_mismatch"
    """
    safe_open, _ = _try_import_safetensors()
    if safe_open is None:
        # Without safetensors we cannot inspect — skip the check; the
        # warm-EMA path will fail downstream with safetensors_unavailable.
        return None
    try:
        with safe_open(slow_path, framework="numpy") as s, safe_open(  # type: ignore
            fast_path, framework="numpy"
        ) as f:
            s_keys = set(s.keys())
            f_keys = set(f.keys())
            if s_keys != f_keys:
                return "target_modules_mismatch"
            s_meta = s.metadata() or {}
            f_meta = f.metadata() or {}
            s_base = s_meta.get("base_model", "")
            f_base = f_meta.get("base_model", "")
            if s_base and f_base and s_base != f_base:
                return "base_model_mismatch"
            for k in sorted(s_keys):
                s_t = s.get_tensor(k)
                f_t = f.get_tensor(k)
                if s_t.shape != f_t.shape:
                    # Rank shows up first in the shape vector for LoRA A/B
                    # matrices, so a rank mismatch surfaces as a shape
                    # mismatch on those tensors. Caller treats this as
                    # a compat failure regardless.
                    if "lora_A" in k or "lora_B" in k:
                        return f"rank_mismatch:{k}"
                    return f"shape_mismatch:{k}"
                if s_t.dtype != f_t.dtype:
                    return f"shape_mismatch:{k}"
    except Exception as e:
        return f"compat_check_error:{type(e).__name__}"
    return None


def _ema_apply(slow_path: str, fast_path: str, alpha: float, out_path: str) -> int:
    """Warm-EMA path: alpha * slow + (1 - alpha) * fast."""
    reason = _compat_check(slow_path, fast_path)
    if reason is not None:
        return _emit({"ok": False, "reason": reason}, exit_code=1)
    safe_open, save_file = _try_import_safetensors()
    if safe_open is None or save_file is None:
        return _emit({"ok": False, "reason": "safetensors_unavailable"}, exit_code=1)
    try:
        import numpy as np

        with safe_open(slow_path, framework="numpy") as s, safe_open(  # type: ignore
            fast_path, framework="numpy"
        ) as f:
            out_tensors: Dict[str, Any] = {}
            for k in s.keys():
                s_t = s.get_tensor(k)
                f_t = f.get_tensor(k)
                out_tensors[k] = (alpha * s_t + (1.0 - alpha) * f_t).astype(s_t.dtype)
            meta = s.metadata() or {}
            meta = dict(meta)
            meta["ema_alpha"] = f"{alpha:.6f}"
            out_dir = Path(out_path).parent
            out_dir.mkdir(parents=True, exist_ok=True)
            tmp_path = str(out_dir / (Path(out_path).name + ".tmp"))
            save_file(out_tensors, tmp_path, metadata=meta)  # type: ignore
            # Sprint 11 / US-11.8 critic-HIGH #3 fix: the warm-EMA path
            # previously did `save_file → os.rename` with no fsync between.
            # A crash after rename succeeds but before the OS commits dirty
            # pages would leave a corrupt `slow.safetensors.v{N+1}` that
            # the next night's run treats as a valid prior slow. Matches
            # the `_atomic_copy` cold-start path (line 68) which already
            # does this correctly. The Phase 0 personal_model atomic-save
            # lesson is: `tmp + fflush + fsync + rename` is the contract.
            with open(tmp_path, "rb+") as out_f:
                os.fsync(out_f.fileno())
            os.rename(tmp_path, out_path)
        return _emit({"ok": True, "alpha": alpha})
    except Exception as e:
        return _emit({"ok": False, "reason": f"ema_error:{type(e).__name__}:{e}"},
                     exit_code=1)


def _cold_start(fast_path: str, out_path: str) -> int:
    """Cold-start path: copy fast → out atomically. No EMA blend."""
    if not os.path.exists(fast_path):
        return _emit({"ok": False, "reason": f"fast_missing:{fast_path}"}, exit_code=1)
    try:
        _atomic_copy(fast_path, out_path)
    except Exception as e:
        return _emit({"ok": False, "reason": f"copy_error:{type(e).__name__}:{e}"},
                     exit_code=1)
    return _emit({"ok": True, "cold_start": True})


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="lora_ema.py", description=__doc__)
    ap.add_argument("--slow", default=None, help="prior slow.safetensors (warm path)")
    ap.add_argument("--fast", required=True, help="tonight's fast.safetensors")
    ap.add_argument("--alpha", type=float, default=0.95,
                    help="EMA weight on prior slow (default 0.95)")
    ap.add_argument("--out", required=True, help="destination slow.safetensors.v{N+1}")
    ap.add_argument("--cold-start", action="store_true",
                    help="copy fast→out (no prior slow available)")
    args = ap.parse_args(argv)

    if args.cold_start:
        return _cold_start(args.fast, args.out)
    if not args.slow:
        return _emit({"ok": False, "reason": "missing_slow_for_warm_ema"}, exit_code=1)
    if not (0.0 <= args.alpha <= 1.0):
        return _emit({"ok": False, "reason": f"alpha_out_of_range:{args.alpha}"},
                     exit_code=1)
    return _ema_apply(args.slow, args.fast, args.alpha, args.out)


if __name__ == "__main__":
    sys.exit(main())
