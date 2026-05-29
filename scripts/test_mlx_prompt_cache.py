#!/usr/bin/env python3
"""
Phase 1a — unit tests for the prompt-cache prefix-reuse PLANNER in
scripts/mlx-server.py.

These test the pure decision logic (`_common_prefix_len`,
`_plan_prompt_cache_reuse`) with no model and no GPU, so they run in CI on
any host. The planner is the brain of prefix reuse: it decides how many
cached tokens to keep and which suffix to prefill. Getting it wrong would
either forfeit the speedup (harmless) or — the dangerous case — reuse KV for
a position whose token differs (silently wrong output). The tests pin the
correctness boundary: KV is reused ONLY for an exact shared prefix.

The real-model identity + speed proof lives in
scripts/test_mlx_prompt_cache_live.py (opt-in, needs a cached MLX model).

Run:
    python3 scripts/test_mlx_prompt_cache.py
Exit 0 = all passed, 1 = a failure.
"""
from __future__ import annotations

import importlib.util
import os
import sys

_PASSED = 0
_FAILED = 0


def _check(cond: bool, label: str):
    global _PASSED, _FAILED
    if cond:
        _PASSED += 1
        print(f"  PASS  {label}")
    else:
        _FAILED += 1
        print(f"  FAIL  {label}")


def _load_server_module():
    """Load scripts/mlx-server.py (hyphenated name → importlib)."""
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "mlx-server.py")
    spec = importlib.util.spec_from_file_location("mlx_server_under_test", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main() -> int:
    m = _load_server_module()
    cpl = m._common_prefix_len
    plan = m._plan_prompt_cache_reuse

    # ── _common_prefix_len ────────────────────────────────────────────
    _check(cpl([], []) == 0, "common_prefix: empty/empty -> 0")
    _check(cpl([1, 2, 3], []) == 0, "common_prefix: x/empty -> 0")
    _check(cpl([1, 2, 3], [1, 2, 3]) == 3, "common_prefix: identical -> full")
    _check(cpl([1, 2, 3], [1, 2, 9]) == 2, "common_prefix: diverge at 2 -> 2")
    _check(cpl([1, 2], [1, 2, 3, 4]) == 2, "common_prefix: a is prefix of b -> len(a)")
    _check(cpl([9, 1, 2], [1, 2, 3]) == 0, "common_prefix: diverge at 0 -> 0")

    # ── _plan_prompt_cache_reuse ──────────────────────────────────────
    # Empty cache -> always reset, prefill everything.
    act, trim, suf = plan([], [1, 2, 3], trimmable=True)
    _check((act, trim, suf) == ("reset", 0, [1, 2, 3]),
           "plan: empty cache -> reset full")

    # New prompt extends the cached prompt (typical multi-turn append):
    # reuse all cached tokens, prefill only the appended suffix.
    act, trim, suf = plan([1, 2, 3], [1, 2, 3, 4, 5], trimmable=True)
    _check((act, trim, suf) == ("reuse", 0, [4, 5]),
           "plan: pure extension -> reuse all, prefill suffix")

    # Divergence mid-prompt (e.g. per-turn time injection changed a token):
    # keep the shared prefix, trim the stale tail, prefill the new suffix.
    act, trim, suf = plan([1, 2, 3, 8, 9], [1, 2, 3, 4, 5], trimmable=True)
    _check(act == "reuse" and trim == 2 and suf == [4, 5],
           "plan: mid-diverge -> trim stale tail, prefill new suffix")

    # No shared prefix at all -> reset.
    act, trim, suf = plan([7, 8, 9], [1, 2, 3], trimmable=True)
    _check((act, trim, suf) == ("reset", 0, [1, 2, 3]),
           "plan: no overlap -> reset")

    # Divergence requiring a trim, but cache type is NOT trimmable -> reset
    # (correctness over speed: we can't drop the stale tail, so rebuild).
    act, trim, suf = plan([1, 2, 3, 8, 9], [1, 2, 3, 4, 5], trimmable=False)
    _check(act == "reset" and suf == [1, 2, 3, 4, 5],
           "plan: diverge + not trimmable -> reset full")

    # Pure extension is fine even when not trimmable (trim_count == 0).
    act, trim, suf = plan([1, 2, 3], [1, 2, 3, 4], trimmable=False)
    _check(act == "reuse" and trim == 0 and suf == [4],
           "plan: extension + not trimmable -> reuse (no trim needed)")

    # New prompt is a strict prefix of cached tokens (we already generated
    # past it): replay the final token so generation has a continuation
    # point, never reuse a position we haven't actually got.
    act, trim, suf = plan([1, 2, 3, 4, 5], [1, 2, 3], trimmable=True)
    _check(act == "reuse" and suf == [3] and trim == (5 - (3 - 1)),
           "plan: new is prefix of cached -> replay last token")

    print(f"\nResult: {_PASSED} passed, {_FAILED} failed")
    return 1 if _FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
