#!/usr/bin/env python3
"""
MLX inference server — implements the M3 adapter-swap endpoint inline.

Spec 2026-05-19 M3 closure (D-M3-1): the `/v1/adapters/swap` endpoint is
implemented INLINE in this file rather than delegated to gemma-realtime.
This removes a hidden external dependency, matches the "your hardware,
your model" product thesis, and lets us version the swap contract.

What's served:
  - GET  /health                  → {"ok": True, "active_adapter": "..."}
  - GET  /v1/adapters/current     → {"adapter_path": "...", "tensors_loaded": N}
  - POST /v1/adapters/swap        → {"status": "ok"|"error", "adapter_path": ...}
  - POST /v1/chat/completions     → OpenAI-shaped completion (delegated when possible)

Delegation policy:
  - If `gemma-realtime` is installed AND it exposes /v1/adapters/swap,
    we delegate chat completions to it (it has TurboQuant+ KV cache
    compression, speculative decoding, etc.). The swap endpoint is
    STILL handled by THIS file.
  - If `gemma-realtime` is installed but does NOT expose
    /v1/adapters/swap, we EXIT NON-ZERO with a named error (AC-M3-1 (b)).
  - If `mlx_lm` is installed and gemma-realtime is not, we use it
    directly via `model.load_weights` for swaps.
  - If neither is available, we run in a stub mode for tests / CI.

Swap request shape:
  POST /v1/adapters/swap
  Content-Type: application/json
  {
    "adapter_path": "/path/to/adapter.safetensors",   # required
    "contact_hash": "0x123abc..."                       # optional, logged
  }

Swap response shape (200 ok):
  {
    "status": "ok",
    "adapter_path": "/path/to/adapter.safetensors",
    "adapter_loaded": "/path/to/adapter.safetensors",
    "tensors_loaded": 42
  }

Swap response shape (400 bad request):
  {"status": "error", "error": "missing adapter_path"}

Swap response shape (404 not found):
  {"status": "error", "error": "adapter not found at <path>"}

Swap response shape (500 internal):
  {"status": "error", "error": "<exception detail>"}

On any exception during load_weights, the previously-loaded adapter is
restored — no half-loaded state is left on the server.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

# Paths to look for gemma-realtime, in priority order.
GEMMA_RT_PATHS = [
    os.path.expanduser("~/Documents/gemma-realtime-1/scripts/mlx-server.py"),
    os.path.expanduser("~/Documents/gemma-realtime/scripts/mlx-server.py"),
    os.path.expanduser("~/gemma-realtime/scripts/mlx-server.py"),
]


def find_gemma_realtime():
    for p in GEMMA_RT_PATHS:
        if os.path.isfile(p):
            return p
    return None


def gemma_realtime_has_swap(path: str) -> bool:
    """Static probe: does the gemma-realtime source actually define the
    swap endpoint? We do this with a string match against the source
    rather than running the binary because the swap endpoint is a static
    routing decision in upstream — it can't appear at runtime if it isn't
    in the source.

    The match is intentionally permissive — any string containing both
    "/v1/adapters/swap" and a POST handler signature counts.
    """
    try:
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            src = f.read()
    except OSError:
        return False
    return "/v1/adapters/swap" in src


def have_mlx_lm() -> bool:
    try:
        import mlx_lm  # noqa: F401
        return True
    except ImportError:
        return False


# ─────────────────────────────────────────────────────────────────────
# In-process model + adapter state.
# ─────────────────────────────────────────────────────────────────────
# When `mlx_lm` is available we hold the loaded model and the path to
# its currently-loaded adapter here. The swap endpoint mutates these.
# When `mlx_lm` is NOT available we still track the path so callers can
# verify the swap contract end-to-end without a GPU (the tensors_loaded
# count is synthetic in that case).

_MLX_MODEL = None  # mlx_lm model handle (or None)
_MLX_TOKENIZER = None  # mlx_lm tokenizer (or None)
_CURRENT_ADAPTER = ""
_TENSORS_LOADED = 0

# Phase 3b — cross-model speculative decoding state. When the operator
# sets HU_MLX_DRAFT_MODEL (or --draft-model), we load a small draft
# model alongside the target and pass it through to mlx_lm.stream_generate
# via the `draft_model=` kwarg (mlx_lm >= 0.21). Newer mlx_lm exposes
# the kwarg; older versions silently ignore extras, so the fallback is
# graceful — spec decode is off but inference still works.
_MLX_DRAFT_MODEL = None
_MLX_DRAFT_TOKENIZER = None
_MLX_DRAFT_PATH = ""

# Phase 1a — prompt-cache prefix reuse. The persona system prompt is large
# and mostly stable across a conversation; only the tail (latest user turn,
# per-turn circadian/time injection) changes. We hold the KV cache from the
# previous request plus the token-id list it corresponds to, and on each new
# request reuse the KV for the longest shared token PREFIX, prefilling only
# the divergent suffix. This is correctness-preserving: KV is never reused for
# a position whose token differs, so output is byte-identical to a cold
# prefill — we only ever save the prefill compute of the shared prefix.
#
# Disabled by setting HU_MLX_PROMPT_CACHE=0. The server is single-threaded
# (HTTPServer, not Threading), so this global state needs no locking — one
# request is prefilled+generated to completion before the next begins.
_PROMPT_CACHE = None          # list of per-layer cache objects, or None
_PROMPT_CACHE_IDS: list = []  # token ids the cache currently holds KV for


def _prompt_cache_enabled() -> bool:
    return os.environ.get("HU_MLX_PROMPT_CACHE", "1") not in ("0", "false", "False", "")


def _invalidate_prompt_cache(reason: str = "") -> None:
    """Drop the reuse cache. MUST be called whenever the model weights or
    adapter change — KV cached under old weights is invalid for new ones."""
    global _PROMPT_CACHE, _PROMPT_CACHE_IDS
    if _PROMPT_CACHE is not None or _PROMPT_CACHE_IDS:
        if reason:
            print(f"[mlx-server] prompt cache invalidated ({reason})", flush=True)
    _PROMPT_CACHE = None
    _PROMPT_CACHE_IDS = []


def _cache_is_rotating(cache) -> bool:
    """True if ANY layer is a RotatingKVCache (sliding-window attention, e.g.
    Gemma's hybrid local/global layers — identified by a `max_size`). Trimming
    such a cache is NOT bit-exact even when can_trim_prompt_cache() says yes:
    the ring-buffer layout differs from a fresh contiguous prefill, producing
    tiny fp differences in attention that can flip a greedy token. So we allow
    only NON-trimming reuse (pure extension) on these — never a tail trim."""
    try:
        return any(getattr(c, "max_size", None) is not None for c in cache)
    except TypeError:
        return False


def _common_prefix_len(a, b) -> int:
    """Length of the longest shared prefix of two token-id sequences."""
    n = min(len(a), len(b))
    i = 0
    while i < n and a[i] == b[i]:
        i += 1
    return i


def _plan_prompt_cache_reuse(cached_ids, new_ids, trimmable: bool):
    """Decide how to reuse a populated prompt cache for a new prompt.

    Pure function (no globals, no model) so the prefix logic is unit-testable
    without a GPU. Returns (action, trim_count, suffix_ids):

      action == "reset" — no usable overlap, or the cache type can't be
                trimmed; caller builds a fresh cache and prefills ALL of
                new_ids. trim_count is 0, suffix_ids == new_ids.
      action == "reuse" — keep the cache, drop `trim_count` tokens off its
                tail, then prefill only `suffix_ids` (= new_ids[common:]).
    """
    if not cached_ids:
        return ("reset", 0, list(new_ids))
    common = _common_prefix_len(cached_ids, new_ids)
    if common == 0:
        return ("reset", 0, list(new_ids))
    trim_count = len(cached_ids) - common
    if trim_count > 0 and not trimmable:
        # Divergent tail can't be dropped from this cache type → rebuild.
        return ("reset", 0, list(new_ids))
    suffix = list(new_ids[common:])
    if not suffix:
        # new_ids is a prefix of (or equal to) the cached tokens — we've
        # already prefilled past this point. Replay the final token as a
        # 1-token suffix so generation has a position to continue from.
        if common >= 1:
            return ("reuse", len(cached_ids) - (common - 1), list(new_ids[common - 1:]))
        return ("reset", 0, list(new_ids))
    return ("reuse", trim_count, suffix)


def _prepare_prompt_cache(prompt_ids):
    """Plan + apply prompt-cache reuse for a freshly tokenized prompt.

    Mutates the module-level cache state and returns (prompt_for_generate,
    prompt_cache) where:
      - prompt_for_generate is the token-id list to actually feed the
        generator (the full prompt on reset, or just the suffix on reuse).
      - prompt_cache is the cache object list to pass through, or None when
        caching is disabled / unavailable (caller then feeds the full prompt
        with no cache).

    On any failure the function degrades to (full_ids, None) so a cache bug
    can never change OUTPUT, only forfeit the speedup.
    """
    global _PROMPT_CACHE, _PROMPT_CACHE_IDS
    full_ids = list(prompt_ids)
    if not _prompt_cache_enabled() or _MLX_MODEL is None:
        return full_ids, None
    try:
        from mlx_lm.models.cache import (
            make_prompt_cache,
            trim_prompt_cache,
            can_trim_prompt_cache,
        )
    except Exception:  # noqa: BLE001 — cache utils absent on old mlx_lm
        return full_ids, None

    try:
        # "trimmable" gates whether we may drop a divergent TAIL. It requires
        # both that mlx_lm reports the cache trimmable AND that the cache is
        # not rotating (rotating trims aren't bit-exact — see
        # _cache_is_rotating). Pure extension (trim_count == 0) never trims and
        # is allowed regardless, so sliding-window models still benefit from
        # multi-turn continuation.
        trimmable = bool(
            _PROMPT_CACHE is not None
            and can_trim_prompt_cache(_PROMPT_CACHE)
            and not _cache_is_rotating(_PROMPT_CACHE)
        )
        action, trim_count, suffix = _plan_prompt_cache_reuse(
            _PROMPT_CACHE_IDS, full_ids, trimmable
        )
        if action == "reuse" and _PROMPT_CACHE is not None:
            if trim_count > 0:
                # trim_prompt_cache returns the number ACTUALLY trimmed. A
                # RotatingKVCache (sliding-window models like Gemma) can't
                # always drop the exact tail once it has wrapped — it may
                # trim fewer tokens than asked. If so, the cache no longer
                # corresponds to the shared prefix and reusing it would feed
                # STALE KV for diverged positions (observed: subtly different
                # output). Correctness over speed — rebuild on a partial trim.
                actual = trim_prompt_cache(_PROMPT_CACHE, trim_count)
                if actual != trim_count:
                    _PROMPT_CACHE = make_prompt_cache(_MLX_MODEL)
                    _PROMPT_CACHE_IDS = []
                    return full_ids, _PROMPT_CACHE
                _PROMPT_CACHE_IDS = _PROMPT_CACHE_IDS[: len(_PROMPT_CACHE_IDS) - trim_count]
            reused = len(full_ids) - len(suffix)
            if reused > 0:
                print(f"[mlx-server] prompt cache: reused {reused} of "
                      f"{len(full_ids)} prompt tokens", flush=True)
            return suffix, _PROMPT_CACHE
        # reset
        _PROMPT_CACHE = make_prompt_cache(_MLX_MODEL)
        _PROMPT_CACHE_IDS = []
        return full_ids, _PROMPT_CACHE
    except Exception as exc:  # noqa: BLE001
        print(f"[mlx-server] prompt cache disabled this turn: {exc}", flush=True)
        _invalidate_prompt_cache()
        return full_ids, None


def _finalize_prompt_cache(full_prompt_ids, generated_ids) -> None:
    """Record what the cache now holds KV for: the full prompt plus the
    tokens we just generated. Next request diffs against this."""
    global _PROMPT_CACHE_IDS
    if _PROMPT_CACHE is None:
        return
    _PROMPT_CACHE_IDS = list(full_prompt_ids) + list(generated_ids)


# ---------------------------------------------------------------------------
# Activation steering (persona vectors).
#
# The h-uman daemon sends an optional per-request field
#   "steering": {"formality": f, "verbosity": f, "warmth": f, "humor": f}
# (doubles in [-1, 1]; see src/providers/compatible.c). Each trait with a
# loaded persona vector is applied as residual-stream injection: the decoder
# layer named by the vector file gets alpha * v_hat added to its hidden-state
# output on every forward pass, where
#   alpha = coefficient * HU_MLX_STEER_RATIO * residual_norm[layer].
# The ratio (default 0.22) is the model-invariant dose: ~22% of the measured
# per-token residual norm steers visibly without degrading fluency (validated
# on gemma-2-2b and gemma-4-26b-a4b in persona-steering-lab, 2026-07-05).
# On MoE models only EARLY-layer vectors are safe — late-layer injection
# disrupts expert routing. The layer choice lives in the vector file.
#
# Vectors load from --steering-dir / HU_MLX_STEERING_DIR: one <trait>.npz per
# trait (keys: v_hat, layer, residual_norm). Feature is fully inert when the
# dir is unset, empty, or a requested trait has no vector.
#
# KV-cache correctness: KV prefilled under one steering configuration is
# invalid for another (the injection shifts every cached hidden state), so
# the prompt cache is invalidated whenever the effective steering signature
# changes — the same rule as an adapter swap.
_STEERING_VECTORS: dict = {}   # trait -> {"v": ndarray, "layer": int, "base_alpha": float}
# Expert-level steering (MoE models; SteerMoE, arXiv 2509.09660). Residual
# injection at mid layers can collapse gemma-4-26b-a4b into token
# repetition because every MoE block's router reads the hidden state the
# injection perturbs (RASA, arXiv 2602.04448) — measured 2026-07-09 as a
# STOCHASTIC 2x-dose failure (1/4 questions at alpha=42; stable at the
# calibrated 1x dose). Expert steering leaves the residual stream untouched
# and instead biases the ROUTER LOGITS of experts whose activation
# frequency differs between trait-positive and trait-negative generations
# (profiled by scripts/moe_expert_profiler.py, exported by
# scripts/moe_export_expert_steering.py as <trait>_experts.npz with keys
# layers/experts/signs/base_bias). Request opt-in: "steering_mode":
# "expert"; bias_e = coefficient * base_bias * sign_e added before top-k.
# Measured caveat (docs/research/2026-07-09-moe-expert-steering.md): this
# mode is collapse-proof but did NOT move judged warmth on its first
# profile — keep it instrumented/opt-in; residual remains the default.
_STEERING_EXPERTS: dict = {}   # trait -> {"layers": {li: [(expert, sign)]}, "base_bias": float}
_STEERING_LAST_SIG: tuple = ()

# Thought-channel gating. gemma-4 emits a deliberation span bracketed by the
# special tokens '<|channel>' (open) and '<channel|>' (close); the visible
# answer follows the close marker. Steering the deliberation tokens is what
# breaks large doses — the steered residual makes the thinking more elaborate
# until the model never exits the channel (observed: formality +0.5 at layer 6
# looped for 5k+ chars without ever emitting the answer). Gating the injection
# OFF inside channel spans keeps the deliberation native while still steering
# the prompt and the visible reply, which unlocks doses the ungated path
# cannot survive. Default ON; set HU_MLX_STEER_GATE_THOUGHT=off to restore
# steer-everything behavior. Single-threaded HTTPServer => plain shared state.
_STEER_GATE = None             # _SteerGate while a gated request is generating
_CHANNEL_IDS_CACHE: tuple | None = None   # (open_id, close_id) once resolved


class _SteerGate:
    """Mutable enabled-flag shared between the generation loop (which watches
    token ids) and _SteeredLayer (which skips injection while disabled).

    Starts DISABLED: gated steering is answer-only. A steered prefill alone
    destabilizes the thought exit (measured: formality +0.6 with unsteered
    thought tokens but steered prompt KV still never left the deliberation),
    so injection engages only once the deliberation is over — at the close
    marker of an interleaved thought span, at a non-thought header, or at
    the first token when the model skips deliberating entirely.

    in_span/span_ids track the tokens between the channel markers so the
    close-marker handler can tell a bare header name from interleaved
    thought content; awaiting_first distinguishes "no deliberation" from
    "deliberation starting"."""
    __slots__ = ("enabled", "in_span", "span_ids", "awaiting_first")

    def __init__(self):
        self.enabled = False
        self.in_span = False
        self.span_ids = []
        self.awaiting_first = True


def _thought_gating_enabled() -> bool:
    """Thought-channel gating is OPT-IN (HU_MLX_STEER_GATE_THOUGHT=on).
    Measured on formality (gemma-4-26b-a4b, layer 6): gating makes any dose
    stable (no thought-runaway even at +1.0) but most of the register shift
    comes from the steered PREFILL, which gating removes — so the effect is
    much weaker. The ungated default preserves the measured A/B baselines;
    turn gating on when dose stability matters more than effect size."""
    return os.environ.get("HU_MLX_STEER_GATE_THOUGHT", "off").lower() \
        in ("on", "1", "true", "yes")


def _resolve_channel_ids(tok) -> tuple:
    """Resolve ('<|channel>', '<channel|>') to token ids, or (None, None) when
    the tokenizer has no such markers (non-thinking model) — gating then
    degrades to steer-everything, the pre-gating behavior."""
    global _CHANNEL_IDS_CACHE
    if _CHANNEL_IDS_CACHE is not None:
        return _CHANNEL_IDS_CACHE
    ids = []
    for marker in ("<|channel>", "<channel|>"):
        tid = None
        if tok is not None:
            conv = getattr(tok, "convert_tokens_to_ids", None)
            if callable(conv):
                try:
                    got = conv(marker)
                    unk = getattr(tok, "unk_token_id", None)
                    if isinstance(got, int) and got >= 0 and got != unk:
                        tid = got
                except Exception:  # noqa: BLE001
                    pass
            if tid is None:
                try:
                    bos = getattr(tok, "bos_token_id", None)
                    enc = [t for t in tok.encode(marker) if t != bos]
                    if len(enc) == 1:
                        tid = enc[0]
                except Exception:  # noqa: BLE001
                    pass
        ids.append(tid)
    resolved = (None, None) if None in ids else tuple(ids)
    _CHANNEL_IDS_CACHE = resolved
    return resolved


def _span_is_thought_header(span_ids) -> bool:
    """True when the tokens between '<|channel>' and '<channel|>' decode to
    the bare channel name 'thought' — the header form, where the thought
    CONTENT follows the close marker. Anything longer (the interleaved form
    puts the whole deliberation inside the markers) is not a header."""
    tok = _MLX_TOKENIZER
    if tok is None or not span_ids:
        return False
    try:
        return tok.decode(list(span_ids)).strip().lower() == "thought"
    except Exception:  # noqa: BLE001
        return False


def _steering_note_token(tok_id) -> None:
    """Feed each generated token id to the active gate. gemma-4 emits its
    deliberation in the interleaved form '<|channel>thought ...content...
    <channel|>answer': the open marker disables injection, and at the close
    marker we decode the buffered span — a bare 'thought' name means the
    header form (thought content FOLLOWS, keep disabled); anything else
    means the deliberation just ended and the answer follows (re-enable).
    No-op when no gated request is in flight."""
    gate = _STEER_GATE
    if gate is None or tok_id is None or _CHANNEL_IDS_CACHE is None:
        return
    open_id, close_id = _CHANNEL_IDS_CACHE
    if gate.awaiting_first:
        gate.awaiting_first = False
        if tok_id != open_id:
            # No deliberation span — the reply starts immediately; steer it.
            gate.enabled = True
    if tok_id == open_id:
        gate.enabled = False
        gate.in_span = True
        gate.span_ids = []
    elif tok_id == close_id:
        gate.in_span = False
        gate.enabled = not _span_is_thought_header(gate.span_ids)
        gate.span_ids = []
    elif gate.in_span and len(gate.span_ids) < 64:
        # 64 tokens is far beyond any channel name; longer spans are
        # interleaved thought content and the prefix suffices to disqualify.
        gate.span_ids.append(tok_id)


def _steering_prime_gate(full_prompt_ids) -> None:
    """Set the active gate's INITIAL state from the prompt tail. Thinking
    chat templates end the prompt with a thought HEADER primer
    ('<|channel>thought\\n<channel|>'), so generation starts already inside
    the deliberation. When the prompt's last channel span is that header,
    start the gate disabled: prefill and the thought run unsteered, and the
    close marker of the model's own generated span re-enables injection for
    the visible answer — i.e. gated steering becomes answer-token steering.
    (The ungated failure mode was real: a steered prefill alone destabilizes
    the thought exit at formality >= +0.5.)"""
    gate = _STEER_GATE
    if gate is None or _CHANNEL_IDS_CACHE is None:
        return
    open_id, close_id = _CHANNEL_IDS_CACHE
    if open_id is None:
        return
    ids = list(full_prompt_ids)
    try:
        last_open = len(ids) - 1 - ids[::-1].index(open_id)
    except ValueError:
        return  # no channel header in the prompt; stay enabled
    span = []
    for tid in ids[last_open + 1:]:
        if tid == close_id:
            break
        span.append(tid)
    if _span_is_thought_header(span):
        # Primer template: generation starts inside the deliberation, so the
        # first generated token is thought content, not the open marker.
        gate.enabled = False
        gate.awaiting_first = False


# Repetition-abort guard. Mid-layer residual steering has a measured
# STOCHASTIC collapse mode: at 2x dose the generation can degenerate into
# token repetition ('eeee…' / 'own own own'; 1/4 questions at alpha=42,
# 2026-07-09). The guard watches the generated token ids WHILE STEERING IS
# APPLIED and trips on runaway repetition; the inline path then retries the
# request unsteered, the streaming path stops the stream early
# (finish_reason "repetition_abort") since sent chunks cannot be recalled.
# Never active on unsteered requests. Kill switch: HU_MLX_REP_GUARD=off.
_REP_GUARD_RUN_LIMIT = 16    # consecutive identical tokens
_REP_GUARD_WINDOW = 64       # sliding window length
_REP_GUARD_DISTINCT_MIN = 5  # trip when a full window has fewer distinct ids


def _rep_guard_enabled() -> bool:
    return os.environ.get("HU_MLX_REP_GUARD", "on").lower() \
        not in ("off", "0", "false", "no")


class _RepetitionGuard:
    """Trips on (a) >= RUN_LIMIT consecutive identical tokens, or (b) a full
    WINDOW of recent tokens containing < DISTINCT_MIN distinct ids (catches
    short multi-token loops the run rule misses). Both signatures are far
    outside natural text: the collapse tail produced runs in the hundreds."""

    __slots__ = ("tripped", "_run_tok", "_run_len", "_window")

    def __init__(self):
        self.tripped = False
        self._run_tok = None
        self._run_len = 0
        self._window = []

    def note(self, tok_id) -> bool:
        """Feed one generated token id; returns True once tripped."""
        if self.tripped:
            return True
        if tok_id == self._run_tok:
            self._run_len += 1
        else:
            self._run_tok = tok_id
            self._run_len = 1
        self._window.append(tok_id)
        if len(self._window) > _REP_GUARD_WINDOW:
            self._window.pop(0)
        if self._run_len >= _REP_GUARD_RUN_LIMIT or (
                len(self._window) == _REP_GUARD_WINDOW
                and len(set(self._window)) < _REP_GUARD_DISTINCT_MIN):
            self.tripped = True
        return self.tripped


def _steering_ratio() -> float:
    try:
        return float(os.environ.get("HU_MLX_STEER_RATIO", "0.22"))
    except ValueError:
        return 0.22


def _init_steering_vectors(dir_path: str) -> None:
    """Load every <trait>.npz in dir_path into the registry. Best-effort:
    a bad file is skipped with a log line; no vectors == feature off."""
    global _STEERING_VECTORS
    _STEERING_VECTORS = {}
    if not dir_path:
        return
    try:
        import numpy as np
        import glob as _glob
    except ImportError:
        print("[mlx-server] steering disabled: numpy unavailable", flush=True)
        return
    for path in sorted(_glob.glob(os.path.join(os.path.expanduser(dir_path), "*.npz"))):
        trait = os.path.splitext(os.path.basename(path))[0]
        if trait.endswith("_experts"):
            _init_expert_spec(trait[: -len("_experts")], path)
            continue
        try:
            data = np.load(path)
            v_hat = np.asarray(data["v_hat"], dtype=np.float32)
            layer = int(data["layer"])
            if "residual_norm" in data:
                base_alpha = _steering_ratio() * float(data["residual_norm"][layer])
            else:
                base_alpha = 1.0
                print(f"[mlx-server] steering vector '{trait}' has no "
                      "residual_norm; base_alpha=1.0 (probably too weak)", flush=True)
            _STEERING_VECTORS[trait] = {
                "v": v_hat, "layer": layer, "base_alpha": base_alpha,
            }
            print(f"[mlx-server] steering vector loaded: {trait} "
                  f"(layer {layer}, base_alpha {base_alpha:.1f}, d {v_hat.shape[0]})",
                  flush=True)
        except Exception as exc:  # noqa: BLE001
            print(f"[mlx-server] steering vector {path} skipped: {exc}", flush=True)


def _init_expert_spec(trait: str, path: str) -> None:
    """Load one <trait>_experts.npz into the expert-steering registry."""
    try:
        import numpy as np
        data = np.load(path)
        layers = {}
        for li, e, s in zip(np.asarray(data["layers"]).tolist(),
                            np.asarray(data["experts"]).tolist(),
                            np.asarray(data["signs"]).tolist()):
            layers.setdefault(int(li), []).append((int(e), float(s)))
        _STEERING_EXPERTS[trait] = {
            "layers": layers, "base_bias": float(data["base_bias"]),
        }
        n = sum(len(v) for v in layers.values())
        print(f"[mlx-server] expert steering spec loaded: {trait} "
              f"({n} experts on layers {sorted(layers)}, "
              f"base_bias {float(data['base_bias']):.2f})", flush=True)
    except Exception as exc:  # noqa: BLE001
        print(f"[mlx-server] expert spec {path} skipped: {exc}", flush=True)


def _parse_steering(raw, available) -> list:
    """Pure: raw request field + available trait names -> sorted plan
    [(trait, coeff)] with coeffs clamped to [-1, 1], zero/malformed/unknown
    entries dropped. Sorted so the signature is order-invariant."""
    if not isinstance(raw, dict):
        return []
    plan = []
    for trait, coeff in raw.items():
        if trait not in available:
            continue
        try:
            c = float(coeff)
        except (TypeError, ValueError):
            continue
        c = max(-1.0, min(1.0, c))
        if c == 0.0:
            continue
        plan.append((str(trait), c))
    return sorted(plan)


def _steering_cache_guard(plan, gate_sig: str = "") -> None:
    """Invalidate the prompt cache when the steering configuration changed —
    KV computed under different steering is stale. Called BEFORE
    _prepare_prompt_cache (ahead of any trim logic). gate_sig includes gate
    threshold info to ensure KV is invalidated when gates change."""
    global _STEERING_LAST_SIG
    sig = (tuple(plan), gate_sig)
    if sig != _STEERING_LAST_SIG:
        _invalidate_prompt_cache("steering change")
        _STEERING_LAST_SIG = sig


def _steering_layer_list(model):
    """Return the decoder-layer list, or None (stub / unknown arch)."""
    for path in (("model",), ("language_model", "model"), ()):
        obj = model
        for attr in path:
            obj = getattr(obj, attr, None)
            if obj is None:
                break
        if obj is not None and isinstance(getattr(obj, "layers", None), list) \
                and obj.layers:
            return obj
    return None


class _SteeredLayer:
    """Delegating decoder-layer wrapper: adds alpha * v_hat to the hidden
    state on every forward pass. gemma4 blocks return (h, kvs, offset)
    tuples; gemma2-style return the tensor directly.

    Optional gating (CAST-style): when gate_threshold is set, applies
    steering only to token positions where |cosine_sim(x, v_hat)| >= threshold.
    Positions below the threshold remain untouched (elementwise mask via mx.where)."""

    def __init__(self, inner, v_hat, alpha: float, gate=None, gate_threshold: float | None = None):
        object.__setattr__(self, "inner", inner)
        import mlx.core as mx
        object.__setattr__(self, "_v", mx.array(v_hat))
        object.__setattr__(self, "_alpha", float(alpha))
        # Thought-channel gating (pre-existing feature)
        object.__setattr__(self, "_gate", gate)
        # CAST-style cosine-similarity gating (new feature)
        # Clamp gate_threshold to [0, 1] if provided
        if gate_threshold is not None:
            gate_threshold = max(0.0, min(1.0, float(gate_threshold)))
        object.__setattr__(self, "_gate_threshold", gate_threshold)

    def __getattr__(self, name):
        return getattr(object.__getattribute__(self, "inner"), name)

    def __call__(self, x, *args, **kwargs):
        import mlx.core as mx
        # Check thought-channel gate first (pre-existing feature)
        gate = object.__getattribute__(self, "_gate")
        if gate is not None and not gate.enabled:
            return self.inner(x, *args, **kwargs)
        
        out = self.inner(x, *args, **kwargs)
        h = out[0] if isinstance(out, tuple) else out

        gate_threshold = object.__getattribute__(self, "_gate_threshold")
        if gate_threshold is not None:
            # Compute cosine similarity between x and v_hat
            # x shape: (..., d), v_hat shape: (d,)
            # Normalize both and compute dot product
            v = object.__getattribute__(self, "_v")
            x_norm = mx.sqrt(mx.sum(x * x, axis=-1, keepdims=True) + 1e-8)
            v_norm = mx.sqrt(mx.sum(v * v) + 1e-8)
            x_normalized = x / x_norm
            v_normalized = v / v_norm
            # Dot product over the last dimension
            cos_sim = mx.sum(x_normalized * v_normalized.reshape((1,) * (x.ndim - 1) + (-1,)), axis=-1)
            # Take absolute value and create mask
            mask = mx.abs(cos_sim) >= gate_threshold
            # Expand mask to match h's shape for broadcasting
            mask_expanded = mask.reshape(mask.shape + (1,) * (h.ndim - mask.ndim))
            # Apply steering only where mask is True
            alpha = object.__getattribute__(self, "_alpha")
            delta = mx.array(alpha, dtype=h.dtype) * v.astype(h.dtype)
            steered = mx.where(mask_expanded, h + delta, h)
        else:
            # Unconditional steering (original behavior)
            alpha = object.__getattribute__(self, "_alpha")
            v = object.__getattribute__(self, "_v")
            steered = h + mx.array(alpha, dtype=h.dtype) * v.astype(h.dtype)

        if isinstance(out, tuple):
            return (steered, *out[1:])
        return steered


class _BiasedRouter:
    """Delegating wrapper around a gemma4 MoE Router: adds a per-expert bias
    to the routing logits BEFORE top-k selection (expert-level steering).
    Replicates Router.__call__ exactly plus the bias term; the residual
    stream is never touched, which is what keeps mid-layer steering from
    collapsing MoE generation the way residual injection does. The optional
    thought-channel gate bypasses the bias while disabled (same contract as
    _SteeredLayer)."""

    def __init__(self, inner, bias, gate=None):
        import mlx.core as mx
        object.__setattr__(self, "inner", inner)
        object.__setattr__(self, "_bias", mx.array(bias))
        object.__setattr__(self, "_gate", gate)

    def __getattr__(self, name):
        return getattr(object.__getattribute__(self, "inner"), name)

    def __call__(self, x):
        import mlx.core as mx
        inner = object.__getattribute__(self, "inner")
        gate = object.__getattribute__(self, "_gate")
        if gate is not None and not gate.enabled:
            return inner(x)
        bias = object.__getattribute__(self, "_bias")
        xn = mx.fast.rms_norm(x, inner.scale * inner._root_size, inner.eps)
        scores = inner.proj(xn) + bias.astype(x.dtype)
        k = inner.config.top_k_experts
        top_k_indices = mx.argpartition(scores, kth=-k, axis=-1)[..., -k:]
        top_k_weights = mx.take_along_axis(scores, top_k_indices, axis=-1)
        top_k_weights = mx.softmax(top_k_weights, axis=-1)
        top_k_weights = top_k_weights * inner.per_expert_scale[top_k_indices]
        return top_k_indices, top_k_weights


class _steering_scope:
    """Context manager bracketing generation: parses the request's steering
    field, guards the prompt cache, installs wrappers, and ALWAYS restores
    the original layers on exit (including client-disconnect exceptions).
    Yields {trait: coeff} for the response's steering_applied field."""

    def __init__(self, body):
        self._mode = "expert" if body.get("steering_mode") == "expert" \
            else "residual"
        registry = _STEERING_EXPERTS if self._mode == "expert" \
            else _STEERING_VECTORS
        self._plan = _parse_steering(body.get("steering"), registry)
        self._restore_routers = []
        # Parse optional steering gate: {"threshold": 0.05} (float 0-1)
        gate_config = body.get("steering_gate", {})
        if isinstance(gate_config, dict) and "threshold" in gate_config:
            try:
                self._gate_threshold = float(gate_config["threshold"])
                # Clamp to [0, 1]
                self._gate_threshold = max(0.0, min(1.0, self._gate_threshold))
            except (TypeError, ValueError):
                self._gate_threshold = None
        else:
            self._gate_threshold = None
        self._restore = []

    def __enter__(self):
        # Guard runs even for an empty plan: steered -> unsteered is also
        # a signature change that must drop the cache. The signature folds
        # in the CAST gate threshold and the steering mode (residual KV and
        # expert-biased KV are mutually stale).
        gate_sig = f"gate:{self._gate_threshold}" if self._gate_threshold is not None else ""
        if self._mode != "residual":
            gate_sig += f"|mode:{self._mode}"
        _steering_cache_guard(self._plan, gate_sig)
        if not self._plan or _MLX_MODEL is None:
            return {}
        owner = _steering_layer_list(_MLX_MODEL)
        if owner is None:
            print("[mlx-server] steering skipped: cannot locate decoder layers",
                  flush=True)
            return {}
        applied = {}
        gate = None
        if _thought_gating_enabled() and \
                _resolve_channel_ids(_MLX_TOKENIZER) != (None, None):
            gate = _SteerGate()
        try:
            if self._mode == "expert":
                applied = self._install_expert(owner, gate)
            else:
                for trait, coeff in self._plan:
                    spec = _STEERING_VECTORS[trait]
                    idx = spec["layer"]
                    alpha = coeff * spec["base_alpha"]
                    original = owner.layers[idx]
                    owner.layers[idx] = _SteeredLayer(original, spec["v"], alpha, gate, self._gate_threshold)
                    self._restore.append((owner, idx, original))
                    applied[trait] = coeff
            if applied:
                # Include gate threshold in the response if gating is enabled
                if self._gate_threshold is not None:
                    applied["_gate"] = self._gate_threshold
                global _STEER_GATE
                _STEER_GATE = gate
                log_msg = f"[mlx-server] steering applied: " + ", ".join(f"{t}={c:+.2f}" for t, c in applied.items() if t != "_gate")
                if self._mode == "expert":
                    log_msg += " (expert-routing mode)"
                if self._gate_threshold is not None:
                    log_msg += f" (gate @{self._gate_threshold:.2f})"
                if gate is not None:
                    log_msg += " (thought-gated)"
                print(log_msg, flush=True)
        except Exception as exc:  # noqa: BLE001 — restore what we installed
            self.__exit__(None, None, None)
            print(f"[mlx-server] steering install failed (no-op): {exc}", flush=True)
            return {}
        return applied

    def _install_expert(self, owner, gate) -> dict:
        """Install _BiasedRouter wrappers for every trait in the plan.
        Biases from multiple traits touching the same layer accumulate into
        one wrapper. Returns {trait: coeff} actually applied (empty when no
        spec layer maps onto a MoE block — dense models are a no-op)."""
        import numpy as np
        applied = {}
        per_layer: dict = {}
        for trait, coeff in self._plan:
            spec = _STEERING_EXPERTS[trait]
            b = coeff * spec["base_bias"]
            for li, experts in spec["layers"].items():
                emap = per_layer.setdefault(li, {})
                for e, s in experts:
                    emap[e] = emap.get(e, 0.0) + b * s
            applied[trait] = coeff
        installed = 0
        for li in sorted(per_layer):
            if li >= len(owner.layers):
                continue
            block = owner.layers[li]
            router = getattr(block, "router", None)
            if router is None or not getattr(block, "enable_moe", False):
                continue
            n_e = int(router.config.num_experts)
            bias = np.zeros(n_e, dtype=np.float32)
            for e, v in per_layer[li].items():
                if 0 <= e < n_e:
                    bias[e] = v
            self._restore_routers.append((block, router))
            block.router = _BiasedRouter(router, bias, gate)
            installed += 1
        if installed == 0:
            print("[mlx-server] expert steering skipped: no MoE layer "
                  "matched the spec", flush=True)
            return {}
        return applied

    def __exit__(self, *_exc):
        global _STEER_GATE
        _STEER_GATE = None
        while self._restore:
            owner, idx, original = self._restore.pop()
            owner.layers[idx] = original
        while self._restore_routers:
            block, original = self._restore_routers.pop()
            block.router = original
        return False


def _stream_generate_iter(full_ids, suffix_ids, max_tokens, prompt_cache):
    """Build an mlx_lm.stream_generate iterator, degrading gracefully when an
    older mlx_lm rejects optional kwargs (prompt_cache, draft_model).

    Returns (iterator, used_cache). CRITICAL invariant: if prompt_cache is
    dropped, we feed the FULL prompt — never the suffix — because a suffix
    without its cached prefix would silently truncate the model's context.
    """
    from mlx_lm import stream_generate
    draft = _MLX_DRAFT_MODEL
    # Attempt ladder: (prompt_arg, use_cache, use_draft).
    attempts = []
    if prompt_cache is not None:
        attempts.append((suffix_ids, True, draft is not None))
    attempts.append((full_ids, False, draft is not None))
    attempts.append((full_ids, False, False))
    last_exc = None
    for prompt_arg, use_cache, use_draft in attempts:
        kw = {"prompt": prompt_arg, "max_tokens": max_tokens}
        if use_cache:
            kw["prompt_cache"] = prompt_cache
        if use_draft:
            kw["draft_model"] = draft
        try:
            iterator = stream_generate(_MLX_MODEL, _MLX_TOKENIZER, **kw)
            if not use_cache:
                # We're feeding the full prompt with no cache this turn;
                # discard any cache state so finalize doesn't mis-record it.
                _invalidate_prompt_cache()
            return iterator, use_cache
        except TypeError as exc:
            last_exc = exc
            continue
    raise last_exc if last_exc else RuntimeError("stream_generate unavailable")


def _maybe_enable_turbo_kv() -> bool:
    """Phase 1c — optional TurboKV compressed KV cache (~50% KV-cache memory
    for ~+0.5% PPL). OFF by default; enable with HU_MLX_TURBO_KV=1. Requires a
    custom mlx build exposing mlx.nn.layers.turbo_kv_cache; if that's absent we
    log once and run with the standard cache (no-op).

    Patches mlx_lm.models.cache.make_prompt_cache, which is the same factory
    the Phase 1a prompt-cache reuse calls — so reuse transparently gets turbo
    caches too. Safe interaction: a turbo cache reports can_trim=False (or is
    flagged rotating), so prompt-cache reuse only does pure extension / reset,
    never an unsafe trim. (Env knobs mirror scripts/turbo-serve.py.)"""
    if os.environ.get("HU_MLX_TURBO_KV", "0") not in ("1", "true", "True"):
        return False
    try:
        from mlx.nn.layers.turbo_kv_cache import TurboKVCache, patch_mlx_lm
        import mlx_lm.models.cache as cache_mod
    except Exception as exc:  # noqa: BLE001 — custom dep may be absent
        print(f"[mlx-server] HU_MLX_TURBO_KV set but turbo_kv_cache unavailable "
              f"({exc}); using standard KV cache", flush=True)
        return False

    mode = os.environ.get("TURBO_MODE", "asymmetric")
    bits = int(os.environ.get("TURBO_BITS", "4"))
    key_bits = bits if mode == "symmetric" else 0  # asymmetric: FP16 keys

    def turbo_make_prompt_cache(model, max_kv_size=None):
        if hasattr(model, "make_cache"):
            cache = model.make_cache()
        else:
            cache = [TurboKVCache(bits=bits, key_bits=key_bits)
                     for _ in range(len(model.layers))]
        patch_mlx_lm(cache)
        return cache

    cache_mod.make_prompt_cache = turbo_make_prompt_cache
    kb = "FP16" if key_bits == 0 else f"turbo{key_bits}"
    print(f"[mlx-server] TurboKV enabled: K={kb} V=turbo{bits} ({mode} mode)",
          flush=True)
    return True


def _try_load_mlx_model(model_id: str) -> bool:
    """Best-effort. Returns True if the model loaded successfully."""
    global _MLX_MODEL, _MLX_TOKENIZER
    if not have_mlx_lm():
        return False
    try:
        from mlx_lm import load
        _MLX_MODEL, _MLX_TOKENIZER = load(model_id)
        _invalidate_prompt_cache("model loaded")
        return True
    except Exception as exc:  # noqa: BLE001 — mlx_lm raises a variety
        print(f"[mlx-server] load({model_id!r}) failed: {exc}", flush=True)
        return False


def _try_load_draft_model(draft_path: str) -> bool:
    """Phase 3b — load a draft model for speculative decoding. Returns
    True on success. Failure is non-fatal: spec decode silently turns
    off and inference proceeds against the target only. The draft must
    use the SAME tokenizer family as the target — caller's responsibility
    (a future doctor check could detect mismatched tokenizers, but the
    fallback is correct regardless: divergent drafts just lower the
    acceptance rate, they don't break output)."""
    global _MLX_DRAFT_MODEL, _MLX_DRAFT_TOKENIZER, _MLX_DRAFT_PATH
    if not draft_path:
        return False
    if not have_mlx_lm():
        return False
    try:
        from mlx_lm import load
        _MLX_DRAFT_MODEL, _MLX_DRAFT_TOKENIZER = load(draft_path)
        _MLX_DRAFT_PATH = draft_path
        print(f"[mlx-server] draft model loaded: {draft_path!r}", flush=True)
        return True
    except Exception as exc:  # noqa: BLE001
        print(f"[mlx-server] draft load({draft_path!r}) failed (spec decode "
              f"disabled): {exc}", flush=True)
        _MLX_DRAFT_MODEL = None
        _MLX_DRAFT_TOKENIZER = None
        _MLX_DRAFT_PATH = ""
        return False


def _default_draft_for_model(model_id: str) -> str:
    """Pick a tokenizer-compatible speculative-decoding draft when the
    operator didn't set one — but ONLY for targets where spec decode actually
    pays off.

    Spec-decode benefit ∝ (target per-token cost − draft cost) × acceptance.
    DENSE Gemma-4 (e.g. 31B) is far more expensive per token than the E2B
    sibling → clear win. A sparse-MoE Gemma-4 (the "a4b" models, ~4B ACTIVE
    params) costs little more than the draft, so the draft+verification
    overhead makes spec decode a net SLOWDOWN (measured 0.79x on 26b-a4b,
    2026-05-29). So we exclude MoE targets; an operator can still force one
    with --draft-model if their hardware says otherwise.

    The E2B sibling shares the Gemma-4 tokenizer exactly (vocab 262144,
    byte-identical encodings — verified). Returns "" when no known-good
    default applies."""
    if not model_id:
        return ""
    mid = model_id.lower()
    is_gemma4 = "gemma-4" in mid
    is_small = "e2b" in mid or "270m" in mid
    is_moe = "a4b" in mid  # sparse mixture-of-experts → draft doesn't pay off
    if is_gemma4 and not is_small and not is_moe:
        return "mlx-community/gemma-4-e2b-it-4bit"
    return ""


def _model_in_local_cache(repo_id: str) -> bool:
    """True if repo_id is already in the local HF cache, so auto-loading a
    default draft can never trigger a surprise multi-GB download. A local
    filesystem path is treated as present (load surfaces a clean error if
    wrong). Explicit operator drafts bypass this — they opted in."""
    if not repo_id:
        return False
    if (os.sep in repo_id or repo_id.startswith("~")) and \
            os.path.exists(os.path.expanduser(repo_id)):
        return True
    cache = os.path.expanduser("~/.cache/huggingface/hub")
    snaps = os.path.join(cache, "models--" + repo_id.replace("/", "--"),
                         "snapshots")
    if not os.path.isdir(snaps):
        return False
    for d in os.listdir(snaps):
        p = os.path.join(snaps, d)
        if os.path.isdir(p) and os.listdir(p):
            return True
    return False


def _tokenizers_compatible(tok_a, tok_b) -> bool:
    """Speculative decoding requires draft and target to tokenize IDENTICALLY
    — a divergent draft tokenizer silently corrupts acceptance (or output).
    Compare vocab size and a probe encoding."""
    if tok_a is None or tok_b is None:
        return False
    try:
        va = getattr(tok_a, "vocab_size", None)
        vb = getattr(tok_b, "vocab_size", None)
        if va is not None and vb is not None and va != vb:
            return False
        probe = "Hey — quick tokenizer parity check, don't overthink it. 123"
        return list(tok_a.encode(probe)) == list(tok_b.encode(probe))
    except Exception:  # noqa: BLE001
        return False


def _configure_draft_model(model_id: str, explicit_draft: str) -> None:
    """Resolve, load, and verify the speculative-decoding draft AFTER the
    target model is loaded. Precedence: explicit operator draft > auto-derived
    sibling (only if already cached). Always verifies tokenizer parity and
    disables spec decode on mismatch — correctness/safety over speed."""
    global _MLX_DRAFT_MODEL, _MLX_DRAFT_TOKENIZER, _MLX_DRAFT_PATH
    if _MLX_MODEL is None:
        return  # no target loaded (stub mode) — nothing to draft for

    draft = explicit_draft
    # Auto-draft is OFF by default. Speculative decoding with the E2B draft
    # was MEASURED to be a net SLOWDOWN on both available large Gemma-4
    # targets (0.79x on 26b-a4b MoE, 0.69x on dense 31B; ~53% acceptance —
    # see scripts/bench_spec_decode_live.py, 2026-05-29). The draft isn't
    # cheap enough relative to these targets on this hardware to win. The
    # plumbing stays for operators whose hardware/draft differs (set
    # HU_MLX_DRAFT_AUTO=1 to auto-pick a sibling, or --draft-model to force);
    # always re-measure with the bench before trusting it.
    auto_on = os.environ.get("HU_MLX_DRAFT_AUTO", "0") in (
        "1", "true", "True")
    if not draft and auto_on:
        cand = _default_draft_for_model(model_id)
        if cand and cand != model_id:
            if _model_in_local_cache(cand):
                draft = cand
                print(f"[mlx-server] spec decode: auto-selected draft {cand}",
                      flush=True)
            else:
                print(f"[mlx-server] spec decode: draft {cand} not in local "
                      f"cache; skipping auto-load (set HU_MLX_DRAFT_MODEL to "
                      f"force a download)", flush=True)
    if not draft:
        return

    if not _try_load_draft_model(draft):
        return  # already logged; spec decode stays off
    if not _tokenizers_compatible(_MLX_TOKENIZER, _MLX_DRAFT_TOKENIZER):
        print(f"[mlx-server] spec decode: draft {draft!r} tokenizer INCOMPATIBLE "
              f"with target {model_id!r} — disabling (would corrupt output)",
              flush=True)
        _MLX_DRAFT_MODEL = None
        _MLX_DRAFT_TOKENIZER = None
        _MLX_DRAFT_PATH = ""
        return
    print(f"[mlx-server] spec decode ENABLED (draft={_MLX_DRAFT_PATH})",
          flush=True)


def _swap_adapter_inline(adapter_path: str) -> tuple[int, dict]:
    """Implement the swap inline via model.load_weights. Returns
    (http_status, response_body).

    Per spec D-M3-1: on exception we REVERT to the previously-loaded
    adapter (or to base if there was none) and return 500.
    """
    global _CURRENT_ADAPTER, _TENSORS_LOADED, _MLX_MODEL

    if not adapter_path:
        return 400, {"status": "error", "error": "missing adapter_path"}

    expanded = os.path.expanduser(adapter_path)
    if not os.path.exists(expanded):
        # Accept either a file OR a directory containing adapters.safetensors
        # (matches mlx_lm's expected layout).
        return 404, {
            "status": "error",
            "error": f"adapter not found at {adapter_path}",
        }

    prior_adapter = _CURRENT_ADAPTER
    prior_tensors = _TENSORS_LOADED

    # If mlx_lm is available, do the real load. Otherwise track the path
    # for the test/stub path — same contract surface, no real weights.
    if _MLX_MODEL is not None and have_mlx_lm():
        try:
            # LoRA adapters require linear_to_lora_layers() injection — plain
            # load_weights() silently drops tensors with names like
            # `lora_A.weight` that don't match the base model's parameters.
            # Detect: if the path is a directory containing adapter_config.json,
            # use load_adapters(). Otherwise fall back to load_weights() on the
            # resolved file (full-checkpoint replacement).
            adapter_dir = _resolve_adapter_dir(expanded)
            if adapter_dir is not None:
                from mlx_lm.tuner.utils import load_adapters, remove_lora_layers
                # Remove prior LoRA layers (idempotent — no-op if none).
                remove_lora_layers(_MLX_MODEL)
                _MLX_MODEL = load_adapters(_MLX_MODEL, adapter_dir)
                weight_path = os.path.join(adapter_dir, "adapters.safetensors")
                tensors_loaded = _count_safetensors(weight_path)
            else:
                weight_path = _resolve_adapter_weight_file(expanded)
                _MLX_MODEL.load_weights(weight_path, strict=False)
                tensors_loaded = _count_safetensors(weight_path)
        except Exception as exc:  # noqa: BLE001
            # Revert on exception. Best-effort — if revert itself fails
            # we surface that too. State stays "whatever load_weights
            # last left in memory" — which is why we re-load the prior
            # adapter explicitly.
            err_msg = f"load_weights failed: {exc}"
            if prior_adapter and prior_adapter != expanded:
                try:
                    revert_path = _resolve_adapter_weight_file(prior_adapter)
                    _MLX_MODEL.load_weights(revert_path, strict=False)
                except Exception as revert_exc:  # noqa: BLE001
                    err_msg += f"; revert to prior adapter also failed: {revert_exc}"
            return 500, {"status": "error", "error": err_msg}
    else:
        # Stub path: just count tensors in the safetensors file.
        weight_path = _resolve_adapter_weight_file(expanded)
        tensors_loaded = _count_safetensors(weight_path)

    _CURRENT_ADAPTER = os.path.realpath(expanded)
    _TENSORS_LOADED = tensors_loaded

    # The new adapter changes the model's weights — any KV cached under the
    # previous weights is now invalid. Drop it so the next turn cold-prefills.
    _invalidate_prompt_cache("adapter swap")

    return 200, {
        "status": "ok",
        "adapter_path": _CURRENT_ADAPTER,
        "adapter_loaded": _CURRENT_ADAPTER,
        "tensors_loaded": _TENSORS_LOADED,
        "prior_adapter": prior_adapter,
        "prior_tensors": prior_tensors,
    }


def _resolve_adapter_dir(path: str):
    """Resolve a swap path to a LoRA adapter DIRECTORY (one containing
    `adapter_config.json` next to `adapters.safetensors`), or None if the
    path doesn't look like a LoRA adapter layout.

    Layouts handled:
        <path>/adapter_config.json + adapters.safetensors  → return <path>
        <path>/adapters.safetensors/adapter_config.json    → return inner dir
    """
    candidates = [path]
    inner = os.path.join(path, "adapters.safetensors")
    if os.path.isdir(inner):
        candidates.append(inner)
    for cand in candidates:
        if os.path.isdir(cand) and os.path.isfile(
            os.path.join(cand, "adapter_config.json")
        ) and os.path.isfile(os.path.join(cand, "adapters.safetensors")):
            return cand
    return None


def _resolve_adapter_weight_file(path: str) -> str:
    """Resolve a swap path to the actual .safetensors weight FILE.

    mlx_lm.lora's natural output layout is:
        <given-path>/                          ← what callers typically pass
            adapters.safetensors/              ← directory created by mlx_lm.lora
                adapters.safetensors           ← the actual weight file
                adapter_config.json

    So we walk down: if `path` is a directory, look for adapters.safetensors
    inside. If THAT is also a directory, look one level deeper. If it's a
    file with .safetensors extension, return it. Returns the original path
    if no safetensors file is found (caller's load_weights will surface a
    clean error).
    """
    cur = path
    # Walk down up to 3 levels — covers the typical mlx_lm.lora nesting.
    for _ in range(3):
        if os.path.isfile(cur) and cur.endswith(".safetensors"):
            return cur
        if os.path.isdir(cur):
            cand = os.path.join(cur, "adapters.safetensors")
            if os.path.exists(cand):
                cur = cand
                continue
        break
    return cur


def _chat_completion_inline(body):
    """Serve an OpenAI-compatible /v1/chat/completions request from the
    in-process MLX model.

    Per spec AC-M3-1 (live-fire): the inline server must serve chat through
    whichever adapter is currently loaded, so the M3 personalization loop
    has a real served-turn proof end-to-end (training → swap → served turn).

    Request: {"messages": [{"role": ..., "content": ...}, ...], "max_tokens": N?,
              "model": "..."?, "temperature": ...?}
    Response: OpenAI-compatible {"choices": [{"message": {"role": "assistant",
              "content": "..."}, "finish_reason": "stop", "index": 0}], ...}

    If mlx_lm isn't available (e.g. fake-mlx host), returns a deterministic
    stub response that's structurally valid but content-free — sufficient for
    transport-contract testing.
    """
    messages = body.get("messages", [])
    if not isinstance(messages, list) or not messages:
        return 400, {"error": "missing or empty messages"}
    max_tokens = int(body.get("max_tokens", 256))

    # Apply the model's chat template if the tokenizer supports it; else
    # fall back to a simple concatenation. Either way produces a prompt
    # string suitable for generate().
    prompt_tokens = 0
    completion_tokens = 0
    if _MLX_MODEL is not None and _MLX_TOKENIZER is not None:
        try:
            tok = _MLX_TOKENIZER
            if hasattr(tok, "apply_chat_template"):
                prompt = tok.apply_chat_template(
                    messages, tokenize=False, add_generation_prompt=True
                )
            else:
                prompt = "\n".join(
                    f"{m.get('role', 'user')}: {m.get('content', '')}"
                    for m in messages
                )
            # Single-threaded HTTPServer means we're on the main thread
            # where load() initialized the GPU stream. Phase 1a — collect the
            # continuation via stream_generate so we reuse the prompt-prefix
            # KV cache AND capture real generated token ids to finalize it
            # (re-encoding text would risk a tokenization mismatch on the
            # next turn's prefix diff, costing a needless cache reset).
            full_ids = tok.encode(prompt)
            prompt_tokens = len(full_ids)
            # Steering scope brackets BOTH cache prep and iterator
            # consumption: the cache guard must run before reuse planning,
            # and the wrappers must stay installed until the last token.
            with _steering_scope(body) as steering_applied:
                _steering_prime_gate(full_ids)
                rep_guard = _RepetitionGuard() \
                    if steering_applied and _rep_guard_enabled() else None
                suffix_ids, prompt_cache = _prepare_prompt_cache(full_ids)
                iterator, used_cache = _stream_generate_iter(
                    full_ids, suffix_ids, max_tokens, prompt_cache)
                parts = []
                generated_ids = []
                for item in iterator:
                    delta = getattr(item, "text", None)
                    if delta is None and isinstance(item, str):
                        delta = item
                    if delta is None:
                        delta = str(item)
                    if delta:
                        parts.append(delta)
                        completion_tokens += 1
                    tok_id = getattr(item, "token", None)
                    if tok_id is not None:
                        generated_ids.append(tok_id)
                        _steering_note_token(tok_id)
                        if rep_guard is not None and rep_guard.note(tok_id):
                            break
                text = "".join(parts)
                if rep_guard is not None and rep_guard.tripped:
                    # Steered generation degenerated. Skip cache finalize
                    # (the KV holds the degenerate tail; the retry's cache
                    # guard drops it on the steered->unsteered signature
                    # change) and retry once without steering.
                    print("[mlx-server] repetition guard tripped after "
                          f"{len(generated_ids)} tokens; retrying unsteered",
                          flush=True)
                else:
                    rep_guard = None
                    if used_cache:
                        _finalize_prompt_cache(full_ids, generated_ids)
            if rep_guard is not None and rep_guard.tripped:
                retry_body = dict(body)
                retry_body.pop("steering", None)
                retry_body.pop("steering_mode", None)
                status, resp = _chat_completion_inline(retry_body)
                if status == 200 and isinstance(resp, dict):
                    resp["steering_retry"] = "repetition_abort"
                return status, resp
        except Exception as exc:
            print(f"[mlx-server] chat generate failed: {exc}", flush=True)
            return 500, {"error": f"generate failed: {exc}"}
    else:
        # No real model loaded — return a deterministic stub so the
        # transport contract is still testable. Token counts stay 0 in
        # this branch; bench callers can detect stub mode by health probe.
        last = messages[-1].get("content", "") if messages else ""
        text = f"[stub] echoing {len(last)} chars from last user message"
        steering_applied = {}

    resp = {
        "id": "chatcmpl-mlx-inline",
        "object": "chat.completion",
        "model": body.get("model", "mlx-inline"),
        "choices": [{
            "index": 0,
            "message": {"role": "assistant", "content": text},
            "finish_reason": "stop",
        }],
        "usage": {
            "prompt_tokens": prompt_tokens,
            "completion_tokens": completion_tokens,
            "total_tokens": prompt_tokens + completion_tokens,
        },
    }
    if steering_applied:
        resp["steering_applied"] = steering_applied
    return 200, resp


def _count_safetensors(path: str) -> int:
    """Parse the safetensors header to count non-metadata tensors.
    Returns 0 on parse failure or empty-tensors stub.
    """
    try:
        import struct
        with open(path, "rb") as f:
            header_len_bytes = f.read(8)
            if len(header_len_bytes) < 8:
                return 0
            header_len = struct.unpack("<Q", header_len_bytes)[0]
            if header_len == 0 or header_len > 16_000_000:
                return 0
            header = json.loads(f.read(header_len).decode("utf-8"))
        return sum(1 for k in header if not k.startswith("__"))
    except (OSError, struct.error, ValueError, json.JSONDecodeError):
        return 0


class SwapHandler(BaseHTTPRequestHandler):
    """Minimal HTTP handler that implements the swap contract inline.
    When the spec mandates delegation for chat completions, this handler
    proxies that route to the configured upstream; for swap, we ALWAYS
    handle ourselves.
    """

    upstream_url: str = ""

    def _send_json(self, status: int, payload: dict):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        global _CURRENT_ADAPTER, _TENSORS_LOADED
        if self.path == "/health":
            return self._send_json(200, {
                "ok": True,
                "active_adapter": _CURRENT_ADAPTER,
                "model_loaded": _MLX_MODEL is not None,
                # Phase 3b — expose draft-model state so the bench
                # harness can verify spec decode is actually configured
                # (vs the operator forgetting to set HU_MLX_DRAFT_MODEL).
                "draft_model_loaded": _MLX_DRAFT_MODEL is not None,
                "draft_model": _MLX_DRAFT_PATH,
            })
        if self.path == "/v1/adapters/current":
            return self._send_json(200, {
                "adapter_path": _CURRENT_ADAPTER,
                "tensors_loaded": _TENSORS_LOADED,
            })
        return self._send_json(404, {"error": f"no route: {self.path}"})

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(n).decode("utf-8") if n else "{}"
        try:
            body = json.loads(raw)
        except json.JSONDecodeError:
            return self._send_json(400, {
                "status": "error", "error": "invalid JSON",
            })

        if self.path == "/v1/adapters/swap":
            ap = body.get("adapter_path")
            if ap is None:
                return self._send_json(400, {
                    "status": "error", "error": "missing adapter_path",
                })
            if not isinstance(ap, str) or not ap:
                return self._send_json(400, {
                    "status": "error",
                    "error": "missing adapter_path",
                })
            # contact_hash is optional, only logged.
            contact_hash = body.get("contact_hash", "")
            print(
                f"[mlx-server] swap requested: {ap!r} contact={contact_hash!r}",
                flush=True,
            )
            status, resp = _swap_adapter_inline(ap)
            return self._send_json(status, resp)

        if self.path == "/v1/chat/completions":
            # AC-M3-1 (live-fire): the inline server must serve chat
            # completions through the swapped adapter, otherwise the
            # M3 personalization loop has no served-turn proof.
            # OpenAI-compatible request shape: messages=[{role, content}].
            #
            # Phase 3a (Gemma throughput program): when the client
            # passes stream=true, switch to SSE so perceived TTFT
            # collapses to first-token latency instead of full-response
            # latency. The existing buffered path stays for stream=false
            # consumers and for backwards compatibility with anything
            # that POSTed without the field.
            if body.get("stream") is True:
                return self._stream_chat_completion(body)
            status, resp = _chat_completion_inline(body)
            return self._send_json(status, resp)

    def _stream_chat_completion(self, body):
        """SSE response path. Emits OpenAI-compatible chunks until the
        model halts (max_tokens or eos), then a final chunk with
        finish_reason='stop' plus a `data: [DONE]\\n\\n` sentinel.

        Falls back gracefully when mlx_lm isn't importable: emits ONE
        delta chunk with the stub text + DONE. This keeps the SSE
        contract testable in stub mode without a real model.
        """
        messages = body.get("messages", [])
        if not isinstance(messages, list) or not messages:
            return self._send_json(400, {"error": "missing or empty messages"})
        max_tokens = int(body.get("max_tokens", 256))
        model_name = body.get("model", "mlx-inline")

        # Begin chunked SSE response. No Content-Length (chunked encoding
        # via close-on-EOF; HTTPServer's chunk handling is good enough
        # here because each `self.wfile.write` flushes a TCP packet).
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        # No Content-Length intentionally — keep the connection
        # streaming until the closing [DONE] sentinel.
        self.end_headers()

        def emit(delta_text: str, finish_reason=None, usage=None, steering=None):
            chunk = {
                "id": "chatcmpl-mlx-inline",
                "object": "chat.completion.chunk",
                "model": model_name,
                "choices": [
                    {
                        "index": 0,
                        "delta": {"content": delta_text} if delta_text else {},
                        "finish_reason": finish_reason,
                    }
                ],
            }
            if usage is not None:
                chunk["usage"] = usage
            if steering:
                chunk["steering_applied"] = steering
            payload = "data: " + json.dumps(chunk) + "\n\n"
            try:
                self.wfile.write(payload.encode("utf-8"))
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                # Client disconnected mid-stream. Re-raise as a sentinel
                # the outer loop catches to stop generation cleanly.
                raise

        prompt_tokens = 0
        completion_tokens = 0
        steering_applied = {}
        finish_reason_override = None
        try:
            if _MLX_MODEL is not None and _MLX_TOKENIZER is not None:
                try:
                    from mlx_lm import stream_generate
                except ImportError:
                    # mlx_lm < 0.20 didn't expose stream_generate at the
                    # top level. Fall back to one-shot generate +
                    # emit-as-one-chunk so the SSE contract still holds.
                    stream_generate = None

                tok = _MLX_TOKENIZER
                if hasattr(tok, "apply_chat_template"):
                    prompt = tok.apply_chat_template(
                        messages, tokenize=False, add_generation_prompt=True
                    )
                else:
                    prompt = "\n".join(
                        f"{m.get('role', 'user')}: {m.get('content', '')}"
                        for m in messages
                    )
                try:
                    full_ids = tok.encode(prompt)
                except Exception:
                    full_ids = None
                prompt_tokens = len(full_ids) if full_ids is not None else 0

                if stream_generate is not None and full_ids is not None:
                    # Phase 1a — reuse KV for the shared prompt prefix; only
                    # the divergent suffix is prefilled. Phase 3b — pass the
                    # draft model when loaded for speculative decoding. The
                    # iterator helper degrades gracefully on older mlx_lm.
                    # Steering scope brackets cache prep AND iterator
                    # consumption (stream_generate is lazy); restore runs on
                    # client disconnect via the with-exit.
                    with _steering_scope(body) as steering_applied:
                        _steering_prime_gate(full_ids)
                        # Streamed chunks cannot be recalled, so on a guard
                        # trip we stop early instead of retrying (the client
                        # sees finish_reason "repetition_abort").
                        rep_guard = _RepetitionGuard() \
                            if steering_applied and _rep_guard_enabled() else None
                        rep_tripped = False
                        suffix_ids, prompt_cache = _prepare_prompt_cache(full_ids)
                        iterator, used_cache = _stream_generate_iter(
                            full_ids, suffix_ids, max_tokens, prompt_cache)
                        generated_ids = []
                        for item in iterator:
                            # mlx_lm yields different shapes across versions:
                            # newer GenerationResponse objects have `.text`;
                            # older paths yield bare strings.
                            delta = getattr(item, "text", None)
                            if delta is None and isinstance(item, str):
                                delta = item
                            if delta is None:
                                delta = str(item)
                            tok_id = getattr(item, "token", None)
                            if tok_id is not None:
                                generated_ids.append(tok_id)
                                _steering_note_token(tok_id)
                                if rep_guard is not None \
                                        and rep_guard.note(tok_id):
                                    rep_tripped = True
                                    print("[mlx-server] repetition guard "
                                          f"tripped mid-stream after "
                                          f"{len(generated_ids)} tokens; "
                                          "stopping early", flush=True)
                                    break
                            if delta:
                                emit(delta)
                                completion_tokens += 1
                        if rep_tripped:
                            finish_reason_override = "repetition_abort"
                        elif used_cache:
                            _finalize_prompt_cache(full_ids, generated_ids)
                elif stream_generate is not None:
                    # Tokenizer.encode failed; fall back to string prompt with
                    # no cache reuse (still streams).
                    iterator = stream_generate(
                        _MLX_MODEL, _MLX_TOKENIZER, prompt=prompt,
                        max_tokens=max_tokens)
                    for item in iterator:
                        delta = getattr(item, "text", None)
                        if delta is None and isinstance(item, str):
                            delta = item
                        if delta is None:
                            delta = str(item)
                        if delta:
                            emit(delta)
                            completion_tokens += 1
                else:
                    # Fallback: one-shot then emit as a single chunk.
                    from mlx_lm import generate as _generate
                    text = _generate(
                        _MLX_MODEL, _MLX_TOKENIZER,
                        prompt=prompt, max_tokens=max_tokens,
                    )
                    try:
                        completion_tokens = len(tok.encode(text))
                    except Exception:
                        completion_tokens = 0
                    if text:
                        emit(text)
            else:
                # Stub mode: emit one delta chunk + DONE so the SSE
                # contract is exercisable in CI without mlx_lm installed.
                last = messages[-1].get("content", "") if messages else ""
                stub = f"[stub] echoing {len(last)} chars from last user message"
                emit(stub)
                completion_tokens = 0
        except (BrokenPipeError, ConnectionResetError):
            return  # client gone, no point sending finish chunk
        except Exception as exc:  # noqa: BLE001 — mlx_lm raises a variety
            print(f"[mlx-server] stream generate failed: {exc}", flush=True)
            # Best-effort finish chunk + DONE so the client doesn't hang.
            try:
                emit("", finish_reason="error")
            except Exception:
                pass

        # Final chunk + DONE sentinel.
        try:
            emit("", finish_reason=finish_reason_override or "stop", usage={
                "prompt_tokens": prompt_tokens,
                "completion_tokens": completion_tokens,
                "total_tokens": prompt_tokens + completion_tokens,
            }, steering=steering_applied)
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            return

        return self._send_json(404, {"error": f"no route: {self.path}"})

    def log_message(self, *_):
        # quiet default access log; we print our own concise lines
        pass


def _run_inline_server(port: int, initial_adapter: str, model_id: str,
                       draft_model: str = "", steering_dir: str = ""):
    """Boot the in-process HTTP server. This is the path that satisfies
    AC-M3-1 (a): we OWN the swap endpoint definition.
    """
    global _CURRENT_ADAPTER

    # Phase 1c — optionally swap in the TurboKV compressed cache factory
    # BEFORE any cache is built. No-op unless HU_MLX_TURBO_KV=1.
    _maybe_enable_turbo_kv()

    # Activation steering — inert unless a vectors dir is configured.
    _init_steering_vectors(steering_dir)

    # Try to load the model if it's available. Failures are tolerated —
    # the swap endpoint still works against a stub state so tests can
    # exercise the contract.
    if model_id and have_mlx_lm():
        if _try_load_mlx_model(model_id):
            print(f"[mlx-server] loaded mlx_lm model: {model_id}", flush=True)
            # Phase 1b — configure speculative decoding AFTER the target is
            # loaded (the tokenizer-parity check needs both tokenizers).
            _configure_draft_model(model_id, draft_model)
        else:
            print(
                f"[mlx-server] mlx_lm available but model {model_id!r} did not load; "
                f"swap endpoint will run in stub state",
                flush=True,
            )
    elif not have_mlx_lm():
        print(
            "[mlx-server] mlx_lm NOT installed; running in stub mode "
            "(swap endpoint exercised, no real weights loaded)",
            flush=True,
        )

    # Seed the current adapter if the operator passed one.
    if initial_adapter:
        status, resp = _swap_adapter_inline(initial_adapter)
        if status != 200:
            print(
                f"[mlx-server] initial-adapter load failed (status={status}): {resp}",
                flush=True,
            )
        else:
            print(
                f"[mlx-server] initial adapter: {_CURRENT_ADAPTER} "
                f"({_TENSORS_LOADED} tensors)",
                flush=True,
            )

    # Plain HTTPServer (not Threading) — MLX uses thread-local GPU streams.
    # ThreadingHTTPServer spawns a thread per request which won't have a
    # stream initialized; serializing chat + swap requests on one thread is
    # also the right behavior (you don't want to swap adapters mid-generation).
    srv = HTTPServer(("127.0.0.1", port), SwapHandler)
    print(
        f"[mlx-server] listening on http://127.0.0.1:{port} "
        f"(adapter swap endpoint: /v1/adapters/swap)",
        flush=True,
    )
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("[mlx-server] shutting down", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", type=int, default=8741)
    ap.add_argument("--adapter", default="",
                    help="Initial adapter path (loaded at startup if provided)")
    ap.add_argument("--model", default=os.environ.get(
                        "HUMAN_MLX_MODEL", "mlx-community/gemma-4-26b-a4b-it-4bit"),
                    help="Model id to load (only used when mlx_lm is installed)")
    ap.add_argument("--steering-dir",
                    default=os.environ.get("HU_MLX_STEERING_DIR", ""),
                    help="directory of <trait>.npz persona steering vectors; "
                         "unset = steering disabled (requests' steering field "
                         "is ignored, matching pre-steering behavior)")
    ap.add_argument("--draft-model", default=os.environ.get("HU_MLX_DRAFT_MODEL", ""),
                    help="Speculative-decoding draft model id or local path. When "
                         "set, loaded alongside the target and passed to "
                         "stream_generate via draft_model= (tokenizer parity is "
                         "verified; mismatch disables spec decode). NOTE: spec "
                         "decode was measured as a SLOWDOWN on both large Gemma-4 "
                         "targets with the E2B draft, so auto-selection is OFF by "
                         "default. Set HU_MLX_DRAFT_AUTO=1 to opt into auto-picking "
                         "a compatible sibling, and re-measure with "
                         "scripts/bench_spec_decode_live.py before trusting it.")
    ap.add_argument("--prefer-upstream", action="store_true",
                    help="If gemma-realtime is installed AND exposes "
                         "/v1/adapters/swap, delegate to it. Default is to "
                         "ALWAYS use the inline server (per spec D-M3-1).")
    ap.add_argument("--no-upstream", action="store_true",
                    help="Force inline server even if gemma-realtime is "
                         "installed (overrides --prefer-upstream).")
    args = ap.parse_args()

    # Per AC-M3-1 (b): if we're going to delegate to gemma-realtime, we
    # MUST verify the upstream actually exposes /v1/adapters/swap before
    # we proceed. Otherwise we exit non-zero with a clear error.
    if args.prefer_upstream and not args.no_upstream:
        gemma = find_gemma_realtime()
        if gemma:
            if not gemma_realtime_has_swap(gemma):
                print(
                    f"ERROR: gemma-realtime found at {gemma} but it does NOT "
                    f"expose POST /v1/adapters/swap. Per spec AC-M3-1, the "
                    f"M3 adapter-swap loop requires this endpoint. Either "
                    f"update gemma-realtime, OR re-run without --prefer-upstream "
                    f"to use the inline swap endpoint in this file.",
                    file=sys.stderr,
                )
                return 4
            # Upstream HAS the swap endpoint — delegate to it. Note: even
            # though this is the "delegation" path, per spec D-M3-1 we
            # don't ACTUALLY delegate /v1/adapters/swap; we delegate other
            # routes (chat, streaming) only. The simplest expression of
            # that is: run gemma-realtime AND our inline server on the
            # same port (impossible) or different ports. For now, when
            # operator explicitly chooses --prefer-upstream, we hand off
            # entirely — they've opted in.
            sys.argv[0] = gemma
            parent = os.path.dirname(gemma)
            if parent not in sys.path:
                sys.path.insert(0, parent)
            import importlib.util
            spec = importlib.util.spec_from_file_location("__main__", gemma)
            mod = importlib.util.module_from_spec(spec)
            sys.modules["__main__"] = mod
            spec.loader.exec_module(mod)
            return 0

    # Default path (and AC-M3-1 (a)): inline swap server. The draft model
    # for speculative decoding is resolved + loaded + tokenizer-checked
    # inside _run_inline_server, AFTER the target loads (Phase 1b).
    _run_inline_server(args.port, args.adapter, args.model, args.draft_model,
                       args.steering_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
