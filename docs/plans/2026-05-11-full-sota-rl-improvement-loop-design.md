---
status: closed
last_audit: 2026-05-25
---

# Full-SOTA RL & Neural Improvement Loop — Design Spec

**Status:** DRAFT (awaiting user review before plan-writing)
**Author:** Authored collaboratively in brainstorming session, May 11 2026
**Owner:** Plan author (assigned at plan-writing handoff)
**Linked plan:** *(to be created at `docs/plans/2026-05-11-full-sota-rl-improvement-loop.md` after spec approval)*
**Linked audit:** `docs/audits/2026-05-11-rl-loop-baseline-audit.md` *(to be created in P0)*
**Position in track structure:** **Track D Phase 2 — closed-loop RL** on top of Track D Phase 1 (in-flight, see `docs/plans/2026-05-10-master-follow-through-program.md` Track D rows). This spec consumes Track D Phase 1 primitives (`hu_communication_style_fidelity_score`, `human ml lora-baseline`/`lora-ab`, `--from-history`/`--persist`, personal-model v4 decay) and adds the RL training layer (DPO/KTO/GRPO/RM) on top. See §1.5 for the explicit fold-in mapping.

**Naming convention used throughout this spec:**
- The proof artifact lives at `docs/proof/rl-loop-proof.md` (single canonical file, kept current with the latest shipping adapter). Historical snapshots are archived to `docs/proof/archive/<YYYY-MM-DD>-rl-loop-proof.md` at each release tag.
- Adversarial audit report follows the same convention: `docs/proof/adversarial-audit-report.md` + dated archives.
- Adapter evidence directories under `~/.human/proofs/` use full dated IDs: `~/.human/proofs/<YYYY-MM-DD>-<method>-step-<N>/`.

---

## 1. Goal

Build, ship, and prove a closed reinforcement-learning + neural improvement loop in the `human` runtime that is competitive with 2026 state of the art (Apple Foundation Models adapter system, Google Gemini Nano personalization, the trl/verl RLHF stack). Specifically:

> A user chats with `human`. They react with 👎 to a response. A background trainer collects the preference signal, performs a real RL update on a LoRA adapter for a local Gemma-3-4B-it model, hot-swaps the new adapter, and the next response on the same prompt is **measurably and provably different**, with persona fidelity improved by ≥5% and no regression on standard benchmarks. This entire loop runs locally on Apple Silicon, in C11, with evidence saved to disk and an adversarially-reviewed audit trail.

The win condition is the **scorecard published in `docs/proof/rl-loop-proof.md`** (deltas measured as **absolute persona-fidelity-score points** on the scale defined in §11 row 4, not relative percentage):

| Method | Persona fidelity | MT-Bench | IFEval | Latency p95 |
|---|---|---|---|---|
| Stock Gemma-3-4B-it (baseline) | reference | reference | reference | reference |
| + our DPO LoRA | ≥ +5% | within 1% | within 2% | ≤ +50ms |
| + our KTO LoRA | ≥ +3% | within 1% | within 2% | ≤ +50ms |
| + our GRPO LoRA | ≥ +5% | within 1% | within 2% | ≤ +50ms |
| Apple FM (where available) | reported honestly | n/a | n/a | reported |
| Gemini Nano (where available) | reported honestly | n/a | n/a | reported |

If those numbers are reproducible by a fresh-clone reviewer, v1 is done.

---

## 1.5. Coordination with In-Flight Track D Work

This spec was authored against an audit baseline of May 11 2026 (committed master). At the same time a substantial **Track D Phase 1** body of work was in flight (uncommitted) that ships ~30% of what an early draft of this spec proposed to build. Rather than duplicate, this spec **consumes** Track D Phase 1 primitives wherever they exist and **adds** only the RL layer on top.

### 1.5.1 Already-shipped Track D Phase 1 primitives this spec consumes (NOT duplicated)

| In-flight primitive | Lives at | Consumed by spec section |
|---|---|---|
| `hu_communication_style_fidelity_score` (3-axis: lowercase + abbreviation + length) | `src/memory/personal_model.{h,c}` | §4.6 (extended with 4th axis, not replaced) |
| `hu_communication_style_compare_response_sets` | `src/memory/personal_model.{h,c}` | §4.6 (eval gate uses directly) |
| `human ml lora-baseline --persona <name>` CLI | `src/ml/cli.c` | §6 CI gate |
| `human ml lora-ab --before/--after [--require-positive] [--floor-delta]` CLI | `src/ml/cli.c` | §6 CI gate (every adapter promotion) |
| `scripts/check-lora-baseline.sh` (wired into `verify-all.sh`, floor 0.50) | `scripts/` | §6.5 CI matrix |
| `tests/fixtures/lora_baseline_persona.json` | `tests/fixtures/` | §11 Q4 (extended, not replaced) |
| `hu_persona_banks_extract_from_history` (PII-redacted, quality-filtered, deduped, channel-segregated) | `src/persona/examples.c` | §4.6.5 (the SFT data layer beneath RL) |
| `--from-history` + `--from-history-max` + `--persist` flags on `lora-persona` | `src/ml/cli.c` | §4.6.5 (SFT pipeline) |
| `hu_persona_creator_write` example_banks round-trip | `src/persona/creator.c` | §4.6.5 (SFT data persistence) |
| `hu_ml_lora_persona_caveat_block` / `_doc_path` (centralized caveat strings) | `src/ml/m3_frontier_adapter.c` | §4.1 (P0 caveat work is OBSOLETE — already shipped) |
| `hu_m3_adapter_should_disable` (config + env kill-switch) | `src/ml/m3_frontier_adapter.{h,c}` | §3.1 (rollback path for the adapter seam) |
| `hu_agent_m3_on_provider_success` (wired in 11 sites: `agent_turn.c` + `agent_stream.c`) | `src/agent/agent_turn.c`, `agent_stream.c` | §3.1 (the chat-time hook for adapter inference is already wired) |
| Personal Model v4 (symmetric signal aging, `apply_decay`, `per_turn_tick`, daemon hourly decay, recently-completed-goals scratchpad, v3→v4 migration) | `src/memory/personal_model.{h,c}`, `src/daemon.c`, `src/agent/agent_turn.c` | §4.6.5 (training-data freshness signal feeds trainer scheduler) |

### 1.5.2 Confirmed still-broken bugs the spec's P0 still owns

These were re-verified against the in-flight tree on May 11 2026:

- `src/ml/cli.c:190` — `hu_ml_train(alloc, &model, &optimizer, train_loader, NULL, &cfg.training, NULL, 0, &result)`. `vocab_size=0` and `token_bytes=NULL`. Same at line 2016 (feed-predictor path). NOT FIXED by Track D Phase 1.
- `src/ml/experiment.c:300-302` — `token_bytes=NULL` to `hu_ml_train`. NOT FIXED.
- `src/memory/personal_model.c:1831` — `hu_personal_model_save` is direct `fopen("wb")` with no temp file or rename. The substantial v4 work landed code adjacent to this function but did not fix the atomicity issue.
- `hu_dpo_train_step` — still misnamed; no rename in the in-flight diff.
- `CLAUDE.md:53` — still claims atomic-rename + DPO mischaracterization.

### 1.5.3 Boundary agreement (who owns what)

To prevent conflict during parallel work:

- **Track D Phase 1 (existing, ongoing)** — owns: personal-model layer (decay, freshness, goals lifecycle), persona example banks (extraction, persistence), offline persona-fidelity scorer (3-axis), `human ml lora-persona`, `human ml lora-baseline`, `human ml lora-ab`, the M3 adapter seam (fixture, kill switch, chat-time hook), caveat strings.
- **Track D Phase 2 (this spec)** — owns: real DPO/KTO/GRPO trainers, reward model with value head, MLX subprocess training server, llama.cpp Metal inference completion, channel-reaction inbound wiring → preference DB, eval gate (composing existing primitives + a 4th decision-style axis), competitive harness vs Apple FM / Gemini Nano, the closed-loop E2E test.
- **Shared (touched by both)** — `src/ml/cli.c` (Track D adds CLI subcommands; Track D Phase 2 adds `cli_dpo.c`, `cli_kto.c`, `cli_grpo.c`, `cli_rm.c` in new files to avoid conflict), `src/memory/personal_model.{h,c}` (Track D Phase 2 adds the 4th decision-style axis to `hu_communication_style_fidelity_score` as an additive extension), `tests/fixtures/lora_baseline_persona.json` (Track D Phase 2 adds a 4th rubric axis to the same fixture).

**Coordination cadence:** rebase against `main` at the start of every phase + after any Track D Phase 1 commit landing in the spec's shared files. See risk #11 in §10.

---

## 2. Background & Motivation

A 5-explorer adversarial audit of `src/ml/`, `src/agent/`, `src/memory/`, `src/eval/`, `src/persona/`, `src/providers/`, and `src/daemon.c` on May 11 2026 (committed `main` baseline) found the state below. Where Track D Phase 1 in-flight work has changed the picture, the deltas are noted explicitly.

**What's real (in committed `main`):**
- Hand-derived backward + Muon+AdamW optimizer in `src/ml/gpt.c`, `train.c`, `muon_adamw.c`. Tests do real finite-difference grad checks.
- Real LoRA primitives in `src/ml/lora.c` (low-rank A/B, apply, backward, save/load).
- Real preference-data layer in `src/ml/dpo.c` (SQLite `dpo_pairs`, JSONL export).
- Real hot-swap adapter loading at daemon startup and post-train (`src/daemon.c:2465-2532`, `src/agent/lora_training_runner.c:90-93`).
- Real eval harness with LLM-as-judge, bootstrap regression detection, nightly W16 bench (`src/eval/eval.c`, `evaluation.yml`).

**What Track D Phase 1 (in-flight, uncommitted) has additionally shipped** (see §1.5.1 for the full mapping):
- Personal Model v4 with symmetric signal aging + decay + goals lifecycle + daemon hourly tick.
- Offline persona-fidelity scorer (3-axis `hu_communication_style_fidelity_score`) + A/B comparator + `human ml lora-baseline` and `human ml lora-ab` CLIs + `scripts/check-lora-baseline.sh` CI gate.
- Banks-from-history SFT data pipeline (`hu_persona_banks_extract_from_history` + `--from-history` + `--persist`).
- M3 adapter seam: kill switch (`hu_m3_adapter_should_disable`), chat-time hook (`hu_agent_m3_on_provider_success` in 11 sites), centralized caveat strings.
- Persona JSON example_banks round-trip (`hu_persona_creator_write`).

**What's still broken or misnamed (re-verified against the in-flight tree):**
- `hu_ml_cli_train`, `hu_ml_cli_train_feed_predictor`, and `hu_experiment_loop` all pass `vocab_size=0` or `token_bytes=NULL` to `hu_ml_train`, defeating the CE objective. Three flagship CLI subcommands silently no-op. NOT fixed by Track D Phase 1.
- `hu_dpo_train_step` is **not DPO**: it asks an external LLM to score 0–100 and aggregates a synthetic loss. No policy log-probs, no reference model, no gradient on policy weights. Misleading name. NOT renamed in Track D Phase 1.
- `lora-persona` in-process backward passes raw logits where it should pass `softmax(logits) − one_hot(target)`, mathematically inconsistent with its documented NLL loss.
- `m3_frontier_adapter.c` is a fixture file-format probe, not a frontier adapter. The caveats around this are now centralized (Track D Phase 1) but the fixture nature is unchanged.
- `llamacpp_chat_with_system` returns `HU_ERR_NOT_SUPPORTED` even when llama.cpp is linked.
- `hu_personal_model_save` does `fopen("wb")` with no temp file or rename. The Track D Phase 1 v4 work landed code adjacent to this function but did not fix the atomicity issue. `CLAUDE.md:53` still claims it's "atomic-rename" save. Documentation drift.
- Channel reactions are outbound-only: inbound 👎 tapbacks/reactji do not reach `hu_dpo_record_from_feedback`. Preference collection relies on substring matching of the user's next text message ("good", "wrong", "thanks").
- No reward model, no value head, no policy-gradient RL anywhere in the binary.
- No real local-inference path: `lora-baseline` and `lora-ab` score response-text strings the user supplies; nothing in the binary actually generates those responses from a personalized local model.

**What this spec adds on top of Track D Phase 1:** real DPO + KTO + GRPO + reward model with value head, llama.cpp Metal inference completion, channel reaction → preference DB wiring, eval gate that composes the existing primitives + a 4th decision-style axis, competitive harness vs Apple FM / Gemini Nano, and the closed-loop E2E test.

The full audit will be archived to `docs/audits/2026-05-11-rl-loop-baseline-audit.md` as part of Phase 0.

---

## 3. Architecture

### 3.1 High-level system

```
                            ┌──────────────────────────────────────┐
                            │  USER (iMessage / Slack / Discord)   │
                            └──────────┬───────────────────────────┘
                                       │ message + reactions
                       ┌───────────────▼───────────────┐
                       │   Channel inbound handlers    │  (NEW: reaction → event)
                       │   (tapback / reactji parsers) │
                       └───────────────┬───────────────┘
                                       │ user_event {msg | reaction}
                       ┌───────────────▼───────────────┐
                       │       agent_turn pipeline     │
                       └───────────────┬───────────────┘
                                       │
                  ┌────────────────────┼────────────────────┐
                  │                    │                    │
        ┌─────────▼─────────┐  ┌───────▼────────┐  ┌────────▼────────┐
        │  src/providers/   │  │ Preference DB  │  │ Personal model  │
        │  llamacpp.c       │  │ (SQLite)       │  │ (existing)      │
        │  Gemma-3-4B-it    │  │ - dpo_pairs    │  └─────────────────┘
        │  Metal backend    │  │ - kto_signals  │
        │  + LoRA hot-swap  │  │ - rl_rollouts  │
        └─────────▲─────────┘  │ - rm_examples  │
                  │            └───────┬────────┘
                  │ adapter            │
                  │ hot-swap           │ scheduled / on-pressure
                  │            ┌───────▼────────┐
                  │            │  Trainer       │  ← hu_lora_training_runner
                  │            │  scheduler     │
                  │            └───┬───┬───┬────┘
                  │                │   │   │
                  │       ┌────────▼┐ ┌▼─┐ ┌▼──────┐
                  │       │ DPO     │ │KTO│ │ GRPO  │  (NEW: real implementations
                  │       │ trainer │ │   │ │ +RM   │   via MLX subprocess)
                  │       └────┬────┘ └─┬─┘ └───┬───┘
                  │            │        │       │
                  │            └────────┼───────┘
                  │                     │ candidate adapter (.safetensors → GGUF LoRA)
                  │            ┌────────▼────────┐
                  │            │  Eval gate      │  (existing eval + NEW persona/regression gates)
                  │            │  - persona ≥ Δ  │
                  │            │  - regression ≤ │
                  │            │  - latency ≤    │
                  │            └────────┬────────┘
                  │                     │ pass
                  └─────────────────────┘
                                       │
                       ┌───────────────▼────────────────┐
                       │ Competitive eval harness (NEW) │
                       │ vs Apple FM / Gemini Nano /    │
                       │ stock Gemma-3-4B baseline       │
                       └────────────────────────────────┘
```

### 3.2 The three closed loops

1. **Inference loop (chat-time):** user → `agent_turn` → `llamacpp_chat_with_system` (Metal backend) + currently-loaded LoRA adapter → response. Today: broken (`HU_ERR_NOT_SUPPORTED`). After v1: real.
2. **Preference loop (every reaction):** reaction event → SQLite preference store → trainer scheduler → DPO/KTO/GRPO step (via MLX subprocess) → adapter (`.safetensors` → GGUF LoRA conversion) → `hu_provider_load_adapter` hot-swap → next chat is different. Today: substring-heuristic only, no real training. After v1: real.
3. **Quality loop (every adapter swap):** trained adapter → `eval_gate` (persona fidelity + regression + latency, all bootstrap-CI'd) → competitive scorecard → accept/reject promotion → write `~/.human/proofs/<adapter-id>/` evidence directory. Today: eval exists, retraining trigger doesn't. After v1: real.

### 3.3 Module dependency direction (must remain inward)

```
channels/ ──► agent/ ──► providers/, ml/, memory/, eval/
                          ▲           ▲
                          │           │
                       eval/ ─────────┘
                       ml/  ──► providers/  (rollout.c needs provider.chat for GRPO)
```

`providers/` never depends on `ml/`. This is the wall that keeps the inference path lean and the training path swappable.

### 3.4 Net-new vtable surfaces

```c
// hu_provider_t — EXISTING; src/providers/llamacpp.c finishes its impl
//   chat_with_system(), load_adapter(), supports_native_tools()

// NEW: uniform RL trainer interface — DPO, KTO, GRPO all implement this
typedef struct hu_rl_trainer_vtable {
    const char *(*name)(void *ctx);
    hu_error_t (*prepare)(void *ctx, const hu_rl_data_t *data);
    hu_error_t (*step)(void *ctx, const hu_rl_batch_t *batch, double *out_loss);
    hu_error_t (*save_adapter)(void *ctx, const char *path);
    void       (*deinit)(void *ctx);
} hu_rl_trainer_vtable_t;

typedef struct hu_rl_trainer {
    void *ctx;
    const hu_rl_trainer_vtable_t *vtable;
} hu_rl_trainer_t;

// NEW: reward model abstraction
typedef struct hu_reward_model_vtable {
    hu_error_t (*score)(void *ctx, const char *prompt, const char *response, double *out);
    hu_error_t (*score_batch)(void *ctx, const char **prompts, const char **responses,
                              size_t n, double *out);
    void       (*deinit)(void *ctx);
} hu_reward_model_vtable_t;

// NEW: external judge abstraction for competitive eval
typedef struct hu_eval_judge_external_vtable {
    const char *(*name)(void *ctx);                        // "apple_fm" | "gemini_nano" | ...
    bool        (*available)(void *ctx);
    hu_error_t  (*generate)(void *ctx, const char *prompt, char *out, size_t out_cap);
    void        (*deinit)(void *ctx);
} hu_eval_judge_external_vtable_t;
```

These three new vtables are the only net-new public surfaces. Everything else is implementation behind them.

---

## 4. Component Breakdown — File Map

Roughly **~80 new source/test files** (additional fixtures, scripts, Python, Swift, JS, and CI YAML push the total touched-file count toward ~100), **~12 modify**, **~5,500 LOC new C11**, **~800 LOC Python**, **~200 LOC Swift**. The earlier draft estimated similar; Track D Phase 1 fold-in removed conceptual duplicates (parallel persona-fidelity scorer, parallel `lora-baseline` infrastructure) but did not significantly reduce the file count because the spec's net-new RL training layer (DPO / KTO / GRPO / RM / MLX subprocess / channel reaction wiring) is the bulk of the work. Every new source file gets a paired test file. Every loss function gets a finite-difference grad check.

Module size discipline: **target ≤500 LOC per file.** New CLI subcommands are added in new files (`cli_dpo.c`, `cli_kto.c`, `cli_grpo.c`, `cli_rm.c`) rather than growing existing `src/ml/cli.c`.

### 4.1 Phase 0: Honesty pass (~9 files touched, slimmed by Track D Phase 1 coordination)

Items removed from this phase because Track D Phase 1 already shipped them:
- `src/ml/m3_frontier_adapter.c` caveat block work — DONE: `hu_ml_lora_persona_caveat_block`/`_doc_path` centralized in `m3_frontier_adapter.c`, `cli.c` consumes them at 3 sites, 4 snapshot tests pin the substrings.
- `docs/plans/2026-05-10-master-follow-through-program.md` audit — DONE: Track D rows updated to `done` for D0.3–D2.2.

| Action | Path | Responsibility |
|---|---|---|
| MODIFY | `src/ml/cli.c:190, 2016` | Fix `vocab_size=0` + `token_bytes=NULL` silent bug at the `hu_ml_train(..., NULL, 0, ...)` call sites |
| MODIFY | `src/ml/experiment.c:300-302` | Fix `token_bytes=NULL` silent bug |
| MODIFY | `src/memory/personal_model.c` (around line 1831) | Atomic save: `fopen(tmp) → fwrite → fflush → fsync(fileno) → fclose → rename(tmp, final)`. Coordinate with Track D Phase 1 v4 work in adjacent code. |
| MODIFY | `src/ml/dpo.c` | Rename `hu_dpo_train_step` → `hu_dpo_judge_step`; old name kept as `__attribute__((deprecated))` shim returning the same values |
| MODIFY | `include/human/ml/dpo.h` | Header for rename + deprecation |
| MODIFY | `CLAUDE.md:53` | Fix atomic-rename + DPO mischaracterizations to match reality post-fix |
| NEW | `tests/test_personal_model_atomic_save.c` | Crash-during-save safety via `fork()` + `kill -9` between write/rename; verify the final file is either the pre-save state (intact) or the post-save state (intact), never partial |
| NEW | `tests/test_ml_cli_actually_trains.c` | CLI training paths now produce non-zero loss reduction on synthetic data with `vocab_size=128` and a non-NULL `token_bytes` |
| NEW | `tests/test_dpo_judge_naming.c` | Deprecation shim works + new name returns identical values for identical inputs |
| NEW | `docs/audits/2026-05-11-rl-loop-baseline-audit.md` | Snapshot of the 5-explorer audit as historical record |

### 4.2 Phase 1: llama.cpp Metal inference (~10 new + 2 modify)

| Action | Path | Responsibility |
|---|---|---|
| MODIFY | `src/providers/llamacpp.c:125-135` | Implement `chat_with_system`: tokenize → decode loop → sample → detokenize |
| MODIFY | `src/providers/llamacpp.c` (Metal flag) | `n_gpu_layers = -1` on `__APPLE__`, configurable elsewhere |
| NEW | `src/providers/llamacpp_sampling.c` | Temperature + top-k + top-p + min-p sampling (~200 LOC) |
| NEW | `include/human/providers/llamacpp_sampling.h` | Sampling API |
| NEW | `src/providers/llamacpp_kvcache.c` | Multi-turn KV cache reuse + system-prompt prefix cache (~300 LOC) |
| NEW | `include/human/providers/llamacpp_kvcache.h` | KV cache API |
| NEW | `src/providers/llamacpp_decode.c` | Decode loop isolated for testability (~250 LOC) |
| NEW | `include/human/providers/llamacpp_decode.h` | Decode API |
| MODIFY | `CMakeLists.txt` | Vendor llama.cpp at pinned tag, default `HU_ENABLE_LLAMACPP=ON` for `rl_sota` preset, `HU_LLAMACPP_METAL=ON` on Apple |
| NEW | `scripts/fetch-gemma-gguf.sh` | Reproducible Gemma-3-4B-it Q4_K_M fetch with SHA-256 verification |
| NEW | `tests/test_llamacpp_chat_metal.c` | Integration test (gated `HU_HAVE_GEMMA_GGUF=1`) |
| NEW | `tests/test_llamacpp_lora_hotswap.c` | Hot-swap test using known-perturbing LoRA fixture |
| NEW | `tests/test_llamacpp_kvcache.c` | KV cache correctness vs no-cache baseline |
| NEW | `tests/test_llamacpp_sampling.c` | Sampling determinism with fixed seed |

**Phase 1 ends with a "stock Gemma sanity gate":** the base model must pass a 20-prompt response-quality eval before P2 builds on it.

### 4.3 Phase 2: Real DPO + reaction wiring (~14 new + 3 modify)

| Action | Path | Responsibility |
|---|---|---|
| NEW | `src/ml/policy_logprobs.c` | Compute `log π_θ(y\|x)` for arbitrary (x,y) via teacher-forced forward (~200 LOC) |
| NEW | `include/human/ml/policy_logprobs.h` | Public API |
| NEW | `src/ml/reference_model.c` | Frozen π_ref: clone gpt_t weights, freeze, expose forward-only (~250 LOC) |
| NEW | `include/human/ml/reference_model.h` | Public API |
| NEW | `src/ml/dpo_real.c` | Real DPO loss `−log σ(β·(log π_θ(y_w\|x)/π_ref(y_w\|x) − log π_θ(y_l\|x)/π_ref(y_l\|x)))` with backward (~350 LOC). Implements `hu_rl_trainer_t`. |
| NEW | `include/human/ml/dpo_real.h` | Public API |
| NEW | `src/ml/rl_trainer.c` | `hu_rl_trainer_t` vtable definition + factory (~150 LOC) |
| NEW | `include/human/ml/rl_trainer.h` | Vtable header |
| NEW | `src/ml/cli_dpo.c` | `human ml dpo-train` subcommand (~150 LOC) |
| MODIFY | `src/ml/cli.c` | Dispatch to `cli_dpo.c` (≤20 LOC delta) |
| MODIFY | `src/channels/imessage_inbound.c` (or equivalent) | Parse inbound tapback events → emit reaction event |
| MODIFY | `src/channels/slack_inbound.c` | Parse inbound `reactions.added` events → emit reaction event |
| NEW | `src/channels/reaction_event.c` | Channel-agnostic reaction event normalizer (~150 LOC) |
| NEW | `include/human/channels/reaction_event.h` | Event type definition |
| NEW | `src/agent/reaction_handler.c` | Reaction event → preference pair lookup + SQLite store (~200 LOC) |
| NEW | `include/human/agent/reaction_handler.h` | Public API |
| MODIFY | `src/agent/agent_turn.c` | Wire reaction events through `reaction_handler` (replaces substring heuristic for *reaction* events; substring fallback retained for text-channel users) |
| NEW | `tests/test_dpo_real_loss.c` | Finite-difference grad check on real DPO loss |
| NEW | `tests/test_dpo_real_e2e.c` | Synthetic prefs → DPO step → verify `log π(y_w)` ↑, `log π(y_l)` ↓ |
| NEW | `tests/test_policy_logprobs.c` | Teacher-forced log-probs match expected for known weights |
| NEW | `tests/test_reference_model.c` | π_ref forward matches base π_θ at clone time, stays frozen after π_θ updates |
| NEW | `tests/test_reaction_handler_e2e.c` | iMessage tapback → SQLite preference row, end-to-end |

### 4.4 Phase 3: KTO + reward model with value head (~10 new + 1 modify)

| Action | Path | Responsibility |
|---|---|---|
| NEW | `src/ml/kto.c` | KTO loss (sigmoid-based single-signal, λ_D for desirable / λ_U for undesirable, reference-aware) (~300 LOC). Implements `hu_rl_trainer_t`. |
| NEW | `include/human/ml/kto.h` | Public API |
| NEW | `src/ml/value_head.c` | Linear value head on backbone hidden state, forward + backward (~200 LOC) |
| NEW | `include/human/ml/value_head.h` | Public API |
| NEW | `src/ml/reward_model.c` | RM = backbone (Qwen-2.5-0.5B GGUF) + value head; implements `hu_reward_model_t` (~300 LOC) |
| NEW | `include/human/ml/reward_model.h` | Vtable + factory |
| NEW | `src/ml/reward_model_train.c` | RM training loop on collected pairs (Bradley-Terry log-likelihood) (~250 LOC) |
| NEW | `src/ml/cli_kto.c` | `human ml kto-train` subcommand |
| NEW | `src/ml/cli_rm.c` | `human ml rm-train` subcommand |
| MODIFY | `src/ml/cli.c` | Dispatch to `cli_kto.c` and `cli_rm.c` (≤20 LOC delta) |
| NEW | `tests/test_kto_loss.c` | Finite-difference grad check + sign-of-gradient check on synthetic signal |
| NEW | `tests/test_value_head.c` | Forward + backward grad check |
| NEW | `tests/test_reward_model_train.c` | RM trains to reproduce known preference order on synthetic data |
| NEW | `tests/test_reward_model_inference.c` | RM inference latency < 50ms for 512-token completion |

### 4.5 Phase 4: GRPO + multi-rollout (~9 new + 1 modify)

| Action | Path | Responsibility |
|---|---|---|
| NEW | `src/ml/rollout.c` | Sample N completions per prompt via `hu_provider_t.chat`; parallel via OpenMP if available (~250 LOC) |
| NEW | `include/human/ml/rollout.h` | Public API |
| NEW | `src/ml/grpo.c` | GRPO loss: group-relative baseline `(r_i − mean(r))/std(r)`, PPO ratio clip ε=0.2, KL penalty β=0.04 to π_ref (~450 LOC). Implements `hu_rl_trainer_t`. |
| NEW | `include/human/ml/grpo.h` | Public API |
| NEW | `src/ml/kl_divergence.c` | KL between two log-prob distributions over token vocab (~100 LOC) |
| NEW | `include/human/ml/kl_divergence.h` | Public API |
| NEW | `src/ml/cli_grpo.c` | `human ml grpo-train` subcommand |
| MODIFY | `src/ml/cli.c` | Dispatch to `cli_grpo.c` (≤10 LOC delta) |
| NEW | `tests/test_grpo_loss.c` | Finite-difference grad check on full GRPO loss |
| NEW | `tests/test_rollout.c` | N rollouts return N distinct (or with seed, deterministic) completions |
| NEW | `tests/test_kl_divergence.c` | Self-KL = 0; non-negativity; symmetry-breaking properties |
| NEW | `tests/test_grpo_e2e.c` | Synthetic reward fn + GRPO trains policy to match it on toy task |

### 4.6 Phase 5: Eval gate + competitive harness (~12 new + 3 modify)

**Major restructure from earlier draft:** Track D Phase 1 already shipped a deterministic 3-axis offline persona-fidelity scorer (`hu_communication_style_fidelity_score`), an A/B comparator (`hu_communication_style_compare_response_sets`), CLIs (`human ml lora-baseline`, `human ml lora-ab`), a CI gate (`scripts/check-lora-baseline.sh`), and a fixture (`tests/fixtures/lora_baseline_persona.json`). This phase **extends** that scorer with a 4th axis (decision-style match) and **composes** the existing primitives in `eval_gate`, rather than building a parallel `persona_fidelity_v2` module.

| Action | Path | Responsibility |
|---|---|---|
| MODIFY | `src/memory/personal_model.c` (additive) | Add 4th decision-style axis to `hu_communication_style_fidelity_score`. Decision-style match measures: (a) hedging-vs-direct word frequency (e.g. "maybe", "perhaps", "definitely"), (b) question-vs-statement ratio, (c) imperative-vs-suggestive verbs. Same triangular-match shape as the existing 3 axes. Preserves the old 3-axis behavior behind a new entry-point `hu_communication_style_fidelity_score_v1` (deprecated shim) so existing callers and CI fixtures don't break; the new default returns the 4-axis mean. (~150 LOC additive) |
| MODIFY | `include/human/memory/personal_model.h` | Header for 4-axis API + v1 deprecation shim |
| MODIFY | `tests/fixtures/lora_baseline_persona.json` | Extend the existing fixture with decision-style example responses (~30 LOC additive, score floor in `check-lora-baseline.sh` re-tuned if needed) |
| NEW | `tests/fixtures/lora_baseline_persona_v2_responses.json` | 100 prompt-tagged reference responses scored on the 4-axis rubric by the corpus owner; held-out from training (per §11 Q4) |
| NEW | `tests/fixtures/lora_baseline_persona_v2_rubric.md` | The 4-axis rubric documented for reviewer re-rating |
| NEW | `src/eval/leaderboard.c` | MT-Bench / AlpacaEval / IFEval offline runners with cached gold judges (~400 LOC) |
| NEW | `include/human/eval/leaderboard.h` | Public API |
| NEW | `src/eval/eval_gate.c` | Combined gate composing existing primitives: persona ≥ Δ via `hu_communication_style_compare_response_sets` + 4th axis, regression ≤ ε via leaderboard, latency p95 ≤ τ — all bootstrap CIs, lower-95-CI > baseline-upper-95-CI required (~200 LOC) |
| NEW | `include/human/eval/eval_gate.h` | Public API |
| NEW | `src/eval/judge_external.c` | `hu_eval_judge_external_t` vtable + factory (~100 LOC) |
| NEW | `include/human/eval/judge_external.h` | Vtable header |
| NEW | `src/eval/apple_fm_client.c` | Bridge to Apple Foundation Models via Swift FFI subprocess (~300 LOC) |
| NEW | `scripts/eval_external/apple_fm_server.swift` | Long-running Swift server speaking JSON over stdio |
| NEW | `src/eval/gemini_nano_client.c` | Bridge to Chrome Built-in AI / `window.ai` API via headless Chrome (~250 LOC) |
| NEW | `scripts/eval_external/chrome_ai_server.js` | Headless Chrome bridge |
| NEW | `src/eval/stock_baseline.c` | Stock Gemma-3-4B (no LoRA) baseline judge (~100 LOC) |
| NEW | `src/eval/competitive_harness.c` | Orchestrates side-by-side: stock / our DPO / our KTO / our GRPO / Apple FM / Gemini Nano. Uses `hu_communication_style_compare_response_sets` for the persona-fidelity column. (~400 LOC) |
| NEW | `include/human/eval/competitive_harness.h` | Public API |
| MODIFY | `src/agent/lora_training_runner.c` | Call `eval_gate` before promoting adapter; reject if gate fails. Reuse `human ml lora-ab --require-positive` semantics where applicable. |
| MODIFY | `src/ml/cli.c` | `human eval competitive`, `human eval leaderboard` subcommands (dispatch to existing eval surface, ≤30 LOC delta) |
| NEW | `tests/test_communication_style_fidelity_v2_axis.c` | The 4th decision-style axis: score known persona pairs, verify expected ordering and 3-axis backward compat |
| NEW | `tests/test_leaderboard.c` | Mock leaderboard runs, verify score parsing |
| NEW | `tests/test_eval_gate.c` | Gate accepts/rejects on synthetic eval results matching policy |
| NEW | `tests/test_competitive_harness.c` | Mock external judges, verify scorecard rendering |

### 4.6.5 SFT pipeline integration (Track D Phase 1 surface that this spec consumes, not duplicates)

The Track D Phase 1 in-flight work ships a **complete SFT (supervised fine-tuning) data pipeline**:

```
~/.human/memory.db (conversation history)
    │
    ▼  hu_persona_banks_extract_from_history (PII redact + quality + dedup + channel split)
    │
~/.human/personas/<name>.json (example_banks per channel)
    │
    ▼  hu_persona_creator_write (with --persist)  +  human ml lora-persona --export-jsonl
    │
<persona>.jsonl (Alpaca format)
    │
    ▼  human ml lora-persona --backend mlx  →  python -m mlx_lm.lora
    │
<sft-adapter>.safetensors / GGUF LoRA
    │
    ▼  hu_provider_load_adapter (existing)
    │
chat-time inference with SFT-personalized base model
```

**This spec's RL layer (DPO/KTO/GRPO/RM) layers ON TOP of SFT**, not in parallel:

```
SFT-personalized adapter (above)
    │
    ▼  reaction events → SQLite preference/signal store (THIS SPEC, P2/P3)
    │
    ▼  trainer scheduler picks DPO | KTO | GRPO based on signal type + volume (THIS SPEC, cross-cutting)
    │
RL-refined adapter
    │
    ▼  eval_gate (THIS SPEC, P5) — composes lora-baseline + lora-ab + leaderboard + decision-style 4th axis
    │
    ▼  hu_provider_load_adapter (existing)  ←  hot-swap on gate pass
    │
chat-time inference with SFT+RL personalized base model
```

The trainer scheduler (§4.8 `src/ml/trainer_scheduler.c`) decides at run time whether to:
1. Run SFT only (cold start: no preference data yet, banks-from-history available)
2. Run RL only (existing SFT adapter present, new preference data accumulated)
3. Run SFT-then-RL (curriculum: re-train SFT with refreshed banks, then DPO/KTO on top)

This is documented in the plan, not a separate code module — the scheduler reads three counters (`sft_data_count`, `pref_pair_count`, `signal_count`) and picks the recipe.

### 4.7 Phase 6: E2E proof + demo (~5 new)

| Action | Path | Responsibility |
|---|---|---|
| NEW | `tests/test_e2e_rl_loop.c` | The proof: chat → simulated 👎 → trainer → adapter → re-chat → assert response changed AND eval_gate passed |
| NEW | `scripts/demo-rl-loop.sh` | Reproducible demo script that produces the win-condition table |
| NEW | `scripts/record-demo.sh` | asciinema recording wrapper |
| NEW | `docs/proof/rl-loop-proof.md` | The proof artifact: methodology + results + scorecard + reproducibility recipe |
| NEW | `docs/proof/adversarial-audit-report.md` | Aggregated `critic` + `sprint-auditor` findings + remediations |
| NEW | `docs/proof/archive/.gitkeep` | Directory for dated historical snapshots (release-time archiving) |

### 4.8 Cross-cutting (~16 new files, touched in multiple phases)

| Action | Path | Responsibility |
|---|---|---|
| NEW | `src/ml/trainer_scheduler.c` | Orchestrates which RL method runs when based on config + collected data volume (~250 LOC) |
| NEW | `include/human/ml/trainer_scheduler.h` | Public API |
| NEW | `src/ml/adapter_format.c` | Bridges HUML LoRA ↔ MLX safetensors ↔ GGUF LoRA formats (~400 LOC) |
| NEW | `include/human/ml/adapter_format.h` | Public API |
| NEW | `src/ml/mlx_subprocess.c` | Long-running MLX Python training server lifecycle: spawn, JSON-over-Unix-socket protocol, heartbeat every 10s, resurrect on death with exponential backoff (~350 LOC) |
| NEW | `include/human/ml/mlx_subprocess.h` | Public API |
| NEW | `scripts/mlx_trainer/server.py` | Python MLX server: receives JSON requests, runs DPO/KTO/GRPO via mlx-lm, returns adapter bytes |
| NEW | `scripts/mlx_trainer/requirements.txt` | Pinned mlx, mlx-lm, transformers versions |
| NEW | `src/ml/training_telemetry.c` | Step-level metrics → SQLite (loss, KL, grad norm, sample reward distribution) (~200 LOC) |
| NEW | `include/human/ml/training_telemetry.h` | Public API |
| NEW | `tests/test_adapter_format.c` | HUML ↔ MLX safetensors ↔ GGUF round-trip tests |
| NEW | `tests/test_mlx_subprocess.c` | Subprocess lifecycle: spawn, train one step, kill mid-train, resurrect, complete |
| NEW | `tests/test_trainer_scheduler.c` | Scheduler picks correct method based on data shape |
| NEW | `.github/workflows/rl-sota.yml` | CI: macOS M-series + Linux runners; full `rl_sota` build; T4 E2E with fixture-tiny model (gated on PRs touching `src/ml/`, `src/providers/llamacpp.c`, `src/eval/`) |
| NEW | `.github/workflows/rl-sota-full.yml` | CI: manual-dispatch + nightly local-only; fetches real Gemma-3-4B; runs T6 competitive harness |
| NEW | `tests/fixtures/reactions/imessage_tapback_*.json` | Golden inbound tapback fixtures for parser tests |
| NEW | `tests/fixtures/reactions/slack_reaction_added_*.json` | Golden inbound reactji fixtures for parser tests |
| NEW | `scripts/lint-no-secrets.sh` | Lint pass: detects API keys, absolute model paths, `~/Documents/...` references |

### 4.9 External dependencies

| Dep | Why | Where | Size impact |
|---|---|---|---|
| **llama.cpp** (pinned tag, vendored) | Inference engine for Gemma-3-4B with Metal/CUDA | `third_party/llama.cpp/` | ~2 MB to compiled binary; Metal kernels runtime-loaded |
| **Gemma-3-4B-it Q4_K_M GGUF** | The base model | Fetched at runtime to `~/.human/models/`, not vendored | ~2.4 GB on user disk |
| **Qwen-2.5-0.5B-Instruct Q4_K_M GGUF** | Reward model backbone | Fetched at runtime to `~/.human/models/` | ~400 MB on user disk |
| **MLX + mlx-lm** (Python, version-pinned) | Training engine | `scripts/mlx_trainer/requirements.txt`; spawned as subprocess | Zero binary impact |
| **Apple Foundation Models framework** (optional) | Competitive baseline | macOS 26+ with developer entitlement; Swift bridge subprocess | Zero (system framework) |
| **Chrome Canary + `window.ai` API** (optional) | Gemini Nano competitive baseline | Headless Chrome subprocess | User-installed |

### 4.10 New CMake options

```cmake
option(HU_ENABLE_LLAMACPP         "Vendor llama.cpp for local inference"        OFF)
option(HU_LLAMACPP_METAL          "Enable Metal backend (Apple Silicon)"        ON)
option(HU_ENABLE_RL_FULL          "DPO_REAL + KTO + GRPO + RM + scheduler"      OFF)
option(HU_ENABLE_MLX_TRAINER      "MLX subprocess training server"              OFF)
option(HU_ENABLE_COMPETITIVE_EVAL "Apple FM + Gemini Nano comparison harness"   OFF)
```

New preset in `CMakePresets.json`:
```
"rl_sota": {
  HU_ENABLE_ML=ON, HU_ENABLE_LLAMACPP=ON, HU_LLAMACPP_METAL=ON,
  HU_ENABLE_RL_FULL=ON, HU_ENABLE_MLX_TRAINER=ON, HU_ENABLE_COMPETITIVE_EVAL=ON,
  HU_ENABLE_SQLITE=ON, HU_ENABLE_ALL_CHANNELS=ON
}
```

Default `release` preset stays at current flags (RL features strictly opt-in). Hard sanity gate: default release binary delta ≤ +250 KB vs prior release.

---

## 5. Phasing — 6 Phases, No Cuts

| # | Phase | Goal |
|---|---|---|
| **0** | Honesty pass | All silent bugs fixed. All misleading names corrected. Documentation drift reconciled. Audit committed. |
| **1** | llama.cpp Metal inference | `human chat --provider llamacpp --model gemma-3-4b-it` returns coherent text on Apple Silicon. LoRA hot-swap works against fixture adapter. KV cache reused across turns. **Stock Gemma sanity gate passes** (20-prompt response-quality eval at `tests/fixtures/gemma_sanity_prompts.json`; pass criterion: ≥18/20 responses are coherent + on-topic per LLM-judge with rubric committed alongside). |
| **2** | Real DPO + reaction wiring | Real DPO loss with frozen π_ref + policy log-probs. iMessage tapback + Slack reactji inbound → SQLite preference row. Scheduled DPO trainer (via MLX subprocess) produces adapter that, when hot-swapped, measurably changes response on the prompt that earned the 👎. |
| **3** | KTO + reward model w/ value head | KTO trainer (single-signal optimization). Reward model = Qwen-2.5-0.5B + linear value head, trained on collected pairs. RM inference < 50ms for 512-token completion. |
| **4** | GRPO + multi-rollout | GRPO trainer with N=4 rollouts per prompt, group-relative baseline, PPO-style clip ε=0.2, KL penalty β=0.04 to π_ref. Rollouts done via MLX provider with parallel sampling. **Timeline padded by 50%** vs. other phases per risk #5 in §10. |
| **5** | Eval gate + competitive harness | `eval_gate` integrated into `lora_training_runner`. Persona fidelity v2 + leaderboard runners online. Apple FM bridge (where entitlement available). Gemini Nano bridge (where `window.ai` available). Scorecard publishable. |
| **6** | E2E proof + demo | `tests/test_e2e_rl_loop.c` passes. `scripts/demo-rl-loop.sh` produces win-condition scorecard. Proof + adversarial audit reports written. |

No phase is cuttable; if any phase slips by >50% past estimate, escalate before starting next phase. Single allowed escape valve: defer Phase 4 (GRPO) to v1.5 — DPO + KTO + RM still gives a credible SOTA story.

Estimated calendar: **5–7 weeks of focused senior-engineer work**. Each phase ends in shippable, testable software.

**Track D Phase 1 coordination per phase:**
- **At phase start**: rebase against `main`. Confirm no Track D Phase 1 changes invalidate spec assumptions. Re-run the §1.5.1 fold-in mapping check.
- **At phase end**: spec changes that touch shared files (`src/ml/cli.c`, `src/memory/personal_model.{h,c}`, `tests/fixtures/lora_baseline_persona.json`) get reviewed against the latest Track D Phase 1 commits before the phase is marked done.
- **Cross-phase mid-flight Track D commits**: if Track D Phase 1 lands new files in `src/ml/` or `src/memory/personal_model.c` between phase boundaries, the next phase's `spec-verifier` gate (§7) re-validates the still-valid scope.

---

## 6. Testing & Proof Harness

### 6.1 Six-tier testing ladder

| Tier | What | Required for |
|---|---|---|
| T1: Unit | One file, one behavior, no I/O | Every new file |
| T2: Property / grad-check | Mathematical invariants + finite-diff gradients | Every loss + every optimizer |
| T3: Integration / vtable | Vtable contract end-to-end with real impl | Every vtable impl |
| T4: E2E behavioral | Real model, real adapter, real chat — observable behavior change | Every phase end |
| T5: Adversarial | Subagent dispatch (`critic`, `verifier`, `aspect-panel`) | Per code change ≥100 LOC and per phase end |
| T6: Competitive | Side-by-side vs external systems (Apple FM, Gemini Nano, base) | Phases 5 + 6 only |

### 6.2 Test count + coverage targets

- **+~80 test functions**, total `9,800 → ~9,900` tests at ship.
- Coverage targets (gcov on `dev` preset, enforced by `scripts/coverage-check.sh` in CI):
  - All new `src/ml/*.c`: ≥90% line, ≥80% branch
  - All loss functions: 100% (finite-diff grad check is the test)
  - All `hu_rl_trainer_t` impls: 100% interface contract via table-driven test
  - All channel reaction parsers: 100% on golden fixtures committed to `tests/fixtures/reactions/`
  - `src/providers/llamacpp.c` (new code only): ≥85% (subprocess paths excluded)
  - `src/eval/competitive_harness.c`: ≥80% (external judges mocked in unit tests)

### 6.3 Test suite tiering for runtime control

```
./build/human_tests --suite=ML-unit         # T1 + T2, <30s
./build/human_tests --suite=RL-integration  # T3 + T4 with mocked tiny model, <60s
HU_HAVE_GEMMA_GGUF=1 ./build/human_tests --suite=RL-E2E-FULL  # T4 with real model, ~5min, CI nightly
```

### 6.4 Hard sanity gates (CI-enforced, block merge)

1. 0 ASan errors (existing standard, extended to MLX subprocess via `HU_TEST=1`)
2. 0 UBSan errors (NEW for ML code — overflow detection on accumulators)
3. Default release binary size delta ≤ +250 KB vs the **most recent tagged release on `main`** at the time of the SOTA merge commit (baseline frozen at branch fork; CI checks against this snapshot)
4. `human_tests` (no args) runtime delta ≤ +30 sec vs the **same baseline as #3**
5. Every new public function has a header doc-comment (lint-enforced)
6. Every new test has at least one `assert` or `HU_TEST_ASSERT_*` (no smoke-only tests for math)
7. `scripts/lint-no-secrets.sh` clean (no API keys, no absolute model paths, no `~/Documents/...`)

### 6.5 CI matrix

| Workflow | Trigger | Runs |
|---|---|---|
| `ci.yml` (existing, extended) | Every PR | Standard build + 9,900 tests with mocked models — T1, T2, T3, T4. **Includes existing Track D Phase 1 gate `scripts/check-lora-baseline.sh`** (already wired into `verify-all.sh`); after the 4th decision-style axis lands, the gate validates the 4-axis mean stays ≥ floor. |
| `rl-sota.yml` (NEW) | Every PR touching `src/ml/`, `src/providers/llamacpp.c`, `src/eval/` | macOS M-series + Linux runners; full `rl_sota` build; T4 E2E with fixture-tiny model (~10 MB GGUF for CI). **Adds a per-PR `human ml lora-ab --before <pre.json> --after <post.json> --require-positive` gate** against committed before/after fixtures so any LoRA-affecting code change is proven not to regress persona-fidelity delta. |
| `rl-sota-full.yml` (NEW) | Nightly + manual dispatch | macOS M-series only; fetches real Gemma-3-4B; runs T6 competitive harness; publishes scorecard. **Local-only, manual run before each release tag** (no paid CI runner) |

---

## 7. Adversarial Review Process — Mandatory Gates

Bound to user commitment: "Adversarial Reviewed with deep critical evidence-based audits." All eight subagent gates below are **mandatory** at the listed points. None are advisory.

| When | Subagent | What it does | Gates |
|---|---|---|---|
| Per phase, before any code | `spec-verifier` | Reads spec section + impl plan, reports gaps | Phase cannot start until 0 gaps |
| Per code change ≥100 LOC | `critic` | Adversarial review of diff: half-fixes, missing edge cases, regressions | Fixes filed before commit |
| Per behavioral claim | `verifier` | Actually runs the code, captures observed behavior + evidence | Claim cannot be made without verifier evidence |
| High-risk phases (P2 dpo_real, P4 grpo, P5 eval_gate) | `aspect-panel` | 5-verifier panel (correctness / edge-case / security / regression / style) with confidence-weighted vote | Phase cannot ship if disagreement ≥40% |
| Per phase end | `sprint-auditor` | Independently re-reads spec + actual deliverables, answers "did we ship this?" without trusting team claims | Phase marked complete only on auditor PASS |
| Test breaks unexpectedly | `regression-hunter` | Bisects to breaking commit, reports failing assertion + blame | Required before "flaky test" hypothesis |
| Once at end of every phase | `dead-code-finder` | Catches unused exports / unreachable branches | Cleanup before commit |
| Once at Phase 5 completion | `security-reviewer` | OWASP-style review of subprocess management (MLX server, Apple FM bridge, Chrome bridge) | Findings fixed before competitive eval ships |

**Per-phase workflow:**
```
spec → spec-verifier (gate) → implement task-by-task →
  per-task: write failing test → run → impl → run → critic on diff → verifier on behavior → commit
phase end → aspect-panel (if high-risk) → dead-code-finder → sprint-auditor → ship
```

---

## 8. Evidence Artifacts

Every adapter promotion writes `~/.human/proofs/<adapter-id>/`:

```
~/.human/proofs/2026-05-15-dpo-step-0042/
├── manifest.json              # Trainer, base model SHA, hyperparams, training pairs SHA
├── training_curves.json       # loss / KL-to-ref / grad-norm / reward-mean per step
├── eval_before.json           # Stock Gemma + previous adapter scores
├── eval_after.json            # New adapter scores
├── eval_delta.json            # Computed deltas with bootstrap CIs
├── delta_responses.md         # Same-prompt before/after for 20 fixed prompts
├── gate_decision.json         # eval_gate verdict + rationale
├── adversarial_review.md      # critic + sprint-auditor outputs
└── reproduce.sh               # One command that re-runs the whole thing
```

The proof artifact in `docs/proof/rl-loop-proof.md` indexes these directories and presents the win-condition scorecard from §1.

---

## 9. Definition of Done — The Ship Contract

v1 ships when **all** of these are true:

1. All 80 new tests pass, 0 ASan, 0 UBSan
2. `cmake --preset rl_sota && cmake --build --preset rl_sota` clean on macOS aarch64
3. `./build/human chat --provider llamacpp --model gemma-3-4b-it-Q4_K_M` returns coherent text
4. `./build/human ml dpo-train --pairs <N≥50>` produces a valid `.safetensors` LoRA adapter
5. `./build/human ml kto-train --signals <N≥100>` produces a valid LoRA adapter
6. `./build/human ml grpo-train --rollouts 4` produces a valid LoRA adapter
7. `./build/human ml rm-train` produces a valid reward model checkpoint
8. `tests/test_e2e_rl_loop.c` passes: chat → reaction → train → re-chat → response measurably changed AND eval_gate passed
9. `./build/human eval competitive --persona seth` produces the win-condition scorecard with bootstrap CIs
10. `~/.human/proofs/<final-adapter-id>/` exists with all 9 evidence files
11. `docs/proof/rl-loop-proof.md` indexes the proof and presents the scorecard
12. `sprint-auditor` subagent has issued PASS verdict on every phase (logged in audit report)
13. `docs/proof/adversarial-audit-report.md` exists with all `critic` + `aspect-panel` findings + remediations
14. Either: (best case) Apple FM column + Gemini Nano column populated honestly with real numbers, OR (honest fallback) scorecard shows `unavailable (reason)` for those columns with documented why

No subjective "done." This is the contract.

---

## 10. Risks & Mitigations

| # | Risk | Severity | Mitigation |
|---|---|---|---|
| 1 | Gemma-3-4B-it LoRA recipe maturity (March 2026 release) | High | P1 ends with stock Gemma sanity gate. P2 starts with recipe-search task (rank ∈ {8,16,32}, lr ∈ {1e-5, 5e-5, 1e-4}) on HH-RLHF subset before training on user data. |
| 2 | Apple FM access (requires entitlement + macOS 26+) | Medium | `apple_fm_client.c` calls `available()` first; harness writes `apple_fm: unavailable (reason)` row honestly |
| 3 | Gemini Nano access (Chrome Canary + `window.ai` API) | Medium | Same graceful-degradation pattern as #2 |
| 4 | MLX subprocess fragility (Python venv, mlx-lm churn) | Medium | Pin all Python versions. Heartbeat every 10s. Resurrect on death with exponential backoff. CI nightly proves subprocess survives 100 train steps. |
| 5 | GRPO is the highest-risk single block (multi-rollout slow, KL-penalty schedule sensitive, group-baseline numerical stability) | High | P4 gets `aspect-panel` mandatory. Reference: trl/grpo_trainer.py + verl/grpo_trainer.py during spec-verifier phase. P4 timeline padded by 50%. |
| 6 | Test runtime explosion | Medium | Tier the suite via `--suite=` selectors (see §6.3) |
| 7 | Timeline drift past 7 weeks under "no cuts, full clean code" | High | Weekly retrospective at end of each phase. >150% of estimate → escalate. Single escape valve: defer GRPO to v1.5. |
| 8 | Eval gate gives false-positive promotions | Medium | Bootstrap CIs (not point estimates) on every metric. Lower 95% CI > baseline upper 95% CI required. Real held-out user prompts in eval suite. |
| 9 | Reward hacking in GRPO | Medium | KL penalty to π_ref; RM trained on diverse data + held-out validation; eval_gate's persona fidelity + leaderboard catch regressions RM missed |
| 10 | Personal data leakage in adapter weights | High | PII-category preprocessing (existing `src/security/` redaction). Optional DP-SGD via existing `hu_dp_accountant`. Documented in privacy section of proof artifact. |
| 11 | **Coordination drift with active Track D Phase 1 work** | High | (a) §1.5.3 boundary agreement (who owns what) is the contract; (b) rebase-against-main at every phase start (§5 phasing notes); (c) shared files (`src/ml/cli.c` shared via separate `cli_*.c` files for each new subcommand; `personal_model.{h,c}` shared via additive 4th axis only; `lora_baseline_persona.json` shared via additive rubric); (d) `spec-verifier` subagent re-validates fold-in mapping at each phase start (§7); (e) any Track D Phase 1 commit in the spec's shared files triggers an out-of-band coordination check before the next spec commit lands. |
| 12 | **The in-flight `lora-baseline` / `lora-ab` measure SCORER agreement, not GENERATIVE quality** | Medium | The 3-axis (and our extended 4-axis) scorer measures whether response text matches a fingerprint. It does NOT measure whether the response is *good* (factual, helpful, coherent). The eval gate composes scorer-based persona fidelity with leaderboard-based quality (MT-Bench / IFEval / AlpacaEval) so an adapter that maximizes fingerprint-match by becoming incoherent gets caught by the leaderboard regression check. The §1 win-condition explicitly tracks both columns. |

---

## 11. Open Questions — Resolved

These were resolved during brainstorming with stated rationale. Plan author: deviation from these defaults requires evidence + spec amendment.

| # | Question | Decision | Rationale |
|---|---|---|---|
| 1 | MLX subprocess transport | Length-prefixed JSON over Unix domain socket | Simpler than gRPC; no head-of-line blocking like stdio; debuggable with `socat`; no protobuf build dep |
| 2 | Reward model backbone | Qwen-2.5-0.5B-Instruct Q4_K_M GGUF + linear value head | MIT license; ~400 MB Q4; fast inference (<50ms achievable); strong at this size class |
| 3 | GRPO reward function source | Trained RM as primary, LLM-judge as fallback for cold-start (<200 pairs), rule-based safety filter on top | Standard 2025-26 stack; RM gives consistent signal, judge bootstraps, rules prevent obvious bad outputs |
| 4 | Persona fidelity v2 ground truth | 100 persona-tagged prompts committed to **`tests/fixtures/lora_baseline_persona_v2_responses.json`** (extending, not replacing, the existing `tests/fixtures/lora_baseline_persona.json` Track D Phase 1 fixture). Reference responses **rated by the corpus owner** (Seth, for the demo persona) on a 0–1 score with a 4-axis rubric. **Axes match the in-flight 3-axis `hu_communication_style_fidelity_score` (lowercase + abbreviation + length match) plus the new 4th axis added in §4.6 (decision-style: hedging-vs-direct vocab + question-vs-statement ratio + imperative-vs-suggestive verbs).** Rubric committed to `tests/fixtures/lora_baseline_persona_v2_rubric.md`. Persona-fidelity score = mean of 4 axes; reported as absolute points (0–1 scale) | Held-out from training; versioned with eval; reviewers can inspect rubric and re-rate; "≥+5%" in §1 means ≥+0.05 absolute points; backward-compatible — `hu_communication_style_fidelity_score_v1` continues to expose the 3-axis path for callers that don't want the 4th axis |
| 5 | Min preference pairs before first DPO step | 50 | Below 50, gradient noise dominates; trainer scheduler defers and logs `insufficient_data` |
| 6 | Single-persona vs multi-persona for v1 | Single-persona ("Seth") | Don't drag multi-tenant complexity into the proof; ship single case clean, scale to per-persona adapter routing in v1.5 |
| 7 | Demo persona | Real Seth persona via consented private corpus (option 3) | Strongest narrative; requires private-data discipline (see §13) |
| 8 | DPO β hyperparameter | 0.1 (DeepSeek/Anthropic standard); search {0.01, 0.1, 0.5} during P2 recipe-search | Default in trl/dpo_trainer.py; well-studied range |
| 9 | LoRA target modules on Gemma-3-4B | Q + K + V + O projections + gate + up + down projections (all linear in transformer block); rank=8 | Standard for instruction-tuning; ~16M trainable params; deviation requires evidence |
| 10 | KL penalty β for GRPO | 0.04 (DeepSeek R1 default), constant schedule for v1 | Constant simpler to debug; decay schedule is optimization, not correctness |

---

## 12. Out of Scope — Explicit Deferrals

The following are tempting under "Full SOTA" but are explicitly **NOT in v1**. Documented now to prevent mid-flight scope creep.

- ❌ **Multi-tenant adapter routing** (one adapter per user/persona at chat time) — v1.5
- ❌ **Speculative decoding** — v1.5 inference perf optimization
- ❌ **In-process MLX bindings** (mlx-c vendoring) — Path A from brainstorming, not Path C
- ❌ **Online RL during chat** (TTT-style weight updates per message) — v2 research
- ❌ **Constitutional AI / self-critique loops** — separate concern
- ❌ **Cross-language LoRA**
- ❌ **Quantization-aware fine-tuning** (we LoRA on F16 base, deploy F16 + LoRA; Q4 deploy variant deferred)
- ❌ **Multi-modal personalization** (vision/audio LoRA) — text only in v1
- ❌ **Ranking models for tool selection** — separate from response RL
- ❌ **HuLa skill RL** (RL on tool orchestration) — separate concern
- ❌ **iOS/macOS native app integration** with the trained adapter — v1.5
- ❌ **Web dashboard visualization of training curves** — proof artifact is markdown + JSON; nice dashboards are v1.5
- ❌ **Distillation** (teacher-student to compress LoRA into base weights) — research

---

## 13. Privacy & Data Handling — Seth Corpus

Demo persona uses a **real consented private corpus** (open question #7, option 3). Three binding commitments:

1. **`~/.human/private/` is git-ignored.** The corpus and any derivatives (training data, fine-tuned adapters trained on it, intermediate checkpoints) live exclusively under `~/.human/private/` and never enter the repository. `.gitignore` updated in P0.
2. **All published proof artifacts go through PII redaction.** `delta_responses.md` and any document under `docs/proof/` referencing Seth-corpus content runs through `src/security/`'s PII redaction (emails, phone, addresses, full names of third parties) before commit. A pre-commit hook enforces this on `docs/proof/`.
3. **Reproducibility recipe documents methodology, not corpus.** `docs/proof/rl-loop-proof.md` tells reviewers *how* to reproduce the workflow with their own corpus, since they cannot replay yours. The exact win-condition numbers are specific to the Seth corpus; the methodology and all infrastructure is fully reproducible.

Optional differential-privacy SGD is available via existing `hu_dp_accountant` in `src/ml/learner.c`; whether to enable is a P5 user-facing config decision.

---

## 14. Reproducibility Recipe (the local "proven to work" contract)

A fresh-clone reviewer must be able to:

```bash
git clone <repo> && cd h-uman
git checkout <sota-merge-commit>
cmake --preset rl_sota && cmake --build --preset rl_sota -j
./scripts/fetch-gemma-gguf.sh                              # ~5 min, ~2.4 GB download, SHA-verified
./build/human_tests --suite=RL                              # all RL tests pass, 0 ASan
bash scripts/check-lora-baseline.sh                        # Track D Phase 1 gate: scorer + 4-axis fidelity floor (~1 sec)
./build/human ml lora-baseline --persona seth              # Track D Phase 1 baseline number (the upper bound a frontier model can hit without LoRA)
./scripts/demo-rl-loop.sh --corpus tests/fixtures/synthetic_persona_corpus/  # ~3 min: produces win-condition table
./build/human ml lora-ab --persona seth \
    --before docs/proof/<adapter-id>/before_responses.json \
    --after  docs/proof/<adapter-id>/after_responses.json \
    --require-positive                                     # Track D Phase 1 A/B gate proves persona-fidelity delta is positive
```

…and observe the same scorecard structure (specific numbers will differ on their corpus). If they cannot, the proof failed.

This recipe is enforced manually before each release tag (per user decision: no paid M-series CI runner for nightly automation; weekly automated reproduction deferred).

---

## 15. References

**RL methods:**
- Rafailov et al. 2023, *Direct Preference Optimization* (DPO)
- Ethayarajh et al. 2024, *KTO: Model Alignment as Prospect Theoretic Optimization*
- Shao et al. 2024 (DeepSeek), *DeepSeekMath: Pushing the Limits of Mathematical Reasoning in Open Language Models* (GRPO)
- Christiano et al. 2017 / Ouyang et al. 2022 (InstructGPT) — RLHF foundations
- huggingface/trl — reference impl for DPO, KTO, GRPO
- volcengine/verl — production RLHF framework (GRPO impl)

**On-device personalization:**
- Apple Foundation Models adapter system (WWDC 2025–2026 sessions)
- Google Gemini Nano with personalization (Pixel 9+ docs)
- ml-explore/mlx + ml-explore/mlx-lm — Apple Silicon training stack
- ggerganov/llama.cpp — local inference reference

**Audit baseline:** `docs/audits/2026-05-11-rl-loop-baseline-audit.md` (created in P0 from this conversation's 5-explorer audit)

---

## Acceptance

This spec is complete when the spec-author and the user both sign off. After acceptance, the implementation plan will be authored at `docs/plans/2026-05-11-full-sota-rl-improvement-loop.md` per the `superpowers:writing-plans` skill, with bite-sized tasks (≤5 min each) per phase.
