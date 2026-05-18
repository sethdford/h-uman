---
title: M3 MLX Bridge — Execution Plan
status: in-progress (first slice landed 2026-05-17)
owner: ML subsystem
created: 2026-05-17
parent: docs/plans/2026-05-10-m3-frontier-model-bridge.md
related:
  - CLAUDE.md (M3 row in Strategic Missions)
  - src/ml/m3_frontier_adapter.c
  - src/providers/mlx.c
  - src/providers/llamacpp.c
  - docs/plans/2026-05-11-init-04-mlx-qwen3-provider.md
---

# Why this doc exists

The parent plan (`2026-05-10-m3-frontier-model-bridge.md`) is a great
honest accounting of what M3 means and where each bridge sits. It is
NOT a phased execution plan — it reads as five independent bridge
designs (A / A.0 / A.0.5 / B / C), each with internal phases.

This doc is the **phased, sequenceable, verifier-shaped execution
plan** for Bridge B (MLX) — the path the SOTA roadmap calls out as
"the primary personalization path for the M-series target market."

It opens with the observability slice that just landed because that
slice is a *precondition* for every later phase: the only honest way
to know a tensor-bearing implementation actually wired up is for the
seam to be observable from the test layer, and right now it is.

# Honest status (matches the next CLAUDE.md M3 line)

> **M3** — Bridge A daemon-pattern proven; **Bridge B first slice
> (probe-counter seam) landed 2026-05-17**; Bridge B inference,
> training, streaming, LoRA loading not yet started. Tensor
> personalization signal still gated on B1.

What this PR ships:
- `hu_m3_frontier_adapter_probe_infer` + `hu_m3_frontier_adapter_probe_count`
  (`include/human/ml/m3_frontier_adapter.h`, `src/ml/m3_frontier_adapter.c`).
- All 11 existing `hu_agent_m3_on_provider_success` chat-path call
  sites now mutate observable state — a regression that drops any of
  them is detected by `test_m3_agent_on_provider_success_advances_probe_count`.
- `hu_m3_frontier_adapter_noop_infer` retained as a backwards-compat
  shim — zero call-site churn for this slice.

What this PR explicitly does NOT ship:
- Any tensor computation. The "probe" is a counter; it is not
  personalization. The user's chat output is byte-identical to a
  build with M3 fully disabled. The seam being observable is a
  prerequisite for personalization, not personalization itself.
- Any MLX runtime linkage. The MLX provider remains a NOT_SUPPORTED
  stub. The llama.cpp provider remains a NOT_SUPPORTED stub until
  `HU_ENABLE_LLAMACPP=ON` and libllama is reachable.

# Phases

The phases are ordered by what *blocks what*. Each phase has a
verifier contract — a sentence describing the test that, when green,
constitutes "this phase shipped."

## Phase B-pre — Observability slice (DONE 2026-05-17)

- **Scope:** convert `noop_infer` to `probe_infer` + counter so the
  existing 11 chat-path call sites are testable.
- **Effort:** ~half a day.
- **Dependencies:** none.
- **Risk:** none — counter is allocated as part of the adapter
  struct, nothing else moves.
- **Verifier contract:** with a fixture adapter attached to an
  agent, calling `hu_agent_m3_on_provider_success(agent)` N times
  makes `hu_m3_frontier_adapter_probe_count(agent->m3_adapter)`
  equal to N.
  - Test: `tests/test_ml.c::test_m3_agent_on_provider_success_advances_probe_count`.
  - Plus three unit tests on the adapter directly: zero-start, increment
    semantics, NULL-safety.

## Phase B1 — MLX runtime linkage (NOT STARTED)

- **Scope:** make `cmake --preset dev` actually link an MLX runtime
  on Apple Silicon when `HU_ENABLE_MLX_PROVIDER=ON`. Three plausible
  approaches in order of pragmatism:
  1. **Subprocess to `python3 -m mlx_lm.generate`** — the path the
     existing MLX provider stub already comments at line 73 of
     `src/providers/mlx.c`. Zero new C link dependency; we cost the
     Python interpreter startup per chat.
  2. **HTTP client to a long-running `mlx-server.sh`** — the path
     the parent plan suggests (Phase B.1 there). Amortizes Python
     startup; adds a daemon lifecycle and a port to manage.
  3. **Direct `libmlx` link** — eliminates Python entirely. Largest
     dependency surface; ABI churn risk.
- **Effort:** ~2 weeks for approach 1 with full test coverage,
  ~4-6 weeks for approach 2 (server lifecycle + health probe + crash
  recovery), ~8+ weeks for approach 3 (ABI work + cross-compile).
- **Dependencies:** Phase B-pre (so the seam wiring is provable
  before we try to slot a real model under it).
- **Risk:** Python runtime version drift, MLX version pinning,
  Apple Silicon-only (must NOT_SUPPORTED on Linux / Intel Mac
  without crashing).
- **Verifier contract:** with `HU_ENABLE_MLX_PROVIDER=ON` on Apple
  Silicon, calling `mlx_chat` with a known fixture prompt returns a
  non-error, non-empty response. Off-arch builds return
  `HU_ERR_NOT_SUPPORTED` cleanly.
  - Test additions: `tests/test_mlx_provider.c::test_mlx_chat_subprocess_round_trip`
    (gated on `HU_ENABLE_MLX_PROVIDER` + `__APPLE__` + `__arm64__`).
  - Test that already exists and must stay green:
    `test_provider_all.c::test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat`.

## Phase B2 — Model loading (depends on B1)

- **Scope:** `mlx_provider_create` accepts a model identifier
  (HuggingFace ID or local safetensors directory), validates it
  exists, and primes the subprocess / server with `--model <id>`
  ahead of the first chat call. Adds a config field
  `personalization.mlx_model_path` (parser + serializer + merge).
- **Effort:** ~1 week.
- **Dependencies:** B1.
- **Risk:** path expansion (`~/`), permission, model-not-found
  classification (NOT_SUPPORTED vs IO).
- **Verifier contract:** `hu_mlx_provider_create` with a present
  model returns HU_OK; with a missing path returns a stable
  classification (currently spec'd as `HU_ERR_NOT_SUPPORTED` to match
  the dispatcher safety contract).
  - Test: `test_mlx_provider.c::test_mlx_provider_create_resolves_model_path`.

## Phase B3 — Inference call (depends on B2)

- **Scope:** real `mlx_chat` body — invoke the MLX runtime, capture
  generated tokens, parse into `hu_chat_response_t`. The probe
  counter from B-pre stays in place; the body of `probe_infer`
  gets a new wire-in that records inference outcome metadata
  (token count, ms) onto the adapter for the eventual learning
  signal.
- **Effort:** ~2 weeks (including streaming-vs-batch decision).
- **Dependencies:** B2.
- **Risk:** OOM on first big context, timeout handling, error
  propagation surface (Python traceback → C error code).
- **Verifier contract:** end-to-end chat call against a tiny MLX
  model produces a deterministic-greedy completion of a known
  prompt. Probe counter advances exactly once per successful chat.
  - Test: `test_mlx_provider.c::test_mlx_chat_greedy_completion_matches_fixture`.
  - Plus an adapter-side test: `test_ml.c::test_m3_probe_count_advances_once_per_chat`.

## Phase B4 — Streaming (depends on B3)

- **Scope:** wire `mlx_vtable.stream_chat` to the subprocess'
  token-by-token output, fan it through `hu_chat_response_chunk`
  the same way `llamacpp.c` does. Wire `supports_streaming` to
  return true.
- **Effort:** ~1 week.
- **Dependencies:** B3.
- **Risk:** chunk boundary correctness, cancellation, partial-UTF-8
  at chunk edges.
- **Verifier contract:** streaming a 50-token completion delivers
  N≥2 chunks summing to the same string a batch chat would return,
  and a mid-stream cancel cleanly terminates the subprocess.
  - Test: `test_mlx_provider.c::test_mlx_stream_chat_chunks_equal_batch`.
  - Test: `test_mlx_provider.c::test_mlx_stream_chat_cancellation_terminates_subprocess`.

## Phase B5 — LoRA adapter loading (depends on B3)

- **Scope:** wire `mlx_load_adapter` and `mlx_active_adapter`.
  Accept an `adapter_path` (safetensors LoRA from `mlx_lm.lora`),
  pass to the subprocess as `--adapter-path`, return active ID via
  the vtable. Bridges `personalization.lora_adapter_path` from the
  daemon's config-driven auto-load into the live MLX chat path.
- **Effort:** ~1 week.
- **Dependencies:** B3.
- **Risk:** safetensors validation, adapter / base shape mismatch,
  rollback path (when `unload_adapter` is called mid-conversation).
- **Verifier contract:** loading a known-good adapter measurably
  changes greedy output on a held-out prompt versus the same model
  without the adapter. Loading a malformed adapter returns
  `HU_ERR_INVALID_ARGUMENT` and leaves the active adapter NULL.
  - Test: `test_mlx_provider.c::test_mlx_lora_adapter_biases_completion`.
  - Test: `test_mlx_provider.c::test_mlx_lora_adapter_malformed_safetensors_rejected`.

# What success looks like

M3 is shipped — in the sense the CLAUDE.md row can drop the
"narrative doesn't match code" honesty caveat — when **all five
items** below are true.

1. Phase B5 verifier contract is green. (Loading a persona-LoRA
   adapter measurably changes the model the user actually chats with
   on Apple Silicon.)
2. `human ml lora-persona --backend mlx` trains a real LoRA against
   a real MLX base from the user's persona bank, end-to-end, with
   the adapter persisted to `~/.human/adapters/<persona-id>/`.
3. The daemon, with `provider: "mlx"` configured, auto-loads the
   trained adapter at startup and applies it to every chat turn.
4. A nightly persona-fidelity eval (`src/eval/persona_fidelity.c`)
   shows the adapter improves fidelity scores by a non-noise margin
   versus the base model on a held-out persona-anchor set.
5. CLAUDE.md M3 row updates from "Bridge A daemon-pattern proven" to
   "Personal LoRA fine-tune on Apple Silicon ships a measurable
   persona-fidelity delta against the frontier base."

Items 1-3 are coding work in this plan's phases. Item 4 plugs into
the existing fidelity harness; the work is choosing a stable anchor
set and writing the cron entry. Item 5 is a one-line CLAUDE.md edit
made when 1-4 are green.

# Out of scope here

- Bridge A (llama.cpp) phases beyond what already shipped — tracked
  in the parent plan.
- Bridge C (any-open-weight unified provider) — explicitly
  punted in the parent plan.
- Continuous-learning / nightly retraining cron — that is SOTA roadmap
  Phase A5 work; the MLX bridge feeds into it, not the other way
  around.
- iOS/iPadOS MLX inference — separate harness and lifecycle.
