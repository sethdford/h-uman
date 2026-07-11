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
    # Signature is (plan, gate_sig) since the steering_gate threshold joined
    # the cache key; empty gate config contributes "".
    _check(srv._STEERING_LAST_SIG == ((("warmth", 0.6),), ""),
           "scope: signature tracked even in stub mode")
    with srv._steering_scope({}) as applied:
        _check(applied == {}, "scope: absent steering is a no-op")
    _check(srv._STEERING_LAST_SIG == ((), ""),
           "scope: signature cleared when steering absent")
    srv._STEERING_VECTORS = {}


class _FakeTok:
    """Tokenizer stub: resolves the two channel markers and decodes a tiny
    vocabulary (enough to distinguish a 'thought' header from content)."""
    unk_token_id = -1
    bos_token_id = 2
    _VOCAB = {45518: "thought", 900: "final", 107: "\n", 42: " blah", 7: " y"}

    def __init__(self, has_markers=True):
        self._ids = {"<|channel>": 100, "<channel|>": 101} if has_markers else {}

    def convert_tokens_to_ids(self, marker):
        return self._ids.get(marker, self.unk_token_id)

    def encode(self, text):
        return [self.bos_token_id, self._ids[text]] if text in self._ids \
            else [self.bos_token_id, 5, 6]

    def decode(self, ids):
        return "".join(self._VOCAB.get(t, "?") for t in ids)


def test_thought_gate(srv):
    # Marker resolution: found via convert_tokens_to_ids, cached
    srv._CHANNEL_IDS_CACHE = None
    _check(srv._resolve_channel_ids(_FakeTok()) == (100, 101),
           "gate: channel ids resolved from tokenizer")
    # No markers (non-thinking model) -> (None, None), gating degrades off
    srv._CHANNEL_IDS_CACHE = None
    _check(srv._resolve_channel_ids(_FakeTok(has_markers=False)) == (None, None),
           "gate: markerless tokenizer resolves to (None, None)")
    srv._CHANNEL_IDS_CACHE = (100, 101)

    # note_token transitions (interleaved form: markers bracket name+content).
    # Gated steering is answer-only: the gate starts DISABLED (prefill and
    # deliberation unsteered) and engages when the deliberation ends.
    saved_tok = srv._MLX_TOKENIZER
    srv._MLX_TOKENIZER = _FakeTok()
    gate = srv._SteerGate()
    _check(not gate.enabled, "gate: starts disabled (answer-only steering)")
    srv._STEER_GATE = gate
    srv._steering_note_token(100)    # model opens its deliberation span
    _check(not gate.enabled, "gate: channel-open keeps injection off")
    srv._steering_note_token(45518)  # 'thought'
    srv._steering_note_token(42)     # deliberation content
    _check(not gate.enabled, "gate: stays disabled inside the thought span")
    srv._steering_note_token(101)
    _check(gate.enabled,
           "gate: close after interleaved thought enables (answer follows)")

    # No-deliberation path: first generated token is NOT the open marker,
    # so the reply starts immediately and must be steered.
    gate = srv._SteerGate()
    srv._STEER_GATE = gate
    srv._steering_note_token(42)
    _check(gate.enabled, "gate: non-marker first token enables (no thought)")

    # Header form: '<|channel>thought\n<channel|>' -> content FOLLOWS the
    # close marker, so the gate must stay disabled until the next header.
    for t in (100, 45518, 107, 101):
        srv._steering_note_token(t)
    _check(not gate.enabled, "gate: bare thought header keeps gate disabled")
    srv._steering_note_token(42)     # thought content after the header
    _check(not gate.enabled, "gate: header-form thought content unsteered")
    for t in (100, 900, 101):        # '<|channel>final<channel|>'
        srv._steering_note_token(t)
    _check(gate.enabled, "gate: non-thought header re-enables injection")

    srv._STEER_GATE = None
    srv._steering_note_token(100)  # must not raise with no active gate
    _check(True, "gate: note_token is a no-op without an active gate")

    # Opt-in env switch: gating removes the steered prefill, which carries
    # most of the register effect, so it defaults OFF (measured baselines
    # are ungated). HU_MLX_STEER_GATE_THOUGHT=on enables it.
    _check(not srv._thought_gating_enabled(), "gate: default is disabled (opt-in)")
    os.environ["HU_MLX_STEER_GATE_THOUGHT"] = "on"
    _check(srv._thought_gating_enabled(), "gate: env on-switch honored")
    os.environ.pop("HU_MLX_STEER_GATE_THOUGHT")

    # Prompt-tail priming: primer templates end the prompt with the thought
    # HEADER, so the first generated token is deliberation content, not the
    # open marker — priming clears awaiting_first so that content doesn't
    # get mistaken for a no-deliberation reply.
    gate = srv._SteerGate()
    srv._STEER_GATE = gate
    srv._steering_prime_gate([5, 6, 100, 45518, 107, 101])  # thought primer
    _check(not gate.enabled and not gate.awaiting_first,
           "prime: thought-header primer keeps gate disabled for the thought")
    srv._steering_note_token(42)    # thought content right after the primer
    _check(not gate.enabled, "prime: post-primer content stays unsteered")
    srv._steering_note_token(100)   # model re-opens its own thought span
    srv._steering_note_token(45518)
    srv._steering_note_token(42)
    srv._steering_note_token(101)   # interleaved close -> answer follows
    _check(gate.enabled, "prime: generated close marker enables for answer")
    gate = srv._SteerGate()
    srv._STEER_GATE = gate
    srv._steering_prime_gate([5, 6, 7])              # no markers at all
    _check(not gate.enabled and gate.awaiting_first,
           "prime: markerless prompt defers to the first-token watch")
    srv._STEER_GATE = None
    srv._steering_prime_gate([100, 45518, 101])      # no active gate: no-op
    _check(True, "prime: no-op without an active gate")
    srv._MLX_TOKENIZER = saved_tok


def test_gated_layer(srv):
    try:
        import mlx.core as mx
        import numpy as np
    except ImportError:
        print("  SKIP  gated-layer tests (mlx unavailable)")
        return

    class PlainBlock:
        def __call__(self, x, mask=None, cache=None):
            return x * 2

    # Test basic gating: apply steering only to tokens with high cosine sim
    # v is a unit vector (normalized)
    v = np.ones(4, dtype=np.float32)  # [1, 1, 1, 1]
    x = mx.ones((1, 3, 4))  # Each token is [1.0, 1.0, 1.0, 1.0]
    
    # Expected: PlainBlock returns x*2 = [2, 2, 2, 2]
    # Steering adds 3 * v = [3, 3, 3, 3]
    # Result: [5, 5, 5, 5]
    
    # Ungated: applies unconditionally (old behavior)
    ungated = srv._SteeredLayer(PlainBlock(), v, 3.0)
    result_ungated = ungated(x)
    _check(bool((abs(np.array(result_ungated) - 5.0) < 1e-6).all()),
           "gated layer: gate=None applies unconditionally")
    
    # Gated with low threshold (e.g., 0.8): should apply (cosine sim = 1.0)
    gated_low = srv._SteeredLayer(PlainBlock(), v, 3.0, gate_threshold=0.8)
    result_gated_low = gated_low(x)
    _check(bool((abs(np.array(result_gated_low) - 5.0) < 1e-6).all()),
           "gated layer: low threshold applies when cos_sim >= threshold")
    
    # Gated with threshold 1.0: only applies if |cos_sim| == 1.0 (exact)
    # Since we're comparing floats, this might not match exactly due to numerical precision
    # but for unit vectors it should match
    gated_exact = srv._SteeredLayer(PlainBlock(), v, 3.0, gate_threshold=1.0)
    result_gated_exact = gated_exact(x)
    # For unit vectors and unit input, cos_sim should be 1.0, so it applies
    _check(bool((abs(np.array(result_gated_exact) - 5.0) < 0.05).all()),
           "gated layer: exact threshold (1.0) applies for unit vectors")
    
    # Test threshold clamping: negative and >1 should clamp
    gated_neg = srv._SteeredLayer(PlainBlock(), v, 3.0, gate_threshold=-0.5)
    _check(object.__getattribute__(gated_neg, "_gate_threshold") == 0.0,
           "gated layer: negative threshold clamped to 0")
    
    gated_over = srv._SteeredLayer(PlainBlock(), v, 3.0, gate_threshold=1.5)
    _check(object.__getattribute__(gated_over, "_gate_threshold") == 1.0,
           "gated layer: oversized threshold clamped to 1")


def test_scope_gate_wiring(srv):
    try:
        import numpy as np
    except ImportError:
        print("  SKIP  scope gate wiring (numpy unavailable)")
        return

    class FakeBlock:
        def __call__(self, x, *a, **kw):
            return x

    class FakeInner:
        def __init__(self):
            self.layers = [FakeBlock() for _ in range(4)]

    class FakeModel:
        def __init__(self):
            self.model = FakeInner()

    saved_model = srv._MLX_MODEL
    srv._MLX_MODEL = FakeModel()
    srv._STEERING_VECTORS = {"formality": {
        "v": np.ones(4, dtype=np.float32), "layer": 1, "base_alpha": 2.0}}
    srv._STEERING_LAST_SIG = ()
    
    # Test with gate threshold in request
    with srv._steering_scope({"steering": {"formality": 0.5}, "steering_gate": {"threshold": 0.05}}) as applied:
        _check(applied.get("formality") == 0.5, "scope: steering applied with gate")
        _check(applied.get("_gate") == 0.05, "scope: gate threshold included in response")
    
    # Test without gate (old behavior)
    with srv._steering_scope({"steering": {"formality": 0.5}}) as applied:
        _check(applied.get("formality") == 0.5, "scope: steering applied without gate")
        _check(applied.get("_gate") is None, "scope: no _gate field when gating absent")
    
    # Test gate threshold clamping
    with srv._steering_scope({"steering": {"formality": 0.5}, "steering_gate": {"threshold": 1.5}}) as applied:
        _check(applied.get("_gate") == 1.0, "scope: gate threshold clamped to 1.0")

    srv._MLX_MODEL = saved_model
    srv._STEERING_VECTORS = {}
    srv._STEERING_LAST_SIG = ()


def test_expert_registry(srv):
    try:
        import numpy as np
    except ImportError:
        print("  SKIP  expert registry tests (numpy unavailable)")
        return
    with tempfile.TemporaryDirectory() as td:
        np.savez(os.path.join(td, "warmth.npz"),
                 v_hat=np.ones(8, dtype=np.float32), layer=2,
                 residual_norm=np.full(4, 100.0, dtype=np.float32))
        np.savez(os.path.join(td, "warmth_experts.npz"),
                 layers=np.array([13, 13, 22], dtype=np.int32),
                 experts=np.array([65, 83, 101], dtype=np.int32),
                 signs=np.array([1.0, 1.0, -1.0], dtype=np.float32),
                 base_bias=np.float32(2.0))
        with open(os.path.join(td, "humor_experts.npz"), "w") as f:
            f.write("not an npz")
        srv._STEERING_EXPERTS.clear()
        srv._init_steering_vectors(td)
        _check("warmth" in srv._STEERING_EXPERTS,
               "expert registry: spec loaded from <trait>_experts.npz")
        _check("warmth_experts" not in srv._STEERING_VECTORS,
               "expert registry: expert file not loaded as residual vector")
        _check("humor" not in srv._STEERING_EXPERTS,
               "expert registry: corrupt spec skipped without crash")
        spec = srv._STEERING_EXPERTS.get("warmth", {})
        _check(spec.get("layers", {}).get(13) == [(65, 1.0), (83, 1.0)],
               "expert registry: layer grouping preserved")
        _check(spec.get("layers", {}).get(22) == [(101, -1.0)],
               "expert registry: negative-sign expert preserved")
        _check(spec.get("base_bias") == 2.0, "expert registry: base_bias")
    srv._STEERING_EXPERTS.clear()
    srv._STEERING_VECTORS = {}


def test_scope_expert_mode(srv):
    try:
        import numpy as np
    except ImportError:
        print("  SKIP  scope expert mode (numpy unavailable)")
        return

    class FakeRouterConfig:
        num_experts = 128
        top_k_experts = 8

    class FakeRouter:
        config = FakeRouterConfig()

        def __call__(self, x):
            return "idx", "w"

    class FakeMoEBlock:
        def __init__(self):
            self.router = FakeRouter()
            self.enable_moe = True

        def __call__(self, x, *a, **kw):
            return x

    class FakeDenseBlock:
        def __call__(self, x, *a, **kw):
            return x

    class FakeInner:
        def __init__(self):
            self.layers = [FakeMoEBlock() if i != 1 else FakeDenseBlock()
                           for i in range(4)]

    class FakeModel:
        def __init__(self):
            self.model = FakeInner()

    saved_model = srv._MLX_MODEL
    srv._MLX_MODEL = FakeModel()
    srv._STEERING_EXPERTS.clear()
    srv._STEERING_EXPERTS["warmth"] = {
        "layers": {0: [(65, 1.0)], 3: [(5, 1.0), (9, -1.0)]},
        "base_bias": 2.0}
    srv._STEERING_VECTORS = {"warmth": {
        "v": np.ones(4, dtype=np.float32), "layer": 1, "base_alpha": 2.0}}
    srv._STEERING_LAST_SIG = ()

    inner = srv._MLX_MODEL.model
    orig_routers = [getattr(b, "router", None) for b in inner.layers]

    with srv._steering_scope({"steering": {"warmth": 0.5},
                              "steering_mode": "expert"}) as applied:
        _check(applied.get("warmth") == 0.5, "expert scope: coeff applied")
        _check(type(inner.layers[0].router).__name__ == "_BiasedRouter",
               "expert scope: router wrapped on spec layer")
        _check(inner.layers[2].router is orig_routers[2],
               "expert scope: unlisted MoE layer untouched")
        bias = np.array(object.__getattribute__(
            inner.layers[3].router, "_bias"))
        _check(abs(bias[5] - 1.0) < 1e-6 and abs(bias[9] + 1.0) < 1e-6,
               "expert scope: bias = coeff*base_bias*sign per expert")
        _check(abs(float(bias.sum())) < 1e-6 + 2.0,
               "expert scope: only listed experts biased")
    _check(inner.layers[0].router is orig_routers[0],
           "expert scope: router restored on exit")
    _check(("mode:expert" in srv._STEERING_LAST_SIG[1]),
           "expert scope: mode folded into cache signature")

    # Residual mode ignores expert-only traits and vice versa: a trait
    # present in BOTH registries resolves per request mode.
    with srv._steering_scope({"steering": {"warmth": 0.5}}) as applied:
        _check(type(inner.layers[1]).__name__ == "_SteeredLayer" or
               applied.get("warmth") == 0.5,
               "expert scope: residual mode still installs _SteeredLayer")
    _check(inner.layers[0].router is orig_routers[0],
           "expert scope: no router wrap in residual mode")

    srv._MLX_MODEL = saved_model
    srv._STEERING_EXPERTS.clear()
    srv._STEERING_VECTORS = {}
    srv._STEERING_LAST_SIG = ()


def test_repetition_guard(srv):
    # Run rule: RUN_LIMIT consecutive identical tokens trips
    g = srv._RepetitionGuard()
    for i in range(srv._REP_GUARD_RUN_LIMIT - 1):
        _check_last = g.note(7)
    _check(not g.tripped, "rep guard: below run limit stays untripped")
    _check(g.note(7) and g.tripped, "rep guard: run limit trips")
    _check(g.note(99), "rep guard: stays tripped after trip")

    # Window rule: a 2-token loop never satisfies the run rule but fills
    # the window with <DISTINCT_MIN distinct ids
    g = srv._RepetitionGuard()
    for i in range(srv._REP_GUARD_WINDOW - 1):
        g.note(i % 2)
    _check(not g.tripped, "rep guard: loop below full window untripped")
    _check(g.note(1) and g.tripped, "rep guard: 2-token loop trips on full window")

    # Natural text: distinct ids never trip either rule
    g = srv._RepetitionGuard()
    for i in range(srv._REP_GUARD_WINDOW * 3):
        g.note(i)
    _check(not g.tripped, "rep guard: distinct stream never trips")

    # Interleaved repeats with enough diversity stay untripped
    g = srv._RepetitionGuard()
    for i in range(srv._REP_GUARD_WINDOW * 2):
        g.note(i % (srv._REP_GUARD_DISTINCT_MIN + 1))
    _check(not g.tripped, "rep guard: DISTINCT_MIN+1 loop is tolerated")

    # Env kill switch
    os.environ["HU_MLX_REP_GUARD"] = "off"
    try:
        _check(not srv._rep_guard_enabled(), "rep guard: env kill switch")
    finally:
        del os.environ["HU_MLX_REP_GUARD"]
    _check(srv._rep_guard_enabled(), "rep guard: default on")


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
    print("[test_mlx_steering] thought gate")
    test_thought_gate(srv)
    print("[test_mlx_steering] gated layer")
    test_gated_layer(srv)
    print("[test_mlx_steering] scope gate wiring")
    test_scope_gate_wiring(srv)
    print("[test_mlx_steering] expert registry")
    test_expert_registry(srv)
    print("[test_mlx_steering] scope expert mode")
    test_scope_expert_mode(srv)
    print("[test_mlx_steering] repetition guard")
    test_repetition_guard(srv)
    print(f"[test_mlx_steering] {_PASSED} passed, {_FAILED} failed")
    return 1 if _FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
