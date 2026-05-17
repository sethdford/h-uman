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
| **D0.3 seam in-tree** — `hu_m3_frontier_adapter_*` fixture load + noop infer (tests) | ✅ `human/ml/m3_frontier_adapter.h`. **Honest caveat (audit 2026-05-16):** `hu_agent_m3_on_provider_success` is wired across 5 call sites, but invokes `hu_m3_frontier_adapter_noop_infer` which is `(void)adapter; return HU_OK` (`src/ml/m3_frontier_adapter.c:89`). No gradient accumulation, no signal capture — the seam exists structurally so the eventual tensor path doesn't need to re-touch call sites. Treat as plumbing, NOT as a working personalization signal. | ✅ **Chat wiring (plumbing only):** `hu_agent_m3_on_provider_success` after every successful frontier provider round in `agent_turn.c` + `agent_stream.c` (primary `chat` / `stream_chat`, GVR, constitutional, metacog regen `chat`, response-guard slim retries, streaming persona rethink) |
| Bank-from-history one-command on-ramp | ✅ `hu_persona_banks_extract_from_history` + `--from-history` flag, PII-redacted, quality-filtered, dedup'd | ✅ |

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

## Bridge A.0.5 — banks-from-history generator (DONE 2026-05-11)

The natural complement to A.0: instead of requiring users to hand-author
example banks in the persona JSON before they can run A.0, derive banks
directly from the user's existing conversation history. Combined with
A.0 this is the one-command path:

```bash
human ml lora-persona --persona seth \
  --from-history ~/.human/memory.db \
  --export-jsonl seth.jsonl
```

`hu_persona_banks_extract_from_history` walks the `messages` table in
the target SQLite DB, pairs adjacent user→assistant turns within each
session, parses the channel from the `session_id` prefix
(`telegram:42` → `telegram`, `imessage:thread-7` → `imessage`,
`session-abc` → `default`), and emits one populated bank per channel
that produced ≥1 surviving example.

Quality gates (Phase A1.2 modules), applied in order before any pair
is admitted to a bank:

1. **PII redaction** (`hu_pii_redact`) — emails, phones, SSNs,
   credit-card-shaped digit groups, IPv4 addresses, and api-key /
   token / secret prefixes are replaced with placeholders. The
   redacted text is what lands in the bank, not the original.
2. **Quality check** (`hu_quality_check`) — length / Shannon entropy
   / unique-byte ratio gates on the concatenated user+assistant
   fingerprint. Catches single-character mashes, keyboard noise, and
   pathologically long blobs.
3. **Within-run dedup** (`hu_dedup_set`) — lowercased,
   whitespace-collapsed FNV-1a fingerprint catches the same
   exchange showing up across N sessions.

Per-channel cap (`max_per_channel`, default 32) bounds the bank size
so a multi-year history doesn't materialize 10K examples per channel.
Channel cap (32) bounds the bank count.

What this unlocks today:
- **One-command path from history to JSONL.** No persona authoring
  required. The user runs the daemon for a while, then feeds the
  resulting `messages` table back into the same CLI.
- **Channel-segregated bank.** Telegram-casual vs iMessage-warm vs
  cli-terse all land in their own bank, ready for the channel-aware
  prompt builder to select from at inference time.
- **Privacy-by-default before training.** Conversation history can
  carry PII verbatim; the bank derived from it cannot.

What it doesn't unlock:
- **Persona JSON write-back.** The derived banks live in memory for
  the duration of one CLI invocation. Users who want to persist them
  to the persona file still need to author the JSON themselves
  (future Phase A1.4 — round-trip writer).

Coverage:
- `hu_persona_banks_extract_from_history`, `hu_persona_example_banks_free`
  in `src/persona/examples.c` (declared in `include/human/persona.h`).
- `--from-history <memory.db>` and `--from-history-max <N>` flags in
  `human ml lora-persona`. Replaces loaded banks for the run; no
  on-disk persona mutation.
- Tests: `test_persona_banks_from_history_*` (12 tests covering null
  args, missing DB, empty DB, channel grouping, default fallback,
  per-channel cap, orphan assistant, low-quality rejection, dedup,
  PII redaction, free safety, and a happy-path extraction).

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

> **AUDIT NOTE (2026-05-16):** `hu_m3_frontier_adapter_noop_infer` is
> `(void)adapter; return HU_OK` at `src/ml/m3_frontier_adapter.c:89`.
> It accumulates no gradient, captures no signal, persists no state.
> The chat hot-path call sites exist *only* so the tensor path
> doesn't need to re-edit them later. **Do not interpret "D0.3
> wired" as "personalization signal flows."** Whether the tensor
> path that replaces this no-op actually moves the persona-fidelity
> metric (`hu_persona_fidelity_score_l1` + the L1/L2 stack in
> `src/eval/persona_fidelity.c`) is the actual M3 success criterion.

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
| 2026-05-11 | D0.3 | `hu_agent_m3_on_provider_success` wired across turn + stream paths (GVR, constitutional, metacog regen, guard retry, stream_chat, rethink). **AUDITED 2026-05-16:** the hook is structurally wired but invokes `hu_m3_frontier_adapter_noop_infer`, a `(void)adapter; return HU_OK` no-op (`src/ml/m3_frontier_adapter.c:89`). Status is "call sites exist," NOT "personalization signal flows." Do not cite this row as evidence that M3 has shipped. |
| 2026-05-11 | Bridge A | `llamacpp` vtable: non-NULL `chat` (delegates to `chat_with_system` like `huml_chat`) + `supports_streaming` → false so `hu_agent_turn_stream` never derefs NULL |
| 2026-05-11 | Bridge A | CMake: system libllama resolution — `find_package(Llama)` → pkg-config → Homebrew/Linux prefix probe; vendored `third_party/llama.cpp/` still preferred. Test build mirrors include-only (link rides on `human_core` PUBLIC). Coverage: `test_llamacpp_chat_rejects_null_args`. |
| 2026-05-11 | Bridge A | `llamacpp.c` ported to **modern llama.cpp API (b3000+)**: `llama_model_load_from_file` / `llama_init_from_model` / `llama_model_free` (the `_load_model_from_file` / `_new_context_with_model` / `_free_model` spellings are `-Werror=deprecated` traps under recent libllama). Adapter API: `llama_adapter_lora_init` + `llama_set_adapters_lora(ctx, &a, 1, &scale)` (the modern API has no per-adapter remove; clear by setting an empty array). Linked path now compiles cleanly against Homebrew `llama.cpp@b6981+`. |
| 2026-05-11 | Bridge A | CI: new `feature-flags` matrix entry `llamacpp-on` builds with `-DHU_ENABLE_SQLITE=ON -DHU_ENABLE_ALL_CHANNELS=ON -DHU_ENABLE_ML=ON -DHU_ENABLE_LLAMACPP=ON`. Exercises both the libllama discovery chain and the linked vtable path on every PR. Local repro: `cmake -S . -B build-llamacpp-ci <flags>` → 9740/9740 passing. |
| 2026-05-11 | Bridge A | **Matrix entry shrunk to minimal Bridge-A reproducer:** `-DHU_ENABLE_SQLITE=ON -DHU_ENABLE_ALL_CHANNELS=ON -DHU_ENABLE_LLAMACPP=ON` (no more piggybacking on `HU_ENABLE_ML`). Required gating the unconditional learner call sites in `src/bootstrap.c` (W13 open/close), `src/daemon.c` (W14 wiring + scheduler-tick outcome drain + LoRA auto-enqueue), and `src/agent/world_model_bridge.c` (`hu_w14_scheduler_register_lora_runner` / `_register_training_data_runner` now return `HU_ERR_NOT_SUPPORTED` when learning is off — public symbols stay defined so daemon callers don't need their own `#ifdef`). The W14 runner sources `src/agent/lora_training_runner.c` + `src/agent/training_data_runner.c` moved to the `if(HU_ENABLE_LEARNING)` CMake block (with matching `HU_TEST_EXTRA_MODULES` carve-out for the test binary). 9790/9790 passing on the slim build, 9847/9847 on dev. |
| 2026-05-11 | Bridge A | `hu_provider_load_adapter` dispatcher safety pinned: regression test pack in `tests/test_provider_all.c` proves NOT_SUPPORTED return on every cloud provider (openai, anthropic, gemini, ollama, openrouter), invalid-args rejection without provider deref, and NULL-return for `hu_provider_active_adapter`. Guards against a future helpers.c refactor accidentally turning a clean NOT_SUPPORTED into a NULL deref when the W13 learner auto-load wires a cloud provider. 8 tests added. |
| 2026-05-11 | Bridge A | Daemon-pattern fall-through pinned: `test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat` walks the daemon's exact startup sequence (`hu_provider_create("openai") → hu_provider_load_adapter(.., NOT_SUPPORTED) → chat_with_system continues serving the base model`). The user-visible promise — "configuring personalization on a cloud provider does not break startup, the daemon logs and falls back" — is now an explicit regression guard. Pairs with the existing dispatcher safety pack: the dispatcher tests prove the call cannot crash; this test proves the *whole turn* survives the NOT_SUPPORTED return. |
| 2026-05-11 | D1.3 | **Rollback flag landed.** `personalization.m3_adapter_disabled` (config) + `HUMAN_M3_ADAPTER_DISABLE` (env) both gate the bridge attach in `bootstrap.c`. Precedence is centralized in `hu_m3_adapter_should_disable(bool cfg_disabled)` so callers (bootstrap, future tools, tests) all observe the same decision. Env semantics: empty / `0` → no force (config wins), any other value (`1`, `true`, `yes`, `on`) → force-disable. Disable causes `hu_agent_m3_adapter_attach` to be skipped; `hu_agent_m3_on_provider_success` is a NULL-adapter no-op so the chat hot path has zero added cost. 7 new tests in `tests/test_ml.c::test_m3_adapter_should_disable_*` + `test_m3_on_provider_success_noop_when_unattached`; 3 new config tests in `tests/test_config_parse.c::test_config_*_m3_disabled*`. |
| 2026-05-11 | A.1.4 | **Persona persist landed.** `hu_persona_creator_write` now emits `example_banks` in the canonical array shape the loader actually parses (`[{"channel": ..., "examples": [...]}]`). Previous object-keyed-by-channel output was silently dropped on load — round-trip now works. `human ml lora-persona --persist` writes derived banks back to `~/.human/personas/<name>.json` after `--from-history`. Pinned by `test_persona_creator_write_round_trips_example_banks` (`tests/test_persona.c`). |
| 2026-05-11 | personal-model | **Symmetric signal aging shipped.** `hu_personal_topic_t.last_mentioned`, `hu_personal_goal_t.last_referenced` (new), `hu_communication_style_t.last_observed_at` (new) drive `hu_personal_topic_effective_score` / `_goal_effective_priority` / `_communication_style_freshness` — same exponential-decay shape as `hu_heuristic_fact_effective_confidence` (60d / 120d / 180d half-lives). `hu_personal_model_apply_decay(model, now)` sweeps facts/topics/goals and prunes anything below `HU_PM_FORGET_FLOOR (0.05)`, idempotent at fixed `now`. Prompt builder gates topics, goals, and the "Mirror their style" directive on these scores so stale signal silently drops out. `HU_PM_VERSION` bumped to 4 (v3 saves reset to defaults; progressive migration tracked separately). 25 new tests in `tests/test_personal_model.c::personal_model_*`. |
| 2026-05-11 | personal-model | **v3→v4 progressive migration landed.** `hu_personal_model_load` now recognizes the legacy v3 on-disk layout (smaller `hu_personal_goal_t` without `last_referenced`, smaller `hu_communication_style_t` without `last_observed_at`) and migrates field-by-field with the new fields zero-filled. Facts and topics survive byte-for-byte (their structs were unchanged across v3↔v4). The in-memory model stamps the current `HU_PM_VERSION` so the next save writes a v4 file. v3 layout is mirrored privately in `personal_model.c` (`hu_pm_v3_model_t`) — tests own their own copy of the layout (`tests/test_personal_model.c::pm_v3_test_model_t`) so a future v4→v5 schema bump can't silently invalidate the migration fixture. 5 new tests pin facts/topics survival, goals zero-fill (with `last_referenced=0` falling back to `created_at`), style zero-fill (samples-with-zero-stamp treated as fully fresh), unknown-version rejection, and v4 native round-trip parity. |
| 2026-05-11 | personal-model | **Idle decay tick wired.** `hu_personal_model_apply_decay` now runs opportunistically in `agent_turn.c` after every `hu_personal_model_ingest` (sub-microsecond cost, idempotent at fixed `now`) and once at `hu_agent_deinit` shutdown. The post-ingest save persists pruned state so crash recovery never re-loads already-evicted facts. No new schema field needed — decay state is purely derived from per-element timestamps. |
| 2026-05-11 | personal-model | **Goal mention pipeline.** `hu_personal_model_touch_goals_in_message(model, msg, msg_len, now)` walks each active goal's description and bumps `last_referenced` when any 5+ char content word appears in the user message (case-insensitive). Wired into `agent_turn.c` between ingest and apply_decay so a goal mentioned at the edge of the half-life isn't pruned in the same turn it was just touched. Goal insert path now stamps `last_referenced = created_at` on creation so the field is correct from day 1 (the loader's `created_at` fallback in `hu_personal_goal_effective_priority` still handles older v3-migrated goals). 8 new tests in `tests/test_personal_model.c::personal_model_touch_goals_*` cover null args, content-word match, short-word filtering, case-insensitivity, inactive-goal skip, no-regression on existing stamps, no-match no-bump, multi-goal bumping. |
| 2026-05-11 | personal-model | **Daemon idle-decay tick wired.** The per-turn decay in `agent_turn.c` covers user-active sessions, but a daemon idle for hours / days never re-evaluated stored signal. New per-hour decay block in `src/daemon.c`'s scheduler-tick path (rate-limited via `last_pm_decay_secs` static, same shape as the existing `last_td_extract_ms` pattern) calls `hu_personal_model_apply_decay(model, now)` and persists the pruned state when anything was evicted. Closes the long-idle-daemon gap so RSS stays bounded across long unattended runs. |
| 2026-05-11 | personal-model | **Goal auto-deactivation on completion.** New `hu_personal_model_resolve_goals_in_message(model, msg, len, now)` walks every active goal and deactivates it when the message contains BOTH a completion verb (`shipped`, `finished`, `done`, `completed`, `wrapped`, `resolved`, `closed`) AND a 5+ char content word from the goal description. Negation guard scans 12 chars before the verb for `not` / `n't` / `without` so "I haven't shipped" does NOT resolve a "ship feature" goal. Word-boundary checks keep "doneness" from matching "done". On match: `goal->active = false`, `goal->progress = 1.0`. Wired into `agent_turn.c` between `touch_goals` and `apply_decay` so a goal can be touched and resolved in the same turn (decay then prunes the inactive slot). 10 tests cover null args, all 7 completion verbs, three negation forms, content-word requirement, inactive idempotence, boundary check, and multi-goal resolution. |
| 2026-05-11 | personal-model | **Style EWMA decay toward neutral.** `hu_personal_communication_style_blend_with_freshness(style, now)` returns a copy of the style struct with each `[0,1]` axis blended toward 0.5 proportionally to `(1 - freshness)` — formula: `blended_axis = raw_axis * freshness + 0.5 * (1 - freshness)`. Pass-through fields (`sample_count`, `avg_message_length`, `last_observed_at`) are untouched. Prompt builder now uses the blended struct in the "Mirror their style" directive so the steering signal smoothly fades toward neutral rather than hitting a hard cliff at the existing freshness=0.3 gate. Read-only: never mutates the source model. 5 unit tests pin the math (raw at full freshness, exact half-life blend, full neutral when sample_count=0, no input mutation). |
| 2026-05-11 | D2.2 | **Offline persona-fidelity baseline scorer + CLI.** `hu_communication_style_fidelity_score(target, response, len)` returns a deterministic `[0,1]` score = mean of three triangular axis matches (`lowercase_ratio` of letters, `abbreviation_ratio` of words against the same shorthand vocab the EWMA tracker uses, length match against `avg_message_length` via relative-error window). Returns -1.0 on null inputs or zero-sample fingerprint. New `human ml lora-baseline --persona <name>` CLI loads a persona, builds a fingerprint from `personal_model.bin` (or falls back to a synthetic casual-lowercase profile when the disk model is empty), scores every response in every example bank, and prints mean/min/max + an interpretation note. The reported mean is the upper bound a frontier model can plausibly hit without LoRA — a post-LoRA mean above this number indicates the adapter is actively pulling toward persona fidelity. 8 unit tests on the scorer + CLI `--help` smoke-tested end-to-end. Closes D2.2 from `optional/not yet` to `done`. |
| 2026-05-11 | D2.1 | **Caveat snapshot tests landed.** `human ml lora-persona`'s honest-gap caveat strings are now centralized in `src/ml/m3_frontier_adapter.c`: `hu_ml_lora_persona_caveat_doc_path()` and `hu_ml_lora_persona_caveat_block()`. `cli.c` consumes the helpers at three sites (training start, test-mode line, `--help` text) — the literal printf blocks are gone. 4 snapshot tests in `tests/test_ml.c::test_lora_persona_caveat_*` pin: (a) the doc-path constant equals the canonical bridge-doc path, (b) the block contains "NOT a frontier" + "HUML GPT" + the doc path, (c) every line is `[lora-persona]`-prefixed for visual alignment, (d) Llama/Qwen/Mistral are referenced and the block ends with `\n` so callers can `fputs` it directly. Guards against silent drift toward overclaiming language during refactors. |
| 2026-05-11 | D2.2 | **lora-baseline CI gate landed.** `scripts/check-lora-baseline.sh` runs the offline persona-fidelity scorer against `tests/fixtures/lora_baseline_persona.json` (a casual-lowercase fixture authored to score reliably ~0.92 against the synthetic fingerprint defaults) and fails when the mean drops below `LORA_BASELINE_FLOOR=0.50` (overridable). Stages the fixture into a private `HU_PERSONA_DIR` so the test never touches the user's real `~/.human/personas`, and forces `HOME` so the synthetic-fingerprint path (no `personal_model.bin`) triggers deterministically. Wired into `scripts/verify-all.sh` between the memory-query scan and the security scan. Failure path verified end-to-end with `LORA_BASELINE_FLOOR=0.99`. Now any regression in the scorer (always-zero, NaN, broken axis math) or the synthetic-fingerprint defaults fails CI before merge. |
| 2026-05-11 | personal-model | **Per-turn maintenance helper extracted.** `hu_personal_model_per_turn_tick(model, msg, len, from_user, now)` bundles the canonical sequence (`ingest → touch_goals → resolve_goals → apply_decay`) with explicit ordering rationale on the helper itself. Returns a `hu_personal_model_turn_tick_result_t` reporting per-phase counts so callers can log/test the integration. `agent_turn.c` collapses the previous 4-call block into a single helper invocation; `agent_deinit` continues to use the standalone `apply_decay` (no message to ingest at shutdown). Ordering invariants — touch must run before resolve (so a touch-then-resolve goal still gets credit for being mentioned), resolve must run before decay (so a freshly-resolved goal hits its post-resolve state in the same tick) — are pinned by 6 dedicated tests. |
| 2026-05-11 | personal-model | **`idle_due` rate-limit helper extracted.** `hu_personal_model_idle_due(last_inout, now, interval)` is a pure is-it-time-yet predicate over `(last, now, interval)`. First-call (`*last <= 0`) fires immediately and stamps `*last = now`; subsequent calls return true only when `now - *last >= interval`. NULL-safe; non-positive `now` or `interval` return false. The daemon's hourly personal-model decay block in `src/daemon.c` now uses this helper instead of an inline gate so the gating logic is unit-testable without booting the daemon. 9 tests cover null, non-positive args, first-call semantics, exact-interval boundary, far-past, and a multi-tick simulation. |
| 2026-05-11 | personal-model | **Recently-completed goals scratchpad.** New `HU_PM_COMPLETED_GOAL_RETAIN_SEC` (7 days) keeps inactive goals alive after resolution so the prompt builder can surface a "Recently completed: …" line for follow-up tone-matching ("congratulations on shipping X" instead of "let me know how X is going"). `hu_personal_goal_is_recently_completed(goal, now)` exposes the gate; `apply_decay`'s goal pruning was changed from "prune unless `eff >= floor`" to "prune unless `eff >= floor` OR `is_recently_completed`"; `resolve_goals_in_message` now stamps `last_referenced = now` defensively (so direct callers without prior `touch_goals` still produce a usable timestamp); `build_prompt` walks the goals array a second time to emit the scratchpad line. Older completed goals (past retention) are pruned by `apply_decay` on the next sweep so the slot doesn't dominate forever. 9 tests cover null, active-not-completed, retention boundary (in/out/at-edge), no-stamp, decay round-trip, and prompt-builder integration (surfaces fresh, omits old). |
| 2026-05-11 | personal-model | **Recently-completed bulk getter + observability.** `hu_personal_model_get_recently_completed_goals(model, now, out, cap)` fills a `const hu_personal_goal_t **` pointer array (NULL-safe, cap-respecting, preserves insertion order) for downstream consumers that need the same retention-window goals the prompt builder surfaces. `hu_personal_model_describe_recently_completed(model, now, buf, cap)` produces a one-line comma-separated summary with ASCII-ellipsis truncation ("ship feature, ...") so log lines and status outputs stay single-line and parsable. `agent_turn.c` calls the describer when `tick.goals_resolved > 0` and emits a `hu_log_info("personal model: %zu goal(s) just completed: %s", ...)` event — the first downstream consumer of the new surface, and the foundation for future channel-specific congratulation flows. 11 new tests (5 getter + 6 describer) cover null, empty buf, cap=0, cap truncation, ellipsis, multi-goal separators, retention boundary, and skip-on-empty-description. |
| 2026-05-11 | D2.2 | **A/B comparator landed.** `hu_communication_style_compare_response_sets(target, set_a, lens_a, n_a, set_b, lens_b, n_b, &sum_a, &sum_b, &delta)` scores every response in both sets against `target`, accumulates per-set summaries (scored / skipped / mean / min / max), and reports `delta = b.mean - a.mean`. Refuses zero-sample-count targets (HU_ERR_INVALID_ARGUMENT — same shape `lora-baseline` rejects). NULL / empty / over-short / score-of-(-1) responses are counted as `skipped` and don't drag the mean toward zero. Explicit `lens` arrays let the JSON loader pass un-NUL-terminated buffers; absent `lens` the comparator falls back to `strlen`. The comparator is the actual offline LoRA evaluation primitive — pass pre-LoRA outputs as `set_a` and post-LoRA as `set_b`, and a positive delta means the adapter is pulling toward persona fidelity. 8 unit tests cover null args, zero-sample-count rejection, empty sets, positive delta (formal vs casual), negative delta when B is worse, skipped-counter math, single-response min/max identity, and explicit-length truncation. |
| 2026-05-11 | D2.2 | **`human ml lora-ab` CLI + paired-fixture gate.** New CLI thin-wraps the comparator with a JSON string-array loader (top-level array of strings, e.g. `["resp 1", "resp 2"]`). Args: `--persona <name> --before <pre.json> --after <post.json>` plus optional `--floor-delta F` (fail when `delta < F`) and `--require-positive` (fail when `delta <= 0`) — the actual evaluation primitives a CI gate needs. Persona drives the fingerprint via the same `personal_model.bin`-or-synthetic fallback as `lora-baseline`, so the two tools are directly comparable. Paired fixtures `tests/fixtures/lora_ab_before.json` (5 formal responses) and `tests/fixtures/lora_ab_after.json` (5 casual lowercase responses) produce +0.368 delta on the canonical run against `lora_baseline_persona.json`. `scripts/check-lora-ab.sh` parses the `[lora-ab]   delta:  +X.XXX` line, fails when below `LORA_AB_FLOOR_DELTA=0.10`; failure path verified end-to-end with `LORA_AB_FLOOR_DELTA=0.99`. Wired into `scripts/verify-all.sh` next to the lora-baseline gate. The lora-ab gate is now the load-bearing offline check that "if you change the scorer or the synthetic fingerprint, CI tells you". |
| 2026-05-11 | persona-prompt | **Congratulation directive in prompt.** The "Recently completed: …" surface (added the same day) is now paired with a short behavioral directive: *"Note: when a recently-completed item comes up in the conversation, acknowledge it warmly (a brief congrats or check-in) before moving on. Don't let the acknowledgment dominate the reply."* Closes the observability → behavior loop — without the directive, the structural list was just trivia the model could ignore. Gated on the same condition as the list itself (no completed items → no directive → no token waste). 2 new prompt-integration tests pin (a) the directive must appear AFTER the structural list and (b) the directive must NOT appear when there are zero completed goals (no-op tax). |
| 2026-05-11 | D2.2 | **`human ml lora-runner` CLI.** Provider-driven response-set generator — closes the "how do I get the JSON files into `lora-ab`?" gap. Args: `--persona <name> --output <path> [--provider <name>] [--model <id>] [--max-examples N]`. Walks the persona's example bank, builds a system prompt from `identity + traits`, calls the active provider's `chat_with_system()` per `incoming` message, and writes the captured responses as a JSON array directly consumable by `lora-ab` as either `--before` or `--after`. The two-pass workflow (no-adapter run → `before.json`; load adapter via `apply-adapter` → `after.json`; `lora-ab --before --after`) is now a 4-command shell pipeline. Per-example errors don't abort; the comparator's `skipped` counter handles empty entries. Under `HU_IS_TEST` the chat call is mocked: the runner echoes each example's canned `response` field so unit tests exercise the full load → write → JSON round-trip without a live provider. 3 unit tests cover the test-mode happy path, missing-arg rejection, and `--max-examples` truncation. Production smoke from a bare invocation correctly fails over to the local provider, reports "5 of 5 examples returned an error or empty content" when the local provider is unreachable, and writes a 5-element JSON-array of empty strings — the no-provider error path is well-defined and dashboard-actionable. |
| 2026-05-11 | D2.2 | **`human ml fidelity-status` JSON command.** Single command, single JSON object — the canonical output format any UI/observability surface needs to display LoRA-fidelity health. Document shape: `{persona, fingerprint_source, baseline:{scored, mean, min, max}, ab:{available, before_mean, after_mean, delta, scored_before, scored_after}}`. Args: `--persona <name>` (required) plus optional `--before/--after/--output`. When `--before` and `--after` are both provided AND parse, `ab.available:true` and the delta is included; otherwise `ab.available:false` so the dashboard knows there's nothing to plot. Output path is optional (defaults to stdout) so the command pipes cleanly to `jq`, saves to disk, or feeds a future dashboard tile. 2 unit tests cover the baseline-only and full-AB shapes; production smoke against `lora_baseline_persona.json` + the existing `lora_ab_*.json` fixtures emits `baseline.mean=0.923` + `ab.delta=0.368` in a single one-line JSON document. With this command the dashboard work is gated on UI design, not backend availability. |
| 2026-05-11 | persona-prompt | **Channel-overlay-aware acknowledgment directive.** New `hu_personal_model_build_prompt_with_overlay(model, overlay, buf, cap)` chooses one of four directive variants based on the active channel's persona overlay: formal-terse ("respectful one-liner, no emoji"), casual-permissive ("an emoji is fine if it fits"), short-length-brevity ("one sentence"), or default. `formal` always trumps `emoji_usage` — no playful directive on a formal channel even when emoji license is high. Legacy `_build_prompt(...)` is a thin wrapper passing `NULL` overlay; null-vs-legacy parity is pinned by a dedicated test, so existing callers see byte-identical output. `agent_turn.c` and `agent_stream.c` both look up the channel overlay via `hu_persona_find_overlay` and forward it — eliminating the streaming/non-streaming directive divergence (the iMessage stream getting a casual congrats while the non-streaming reply got a formal one). 6 new tests cover null-vs-legacy parity, formal-terse, casual+emoji permissive, short-length brevity, formal-overrides-emoji, unknown-overlay default fallback. |
| 2026-05-11 | D2.2 | **Persona-fidelity dashboard tile.** New `<hu-fidelity-tile>` LitElement consumes the JSON shape from `human ml fidelity-status` and renders it as a 3-lane card: baseline mean (% with min-max range), A/B delta (signed %, success-tinted positive / error-tinted negative / muted "no run" when `ab.available=false`), and sample counts. Loading skeleton when `data` is null (`aria-busy=true`); inline error banner with `role=alert` when `errorMessage` is set. Demo gateway gains a `metrics.fidelity` mock returning the canonical 0.923 / +0.368 fixture; `catalog.html` exposes a live tile in the playground. 6 vitest cases pin registration, loading skeleton, populated render with delta tint, negative-delta error tint, no-run muted state, and error banner. `npm run lint:tokens` and `tsc --noEmit` both clean. The dashboard now has a place to plot fidelity that doesn't depend on the user reading CLI output. |
| 2026-05-11 | D2.2 | **`scripts/lora-runner-ab.sh` end-to-end orchestrator.** One command that runs the canonical four-step LoRA fidelity workflow: BASE-run (no adapter) → ADAPTER-run (with `--adapter`) → `lora-ab` compare → emit `status.json` matching the `<hu-fidelity-tile>` data contract. Required wiring: the runner now accepts `--adapter <path>` (+ optional `--adapter-id`) and calls `hu_provider_load_adapter` before any chat — fail-fast on adapter-load failure (returns the provider's error code immediately) prevents producing a base-model "after.json" that would silently zero the delta. The script adds a per-step empty-response sanity check (each runner returns exit 0 even when every chat call errors and the JSON is `["","",...]`; the script catches this between steps via grep so step 2 doesn't waste effort). Shellcheck-clean. Five distinct exit codes for CI consumption (0=pass, 1=bad args, 2=empty response set, 3=gate fail, plus pass-through of subprocess errors). Smoke-tested end-to-end against `lora_baseline_persona.json` with no provider available: the step-1 sanity check correctly fires with a clear "provider unreachable?" diagnostic, exit 2. With a real GGUF adapter and either the local huml provider (HU_ENABLE_LLAMACPP) or any provider that supports `load_adapter`, this is now a one-command operational path from "I trained a LoRA" to "here's the verified delta and a JSON ready for the dashboard tile". |
| 2026-05-11 | D2.2 | **`metrics.fidelity` C gateway method (live data for the dashboard tile).** `cp_admin_metrics_fidelity` (`src/gateway/cp_admin.c`) replaces the demo-gateway mock with live numbers. Both the CLI (`human ml fidelity-status`) and the gateway compute baseline metrics through the shared `src/ml/fidelity.c` primitives (`hu_ml_fidelity_resolve_target` + `hu_ml_fidelity_score_baseline`) so the two surfaces are byte-compatible — a refactor that pulls `~30` lines out of the CLI into a reusable module and removes a future drift surface. Persona resolution: explicit `params.persona` overrides the agent's active persona; with neither set, the handler returns a zero-state response (same shape, all zeros, `error:"no persona configured"`) so the tile always renders. The A/B section is opportunistic — the handler reads `~/.human/last_fidelity_ab.json` (override via `HUMAN_FIDELITY_AB_PATH` for tests) when the orchestrator script has dropped one, and falls back to `ab.available:false` otherwise. `ui/src/views/metrics-view.ts` now calls `metrics.fidelity` in parallel with `metrics.snapshot` (slow fidelity backend never blocks main paint) and renders the tile under a "Persona Fidelity" section. Threat notes: read-only, no caller-supplied paths, no PII surface. 4 new gateway tests pin: zero-state without persona, `params.persona` override, A/B file merge, persona-load-failure fallback. CLI refactor pinned by the existing 25 fidelity tests. |
| 2026-05-11 | D2.2 | **Acknowledgment-directive variant telemetry.** New `hu_personal_model_directive_telemetry_{snapshot,reset}` API (`include/human/memory/personal_model.h`, `src/memory/personal_model.c`) exposes per-variant fire counters maintained by `acknowledgment_directive_for_overlay`. The decision logic was extracted into `directive_variant_for_overlay` so the wording switch and the counter increment agree on which branch fired (no drift between "this is the formal_terse string" and "this is the formal_terse counter"). Six variants tracked: `null_overlay` (legacy `_build_prompt`), `default` (overlay present but no useful signal), `formal_terse`, `casual_emoji`, `casual_or_short`, `adaptive_emoji`. Counters use `__atomic_fetch_add` with relaxed ordering — thread-safe under multi-channel agent loops without a mutex. New gateway method `metrics.directive_telemetry` (`cp_admin_metrics_directive_telemetry`) emits `{total, variants:{null_overlay,default,formal_terse,casual_emoji,casual_or_short,adaptive_emoji}}` so dashboards can verify "are casual+emoji channels actually firing the casual_emoji branch, or is something cliffing them into default?". Demo gateway gains a representative mock (113/184 casual_emoji to illustrate iMessage). 7 new unit tests pin reset semantics, NULL→null_overlay, formal→formal_terse no-spillover, casual+moderate→casual_emoji, unspecified-overlay→default (NOT null_overlay), 3× casual+high accumulates to 3, and label getter coverage. 1 gateway integration test pins the `total:1, variants.casual_emoji:1` JSON shape. With this in place, the channel-overlay-aware directive work is no longer a "ship and hope" — the dashboard can prove the right variant fires per channel. |

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
