#!/usr/bin/env python3
"""
Phase 1b — unit tests for speculative-decoding draft selection in
scripts/mlx-server.py (no model, no GPU).

Pins the pure decision logic that decides WHETHER and WHICH draft model to
auto-load: family-aware default selection, local-cache gating (never trigger
a surprise download), and tokenizer parity (a divergent draft tokenizer
silently corrupts output, so it must be rejected).

Run:  python3 scripts/test_mlx_spec_decode.py
Exit 0 = all passed, 1 = a failure.
"""
from __future__ import annotations

import importlib.util
import os
import sys
import tempfile

_PASSED = 0
_FAILED = 0


def _check(cond, label):
    global _PASSED, _FAILED
    if cond:
        _PASSED += 1
        print(f"  PASS  {label}")
    else:
        _FAILED += 1
        print(f"  FAIL  {label}")


def _load():
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location(
        "mlx_server_spec", os.path.join(here, "mlx-server.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class _FakeTok:
    def __init__(self, vocab, mapping=None):
        self.vocab_size = vocab
        self._m = mapping or {}

    def encode(self, s):
        # deterministic toy encoding: per-char ordinals, optionally remapped
        return [self._m.get(c, ord(c)) for c in s]


def main() -> int:
    m = _load()
    draft_for = m._default_draft_for_model
    compat = m._tokenizers_compatible
    in_cache = m._model_in_local_cache

    # ── _default_draft_for_model ──────────────────────────────────────
    E2B = "mlx-community/gemma-4-e2b-it-4bit"
    _check(draft_for("mlx-community/gemma-4-31b-it-4bit") == E2B,
           "default draft: DENSE gemma-4 31B -> E2B sibling")
    _check(draft_for("mlx-community/gemma-4-26b-a4b-it-4bit") == "",
           "default draft: MoE gemma-4 26b-a4b -> none (spec decode is a "
           "slowdown on sparse-active models)")
    _check(draft_for(E2B) == "",
           "default draft: E2B target -> none (it IS the small one)")
    _check(draft_for("mlx-community/gemma-3-4b-it-bf16") == "",
           "default draft: non-gemma-4 -> none")
    _check(draft_for("mlx-community/Qwen2.5-7B-4bit") == "",
           "default draft: non-gemma -> none")
    _check(draft_for("") == "", "default draft: empty -> none")

    # ── _tokenizers_compatible ────────────────────────────────────────
    _check(compat(_FakeTok(262144), _FakeTok(262144)) is True,
           "compat: same vocab + same encoding -> True")
    _check(compat(_FakeTok(262144), _FakeTok(256000)) is False,
           "compat: differing vocab size -> False")
    _check(compat(_FakeTok(100, {"H": 9}), _FakeTok(100)) is False,
           "compat: same vocab but divergent encoding -> False")
    _check(compat(None, _FakeTok(100)) is False,
           "compat: missing tokenizer -> False")

    # ── _model_in_local_cache (path branch) ───────────────────────────
    with tempfile.TemporaryDirectory() as d:
        _check(in_cache(d) is True, "in_cache: existing local path -> True")
    _check(in_cache("/nonexistent/path/xyz") is False,
           "in_cache: missing local path -> False")
    _check(in_cache("") is False, "in_cache: empty -> False")
    _check(in_cache("definitely/not-a-real-repo-zzz-9999") is False,
           "in_cache: uncached repo id -> False")

    # ── _maybe_enable_turbo_kv off-path (Phase 1c) ────────────────────
    # Default (flag unset) must be a pure no-op: returns False and does NOT
    # patch the cache factory. (The on-path needs a custom mlx build + model,
    # covered live, not here.)
    import mlx_lm.models.cache as cache_mod
    orig_factory = cache_mod.make_prompt_cache
    os.environ.pop("HU_MLX_TURBO_KV", None)
    _check(m._maybe_enable_turbo_kv() is False,
           "turbo: flag unset -> returns False")
    _check(cache_mod.make_prompt_cache is orig_factory,
           "turbo: flag unset -> cache factory untouched")
    os.environ["HU_MLX_TURBO_KV"] = "0"
    _check(m._maybe_enable_turbo_kv() is False,
           "turbo: flag=0 -> returns False")
    os.environ.pop("HU_MLX_TURBO_KV", None)

    print(f"\nResult: {_PASSED} passed, {_FAILED} failed")
    return 1 if _FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
