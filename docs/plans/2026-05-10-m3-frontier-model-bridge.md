---
title: M3 Frontier Model Bridge — Honest Gap and Migration Plan
status: planned (multi-month)
owner: ML subsystem
created: 2026-05-10
parent: 2026-05-10-sota-roadmap-6mo.md
related:
  - CLAUDE.md (M3 row in Strategic Missions)
  - AGENTS.md
  - src/ml/CLAUDE.md
  - docs/ml-subsystem-audit-2026-03.md
  - 2026-05-10-sota-roadmap-6mo.md
  - adr/2026-05-11-adapter-rollback-signal.md
  - adr/2026-05-11-persona-eval-judge.md
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

## Bridge A.0 — Alpaca JSONL exporter (DONE 2026-05-11)

The on-ramp that closes the loop end-to-end **today**, without vendoring
llama.cpp in-tree. Users run:

```bash
human ml lora-persona --persona seth --export-jsonl seth.jsonl
```

and the persona's example banks are written to `seth.jsonl` in the
de-facto-standard Alpaca shape:

```json
{"instruction":"On telegram: casual catch-up","input":"hey how was your day","output":"long. need a walk."}
{"instruction":"On email: work","input":"can you review the doc?","output":"Sure -- by EOD."}
```

This is the format `llama.cpp/finetune`, `axolotl`, `unsloth`, and
`mlx-lm.lora` all consume directly. The user then runs their preferred
fine-tune toolchain externally and configures the resulting GGUF LoRA
back into the daemon via `personalization.lora_adapter_path` — which
already loads cleanly through the `llamacpp` provider (see Phase 4.1
above).

What this unlocks today:
- **No new third-party dependency in-tree.** llama.cpp can stay
  optional (`HU_ENABLE_LLAMACPP=OFF` default).
- **Real personalization for users who care.** A user with the toolchain
  installed can produce a real GGUF LoRA from their persona bank and
  serve it locally through llama.cpp.
- **Honest narrative.** "Train your own LoRA from your persona bank" is
  now a true statement of the runtime, not just a roadmap item.

What it doesn't unlock:
- **One-command train.** Users still run the finetune toolchain
  themselves; the daemon does not orchestrate llama.cpp/finetune.
  Bridge A (Phase 1 → 2 below) is the slice that closes that gap.
- **Cloud providers.** Adapters only apply to local inference. Cloud
  providers (Anthropic, Gemini, OpenAI) silently skip
  `personalization.lora_adapter_path` with NOT_SUPPORTED, as designed.

Coverage:
- `hu_persona_bank_export_jsonl` in `src/persona/examples.c` (declared
  in `include/human/persona.h`).
- `--export-jsonl <path>` flag in `human ml lora-persona`, short-circuits
  before any training-loop setup. No HUML checkpoint required.
- Tests: `test_persona_bank_export_jsonl_writes_alpaca_shape`,
  `test_persona_bank_export_jsonl_null_args_rejected`,
  `test_persona_bank_export_jsonl_escapes_special_chars`.

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
proven on 1B-7B models at usable speeds on M-series. The SOTA roadmap
(see [`2026-05-10-sota-roadmap-6mo.md`](2026-05-10-sota-roadmap-6mo.md)
Phase A2 / A3) elevates Bridge B from "Apple-only nice-to-have" to **the
primary personalization path for the M-series target market** — because
this is where the chat model the user actually talks to runs, and where
spec-decode-via-aligned-draft (Phase A3) wins decode TPS.

### Phase B.1 — Inference scaffold (target: 2 weeks)

- New provider: `src/providers/mlx.c` (Apple Silicon only, gated on
  `HU_ENABLE_MLX=ON`).
- Inference backend: invoke the running `mlx-server.sh` HTTP front
  (re-uses today's optimized server with the `RotatingKVCache.to_quantized`
  and `speculative_generate_step` patches plus the `StreamThoughtFilter`
  fix). The C provider is a thin HTTP client to `127.0.0.1:8741`.
- Health probe: provider startup pings `/health`, returns
  `HU_ERR_NOT_SUPPORTED` if the MLX server isn't reachable (graceful
  fallback to cloud providers, no daemon crash).
- Tests: `tests/test_mlx_provider.c` covering factory ownership, NULL-arg
  rejection, health-probe failure, basic chat round-trip against a
  fixture server. Pattern mirrors `test_llamacpp_provider.c`.

**Exit:** `provider: "mlx"` in config makes the chat path hit local MLX;
NOT_SUPPORTED fall-through stays bulletproof.

### Phase B.2 — LoRA training via `mlx_lm.lora` (target: 2 weeks)

- `human ml lora-persona --mlx-base <hf-id-or-path> --persona <id>`
  - Generates an Alpaca JSONL via the existing exporter (Bridge A.0 work).
  - Invokes `mlx_lm.lora` as a subprocess (initial integration) with a
    deterministic command line derived from training config.
  - Captures the resulting adapter under `~/.human/adapters/<persona-id>/`.
  - Saves training metadata (`<adapter>.metadata.json`) per the
    adapter-rollback ADR ([`adr/2026-05-11-adapter-rollback-signal.md`](adr/2026-05-11-adapter-rollback-signal.md)).
- Quality preservation eval: after each training run, the harness runs
  MT-Bench-short and the persona-eval anchor. Refuses to promote the
  adapter if either regresses beyond the SOTA-roadmap N7 gate
  (≤ 1% drop vs base).

**Exit:** one command produces a measurable persona-aligned adapter on
Gemma-4-31B (or any MLX-compatible base) and stamps it with eval scores.
This is the Phase A2 deliverable in the SOTA roadmap.

### Phase B.3 — Persona-aligned draft adapter for spec decode (target: 2 weeks; SHARED WITH SOTA roadmap A3 / B2)

- Same training pipeline as B.2 but targeting a smaller MLX base
  (Gemma-4 E4B or E2B). Aligns the draft's persona+style with the
  fine-tuned 31B target.
- Acceptance-rate eval: post-training run measures speculative-decode
  acceptance against the persona conversation set. Phase A3 gate
  requires ≥ 50% acceptance (vs ~0% with unaligned drafts demonstrated
  in the 2026-05-10 bench session).
- Auto-wires into `mlx-server.sh` as the `speculative_draft` model when
  acceptance gate passes.

**Exit:** decode TPS lifts ≥ 1.5× on `stream_long_reply` in the
nightly bench versus the no-draft baseline, with no persona-eval
regression. Hits the SOTA roadmap N5 metric (≥ 35 tok/s analytical tier).

### Phase B.4 — In-process MLX (stretch, target: +4 weeks)

- Link `libmlx` directly into the daemon (`HU_ENABLE_MLX_INPROC=ON`,
  Apple Silicon only). Eliminates the subprocess + HTTP hop, shaves
  Python overhead, brings TTFT closer to the SOTA roadmap N6 target
  (≤ 200 ms cached).
- Risk: MLX C++ ABI churn; mitigated by the same fall-through pattern as
  Bridge A — in-proc path is opt-in, the HTTP-front path stays as the
  default until the in-proc path proves stable over a release cycle.

**Exit:** TTFT median for cached prompts drops below 250 ms in the
nightly bench (best-effort gate; full N6 ≤ 200 ms may need the
prompt-cache extension from SOTA roadmap Phase B3.1).

### Phase B.5 — Continuous-learning integration (target: +2 weeks; matches SOTA roadmap A5)

- Wires the Phase B.2 training pipeline into the nightly retraining
  cron defined in SOTA roadmap Phase A5.1.
- A/B harness (A5.2) consumes adapter metadata from B.2.
- Auto-rollback signal hooks (A5.4) follow the adapter-rollback ADR.

**Risk**: MLX is younger, ABI less stable, Apple-only. Less portable.
Mitigated by keeping Bridge A (llama.cpp) as the cross-platform path
and treating Bridge B as the Apple-Silicon-first delivery vector for
M3.

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
  CMake option `HU_ENABLE_LLAMACPP=ON` is set AND libllama is
  reachable. Resolution order in `CMakeLists.txt` (W13 Bridge A block):
  1. **vendored** — `third_party/llama.cpp/CMakeLists.txt`,
  2. **`find_package(Llama)`** — upstream's CMake export (~b3000+),
  3. **`pkg_check_modules(LLAMA llama)`** — Linux distro `llama.pc`,
  4. **manual prefix probe** — Homebrew (Apple Silicon + Intel), `/usr/local`, `/usr`.
  When none match, the build keeps the `HU_ERR_NOT_SUPPORTED` stub.
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
| 2026-05-11 | Bridge A | CMake: system libllama resolution — `find_package(Llama)` → pkg-config → Homebrew/Linux prefix probe; vendored `third_party/llama.cpp/` still preferred. Test build mirrors include-only (link rides on `human_core` PUBLIC). Coverage: `test_llamacpp_chat_rejects_null_args`. |
| 2026-05-11 | Bridge A | `llamacpp.c` ported to **modern llama.cpp API (b3000+)**: `llama_model_load_from_file` / `llama_init_from_model` / `llama_model_free` (the `_load_model_from_file` / `_new_context_with_model` / `_free_model` spellings are `-Werror=deprecated` traps under recent libllama). Adapter API: `llama_adapter_lora_init` + `llama_set_adapters_lora(ctx, &a, 1, &scale)` (the modern API has no per-adapter remove; clear by setting an empty array). Linked path now compiles cleanly against Homebrew `llama.cpp@b6981+`. |
| 2026-05-11 | Bridge A | CI: new `feature-flags` matrix entry `llamacpp-on` builds with `-DHU_ENABLE_SQLITE=ON -DHU_ENABLE_ALL_CHANNELS=ON -DHU_ENABLE_ML=ON -DHU_ENABLE_LLAMACPP=ON`. Exercises both the libllama discovery chain and the linked vtable path on every PR. Local repro: `cmake -S . -B build-llamacpp-ci <flags>` → 9740/9740 passing. |
| 2026-05-11 | Bridge A | **Matrix entry shrunk to minimal Bridge-A reproducer:** `-DHU_ENABLE_SQLITE=ON -DHU_ENABLE_ALL_CHANNELS=ON -DHU_ENABLE_LLAMACPP=ON` (no more piggybacking on `HU_ENABLE_ML`). Required gating the unconditional learner call sites in `src/bootstrap.c` (W13 open/close), `src/daemon.c` (W14 wiring + scheduler-tick outcome drain + LoRA auto-enqueue), and `src/agent/world_model_bridge.c` (`hu_w14_scheduler_register_lora_runner` / `_register_training_data_runner` now return `HU_ERR_NOT_SUPPORTED` when learning is off — public symbols stay defined so daemon callers don't need their own `#ifdef`). The W14 runner sources `src/agent/lora_training_runner.c` + `src/agent/training_data_runner.c` moved to the `if(HU_ENABLE_LEARNING)` CMake block (with matching `HU_TEST_EXTRA_MODULES` carve-out for the test binary). 9790/9790 passing on the slim build, 9847/9847 on dev. |
| 2026-05-11 | Bridge A | `hu_provider_load_adapter` dispatcher safety pinned: regression test pack in `tests/test_provider_all.c` proves NOT_SUPPORTED return on every cloud provider (openai, anthropic, gemini, ollama, openrouter), invalid-args rejection without provider deref, and NULL-return for `hu_provider_active_adapter`. Guards against a future helpers.c refactor accidentally turning a clean NOT_SUPPORTED into a NULL deref when the W13 learner auto-load wires a cloud provider. 8 tests added. |
| 2026-05-11 | Bridge A | Daemon-pattern fall-through pinned: `test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat` walks the daemon's exact startup sequence (`hu_provider_create("openai") → hu_provider_load_adapter(.., NOT_SUPPORTED) → chat_with_system continues serving the base model`). The user-visible promise — "configuring personalization on a cloud provider does not break startup, the daemon logs and falls back" — is now an explicit regression guard. Pairs with the existing dispatcher safety pack: the dispatcher tests prove the call cannot crash; this test proves the *whole turn* survives the NOT_SUPPORTED return. |

# Latent issues surfaced by the `llamacpp-on` matrix entry

Adding the matrix entry uncovered two pre-existing build-config bugs that
were hidden because every prior matrix entry implicitly turned `HU_ENABLE_ML`
on (which forces `HU_ENABLE_LEARNING=ON` per `CMakeLists.txt:38-42`).

1. ~~**`src/bootstrap.c` references `hu_learner_open_default` /
   `hu_learner_close` unconditionally**~~ — **fixed 2026-05-11.** Call
   sites in `bootstrap.c`, `daemon.c`, and the public W14 registration
   shims in `world_model_bridge.c` are now gated on `HU_ENABLE_LEARNING`;
   the W14 runner sources (`lora_training_runner.c`,
   `training_data_runner.c`) live under the `if(HU_ENABLE_LEARNING)`
   block in `CMakeLists.txt` with a matching `HU_TEST_EXTRA_MODULES`
   carve-out for the test binary. Public registration shims keep their
   symbol but return `HU_ERR_NOT_SUPPORTED` when learning is off, so
   daemon callers don't need their own `#ifdef`. The matrix entry now
   builds cleanly with just `HU_ENABLE_LLAMACPP=ON` (no ML/LEARNING
   piggyback).
2. **Transient `snprintf` undeclared error in `src/agent/world_model.c`** —
   `<stdio.h>` is included at line 24 but a parallel build occasionally
   surfaces an "implicit-function-declaration" error. Likely a header
   ordering / ccache interaction; rebuild reliably succeeds. Track for a
   deeper investigation if it recurs in CI.

Two new pre-existing latent bugs surfaced by the slim build that were
fixed under the same triage (one-line each, surgically scoped):

3. **`src/agent/autodream.c::phase_hyperedge_consolidation`** had an
   unused `alloc` parameter that the `-Wunused-parameter -Werror`
   gate was rejecting on dev. `(void)alloc;` added with a comment
   explaining the parameter is reserved for future hyperedge synthesis
   work that needs allocator access for summary materialisation.
4. **`tests/test_personal_model.c::personal_model_style_directive_absent_when_no_samples`**
   referenced `fx.count` after the field was renamed to `fact_count`
   in `hu_fact_extract_result_t`. Field name updated; test runs and
   passes. ccache had been masking the regression on dev.
