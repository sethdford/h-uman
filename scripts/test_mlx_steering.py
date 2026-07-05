#!/usr/bin/env python3
"""
Unit tests for the MLX server's activation-steering support (persona vectors).

Exercises the pure/stateful pieces WITHOUT loading a model:
  - _parse_steering: clamp, filtering, ordering, malformed input
  - _steering_cache_guard: prompt-cache invalidation on signature change
  - _init_steering_vectors: registry load from .npz (incl. base_alpha calc)
  - _SteeredLayer: injection math on fake blocks (tensor + gemma4 tuple)
  - _steering_scope: no-op in stub mode (no model), restore-on-exit

Run:
  python3 scripts/test_mlx_steering.py

Exit codes: 0 = all passed, 1 = failures.
"""

from __future__ import annotations

import importlib.util
import os
import sys
import tempfile

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


def test_parse_steering(srv):
    avail = {"warmth": {}, "formality": {}}
    _check(srv._parse_steering({"warmth": 0.6}, avail) == [("warmth", 0.6)],
           "parse: single known trait")
    _check(srv._parse_steering({"warmth": 5.0}, avail) == [("warmth", 1.0)],
           "parse: clamps above +1")
    _check(srv._parse_steering({"warmth": -9}, avail) == [("warmth", -1.0)],
           "parse: clamps below -1")
    _check(srv._parse_steering({"humor": 0.5}, avail) == [],
           "parse: unknown trait filtered")
    _check(srv._parse_steering({"warmth": 0.0}, avail) == [],
           "parse: zero coefficient dropped")
    _check(srv._parse_steering({"warmth": "hot"}, avail) == [],
           "parse: non-numeric coefficient dropped")
    _check(srv._parse_steering("nope", avail) == [],
           "parse: non-dict steering tolerated")
    _check(srv._parse_steering(None, avail) == [],
           "parse: absent steering tolerated")
    a = srv._parse_steering({"warmth": 0.5, "formality": -0.5}, avail)
    b = srv._parse_steering({"formality": -0.5, "warmth": 0.5}, avail)
    _check(a == b, "parse: order-invariant (sorted plan)")


def test_cache_guard(srv):
    srv._STEERING_LAST_SIG = ()
    srv._PROMPT_CACHE = object()
    srv._PROMPT_CACHE_IDS = [1, 2, 3]
    srv._steering_cache_guard([("warmth", 0.6)])
    _check(srv._PROMPT_CACHE is None and srv._PROMPT_CACHE_IDS == [],
           "guard: cache invalidated when steering turns ON")
    srv._PROMPT_CACHE = object()
    srv._PROMPT_CACHE_IDS = [1]
    srv._steering_cache_guard([("warmth", 0.6)])
    _check(srv._PROMPT_CACHE is not None,
           "guard: cache kept when signature unchanged")
    srv._steering_cache_guard([])
    _check(srv._PROMPT_CACHE is None,
           "guard: cache invalidated when steering turns OFF")


def test_registry(srv):
    try:
        import numpy as np
    except ImportError:
        print("  SKIP  registry tests (numpy unavailable)")
        return
    with tempfile.TemporaryDirectory() as td:
        d = 8
        v = np.zeros(d, dtype=np.float32)
        v[0] = 1.0
        norms = np.full(4, 100.0, dtype=np.float32)
        np.savez(os.path.join(td, "warmth.npz"),
                 v_hat=v, layer=2, residual_norm=norms)
        with open(os.path.join(td, "broken.npz"), "w") as f:
            f.write("not an npz")
        os.environ["HU_MLX_STEER_RATIO"] = "0.25"
        try:
            srv._init_steering_vectors(td)
        finally:
            del os.environ["HU_MLX_STEER_RATIO"]
        _check("warmth" in srv._STEERING_VECTORS, "registry: warmth loaded")
        _check("broken" not in srv._STEERING_VECTORS,
               "registry: corrupt file skipped without crash")
        spec = srv._STEERING_VECTORS.get("warmth", {})
        _check(abs(spec.get("base_alpha", 0) - 25.0) < 1e-6,
               "registry: base_alpha = ratio * residual_norm[layer]")
        _check(spec.get("layer") == 2, "registry: layer preserved")
    srv._init_steering_vectors("")
    _check(srv._STEERING_VECTORS == {}, "registry: empty dir disables feature")


def test_steered_layer(srv):
    try:
        import mlx.core as mx
        import numpy as np
    except ImportError:
        print("  SKIP  steered-layer tests (mlx unavailable)")
        return

    class PlainBlock:
        marker = "attr"

        def __call__(self, x, mask=None, cache=None):
            return x * 2

    class TupleBlock:
        def __call__(self, x, mask=None, cache=None, **kw):
            return x * 2, "kvs", 7

    v = np.ones(4, dtype=np.float32)
    x = mx.ones((1, 3, 4))
    plain = srv._SteeredLayer(PlainBlock(), v, 3.0)
    _check(bool((abs(np.array(plain(x)) - 5.0) < 1e-6).all()),
           "layer: plain block h*2 + 3*v")
    _check(plain.marker == "attr", "layer: attribute delegation")
    tup = srv._SteeredLayer(TupleBlock(), v, 3.0)
    out = tup(x)
    _check(isinstance(out, tuple) and out[1] == "kvs" and out[2] == 7,
           "layer: gemma4 tuple passthrough")
    _check(bool((abs(np.array(out[0]) - 5.0) < 1e-6).all()),
           "layer: gemma4 tuple hidden state steered")


def test_scope_stub_mode(srv):
    # No model loaded → scope yields {} and never touches layers, but the
    # cache guard still runs (signature bookkeeping stays correct).
    srv._MLX_MODEL = None
    srv._STEERING_VECTORS = {"warmth": {"v": None, "layer": 0, "base_alpha": 1.0}}
    srv._STEERING_LAST_SIG = ()
    with srv._steering_scope({"steering": {"warmth": 0.6}}) as applied:
        _check(applied == {}, "scope: stub mode applies nothing")
    _check(srv._STEERING_LAST_SIG == (("warmth", 0.6),),
           "scope: signature tracked even in stub mode")
    with srv._steering_scope({}) as applied:
        _check(applied == {}, "scope: absent steering is a no-op")
    _check(srv._STEERING_LAST_SIG == (),
           "scope: signature cleared when steering absent")
    srv._STEERING_VECTORS = {}


def main() -> int:
    srv = _load_server_module()
    print("[test_mlx_steering] parse")
    test_parse_steering(srv)
    print("[test_mlx_steering] cache guard")
    test_cache_guard(srv)
    print("[test_mlx_steering] registry")
    test_registry(srv)
    print("[test_mlx_steering] steered layer")
    test_steered_layer(srv)
    print("[test_mlx_steering] scope (stub mode)")
    test_scope_stub_mode(srv)
    print(f"[test_mlx_steering] {_PASSED} passed, {_FAILED} failed")
    return 1 if _FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
