---
title: M3 Frontier Model Bridge — Honest Gap and Migration Plan
status: planned (multi-month)
owner: ML subsystem
created: 2026-05-10
related:
  - CLAUDE.md (M3 row in Strategic Missions)
  - AGENTS.md
  - src/ml/CLAUDE.md
  - docs/ml-subsystem-audit-2026-03.md
---

# M3 status, honestly

The CLAUDE.md M3 row says:

> **Hardest. Narrative doesn't match code.** `lora-persona` trains a
> reference GPT on example banks, not the frontier chat model.
> `--checkpoint` is accepted but `(void)checkpoint_path`. CPU-only.
> ggml "planned."

This document captures what's actually shipped, what's missing, and the
multi-month plan to close the gap honestly.

# Reality of the current ML subsystem (2026-05)

| Capability | What we have | What "M3" claims |
|---|---|---|
| BPE tokenizer | ✅ `src/ml/tokenizer_bpe.c` | ✅ |
| Reference GPT (RMSNorm, RoPE, GQA) | ✅ `src/ml/gpt.c` (~32M params, FP32) | ✅ |
| MuonAdamW optimizer | ✅ | ✅ |
| LoRA adapters | ✅ Q/V default, K/O optional | ✅ |
| DPO training loop | ✅ `src/ml/dpo.c` | ✅ |
| Checkpoint save/load (HUML format) | ✅ `src/ml/checkpoint.c` | ✅ |
| `lora-persona` CLI **on reference GPT** | ✅ (this doc's point of contact) | ✅ |
| `--checkpoint` flag actually used | ✅ as of FIX 15 — warm-starts the **reference** GPT | ❌ — does not load Llama/Qwen/Mistral |
| Fine-tune the frontier model the user actually chats with | ❌ | ✅ ("personalize the model the user talks to") |
| GPU acceleration (Metal / CUDA / MLX) | ❌ CPU only | ✅ on Apple Silicon |
| HuggingFace / GGUF checkpoint loader | ❌ | ✅ |
| Adapter inference at chat time (apply LoRA to chat provider) | ❌ | ✅ |
| **D0.3 seam in-tree** — `hu_m3_frontier_adapter_*` fixture load + noop infer (tests) | ✅ `human/ml/m3_frontier_adapter.h` | ✅ **Chat wiring:** `hu_agent_m3_on_provider_success` after every successful frontier provider round in `agent_turn.c` + `agent_stream.c` (primary `chat` / `stream_chat`, GVR, constitutional, metacog regen `chat`, response-guard slim retries, streaming persona rethink) |

# Why this matters

The product thesis (CLAUDE.md, "Persona-First", "Personal Model", "Private
Learning") promises that **the model the user talks to learns from
correction and feedback over time, on the user's own hardware**.

What actually ships today is a reference research GPT that we can fine-tune
locally as a research artifact. The chat path goes to a frontier provider
(Anthropic / OpenAI / Vertex / etc.) which never sees the LoRA we trained.

This is not "privacy by architecture broken" — the conversation still flows
through the frontier provider regardless of LoRA — but it IS "personalization
broken." We cannot honestly say "the model learns who you are over time"
when the artifact we're learning into is not the artifact serving responses.

# Three concrete bridges, in order of pragmatism

## Bridge A — llama.cpp / GGUF inference + LoRA loader (target: 4-6 weeks)

The smallest honest step. Pull `llama.cpp` as a **conditionally-compiled**
provider backend.

- New provider: `src/providers/llamacpp.c` implementing `hu_provider_t`.
- Loads GGUF checkpoints (Llama 3 8B, Qwen 2.5 7B, Mistral 7B, etc.).
- Accepts `--lora-path` to apply a LoRA adapter at runtime.
- `lora-persona` gains a `--gguf-base <path>` mode that:
  1. Mounts the GGUF checkpoint as the base.
  2. Trains LoRA via `llama.cpp/finetune` (CPU first, Metal/CUDA optional).
  3. Saves a GGUF-compatible LoRA adapter.
- Chat path: when `provider=llamacpp`, system loads base + persona-LoRA
  adapter and serves all turns locally.

**Honest deliverable**: `human ml lora-persona --gguf-base llama-3-8b-q4.gguf
--persona seth` produces an adapter that genuinely changes the model the
user talks to (when `provider: llamacpp` is configured).

**Risk**: llama.cpp's API churns, large transitive code, build complexity.
Mitigated by `HU_ENABLE_LLAMACPP=OFF` default and a vendored sub-tree.

**Caveat**: still CPU-default. Apple Silicon Metal acceleration is opt-in
and adds another build dependency.

## Bridge B — MLX / Apple Silicon native LoRA (target: 6-10 weeks)

For the macOS/iOS story specifically. MLX has a clean LoRA fine-tune path
proven on 1B-7B models at usable speeds on M-series.

- New provider: `src/providers/mlx.c` (Apple Silicon only).
- Uses `mlx-lm` from MLX team for inference.
- `lora-persona --mlx-base <model-id>` shells out to `mlx_lm.lora` for
  training (initial integration).
- Phase 2: link `libmlx` directly for in-process training.

**Honest deliverable**: same as Bridge A but with native Metal speed and
no llama.cpp dependency on Apple platforms.

**Risk**: MLX is younger, ABI less stable, Apple-only. Less portable.

## Bridge C — Open-weight frontier inference + adapter (target: 3+ months)

The aspirational version: a unified provider that can serve any
open-weight frontier model via either backend, hot-swap LoRA adapters
per-conversation, and stream tokens with cancellation. This is what the
M3 narrative actually implies.

Punt this until Bridge A is green and we know which backend wins for our
users.

# Migration plan

## Phase 0 — Stop lying (DONE 2026-05-10, FIX 15)

- ✅ `--checkpoint` is no longer silently dropped. It loads a HUML
  reference-GPT base before LoRA attaches.
- ✅ The CLI prints a clear caveat that this is the reference GPT, not a
  frontier model.
- ✅ Help text points users to this doc.

## Phase 1 — Bridge A scaffold (target: +4 weeks)

- Add `HU_ENABLE_LLAMACPP` build flag (default OFF).
- Vendor `llama.cpp` minimal subset under `third_party/llama.cpp/` (or
  fetch via CMake `FetchContent`).
- Implement `src/providers/llamacpp.c` for inference only.
- Tests: roundtrip generation against a tiny GGUF (q4 7B → 1B for CI).
- Update CLAUDE.md M3 honestly: "Reference GPT works. llama.cpp inference
  scaffolded. LoRA application pending."

## Phase 2 — Real LoRA fine-tune via Bridge A (target: +4 more weeks)

- `lora-persona --gguf-base <path>` shells out to `llama.cpp/finetune` (or
  links it directly if the upstream API is stable).
- Save adapter in GGUF-LoRA format, not HUML.
- Tests: adapter measurably changes greedy output on a 5-example bank
  (assert tokens differ from base on a held-out prompt).
- Update CLAUDE.md M3: "LoRA fine-tunes a real frontier base. Adapter
  applies at inference."

## Phase 3 — Apple Silicon path via Bridge B (target: +6 weeks)

- Add `HU_ENABLE_MLX` build flag (default OFF, Apple Silicon only).
- Implement `src/providers/mlx.c`.
- `lora-persona --mlx-base <hf-id>` path.
- Bench against Bridge A on M2; pick a default for Apple Silicon.

## Phase 4 — Provider selection at chat time

### 4.0 Reference-GPT path (DONE 2026-05-10, FIX 18→20)

For the **reference HUML GPT** specifically, the chat-time merge is
landed today. The huml provider:

- Loads adapters via `hu_provider_load_adapter` (FIX 18).
- Validates adapter shape against `HUML_GPT_N_EMBD` / `HUML_GPT_N_LAYER`
  before attaching; mismatched adapters stay loaded but unattached and
  the operator sees a one-shot stderr caveat.
- Calls `hu_gpt_attach_lora(model, adapter, NULL, adapter, NULL, NULL,
  NULL)` (Q+V mirroring training-time targets) so every subsequent
  forward pass is biased.
- Detaches before destroying the adapter on unload / replace / deinit.

Coverage: `test_lora_disk_roundtrip_biases_gpt_forward` saves a LoRA to
disk, `hu_lora_load`s it, attaches to a GPT, asserts the logit sum is
measurably different from the base, then detaches and asserts the base
is restored within 1e-3.  This proves the disk → load → attach →
forward path that the chat path now exercises.

**What's still pending in Phase 4:** the same wiring for non-huml
providers (which requires Bridge A or B to land). Tracking that here:

### 4.1 Config-driven auto-load (DONE 2026-05-10)

Landed via FIX 24 (`feat(daemon): config-driven LoRA adapter auto-load`):

- Config schema gained a top-level `personalization` block:

  ```json
  {
    "personalization": {
      "enabled": true,
      "lora_adapter_path": "~/.human/personas/persona-default.lora",
      "lora_adapter_id": "persona-default",
      "m3_adapter_probe_path": "~/.human/ml/m3_probe.bin"
    }
  }
  ```

  `m3_adapter_probe_path` is optional: when set, the daemon runs
  `hu_m3_frontier_adapter_try_open` at startup (see `human/ml/m3_frontier_adapter.h`)
  and logs OK or failure — useful to validate the M3 seam before real GGUF/LoRA
  wiring lands.

- `hu_personalization_config_t` lives in `include/human/config_types.h`;
  parser at `src/config_parse.c::parse_personalization`; serializer at
  `src/config_serialize.c`; default state (off, NULL paths) is set in
  `src/config_merge.c`.
- Daemon (`src/daemon.c`, immediately after the W14 scheduler open)
  calls `hu_provider_load_adapter(&agent->provider, alloc, path,
  strlen(path), id, strlen(id))` when the block is enabled and a path
  is set.  `adapter_id` defaults to the file basename when blank.
- Provider returns of `HU_ERR_NOT_SUPPORTED` are logged at info level
  (cloud providers skip silently); other failures log a warning and
  fall through to the base model so a missing adapter never breaks
  startup.
- Coverage: `test_config_parses_personalization_block` and
  `test_config_personalization_disabled_by_default` in
  `tests/test_config_parse.c` exercise the schema; existing
  `test_huml_provider_load_unload_adapter` covers the load path the
  daemon dispatches into.

### 4.2 Frontier-provider chat-time merge (Bridge A scaffold landed; libllama vendor-in pending)

- The provider exists at `src/providers/llamacpp.c` with the full
  vtable wired (chat, load_adapter, unload_adapter, active_adapter,
  deinit). All hooks return `HU_ERR_NOT_SUPPORTED` until the
  CMake option `HU_ENABLE_LLAMACPP=ON` is set AND `third_party/llama.cpp/`
  is vendored (or libllama is reachable via the system include path).
- Factory dispatch (`src/providers/factory.c`): selecting `provider:
  "llamacpp"` in config now succeeds; the daemon will tolerate the
  NOT_SUPPORTED return from chat by falling back to whatever real
  provider is configured alongside.
- Six scaffold tests (`tests/test_llamacpp_provider.c`) lock down
  the contract: factory ownership, NULL-arg rejection, NOT_SUPPORTED
  fallthrough on chat / load_adapter / unload_adapter, NULL active
  adapter when nothing is loaded.
- Real implementation is gated behind `__has_include("llama.h")` so
  vendoring upstream is the only remaining step. When that lands the
  six placeholder paths flip to live calls (annotated inline in
  `llamacpp.c`).
- Same vtable triple, but the implementation maps onto the underlying
  framework's adapter API.

### 4.3 Chat-path stub hooks (`hu_agent_m3_on_provider_success`)

When `HU_ENABLE_ML` is on and `personalization.m3_adapter_probe_path` opens a
fixture adapter at bootstrap (`hu_agent_m3_adapter_attach`), the runtime
invokes `hu_m3_frontier_adapter_noop_infer` after each **successful** provider
LLM interaction listed below (stub today; tensor path replaces noop later).

| Site | File |
|------|------|
| Primary `chat` + on-device→cloud fallback `chat` | `src/agent/agent_turn.c` |
| Metacognition regeneration `chat` | `src/agent/agent_turn.c` |
| GVR `hu_gvr_pipeline` (verify/revise internals) | `src/agent/agent_turn.c`, `src/agent/agent_stream.c` |
| Constitutional `hu_constitutional_critique` | `src/agent/agent_turn.c`, `src/agent/agent_stream.c` |
| Response-guard `hu_response_guard_retry_slim` | `src/agent/agent_turn.c`, `src/agent/agent_stream.c` |
| `stream_chat` loop + persona rethink `chat_with_system` | `src/agent/agent_stream.c` |

# What an honest CLAUDE.md M3 row looks like (Phase 2)

> **M3 Private Learning** — On-device ML personalization
>
> **Status: partial.** Reference HUML GPT trains via LoRA (`lora-persona`).
> Frontier model bridge (`--gguf-base`) fine-tunes Llama/Qwen/Mistral via
> llama.cpp; adapter applies at chat time when `provider: llamacpp`. CPU
> default; Metal/MLX opt-in. **Gap:** auto-discovery of "this conversation
> would benefit from a new LoRA cycle"; multi-user adapter routing.
>
> Success metric: LoRA adapter measurably changes 80%+ of held-out
> persona-bank greedy outputs after a single training pass.

# Decision triggers to start Phase 1

Pick this up when ANY of these become true:

- A user files a bug or asks "where do my LoRA adapters get applied?"
  (signals expectation gap).
- We claim "personalization" in a launch / changelog / website copy.
- Bridge A's blocker (llama.cpp API stability) clears upstream.
- Engineering capacity for a 4-6 week project opens.

# Tracking

| Date | Phase | Notes |
|------|------|-------|
| 2026-05-10 | 0 | FIX 15: `--checkpoint` honest, caveat printed, plan doc landed |
| 2026-05-10 | 4.0 | FIX 20: huml chat-time LoRA merge — `hu_gpt_attach_lora` wired into provider load/unload; e2e disk-roundtrip test asserts forward-pass bias |
| 2026-05-11 | D0.3 | `hu_agent_m3_on_provider_success` wired across turn + stream paths (GVR, constitutional, metacog regen, guard retry, stream_chat, rethink) |
| 2026-05-11 | Bridge A | `llamacpp` vtable: non-NULL `chat` (delegates to `chat_with_system` like `huml_chat`) + `supports_streaming` → false so `hu_agent_turn_stream` never derefs NULL |
