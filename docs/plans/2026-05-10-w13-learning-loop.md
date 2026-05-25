---
title: "W13 — Learning Loop: LoRA + DPO from W4 verifier flags + W5 deltas + W3 case outcomes"
created: 2026-05-10
status: deferred
parent: 2026-05-10-memory-v2-roadmap-overview.md
risk: high
scope: include/human/ml/, src/ml/, src/providers/, gated behind HU_ENABLE_LEARNING
last_audit: 2026-05-25
---

# W13 — Learning Loop

## Goal

Close the M3 mission gap (CLAUDE.md): on-device gradient-based personalization that actually changes the **chat-time model's** behavior, not just its system prompt. New `hu_learner_t` vtable with MLX (Apple Silicon), llama.cpp (Linux/CUDA), and CPU-fallback backends. Reads training signal from existing v1 sources: W4 verifier flags (negative DPO pairs), W5 persona deltas (positive style adaptation), W3 case outcomes (reward signal). Produces LoRA adapters that the existing chat path loads at inference.

## Motivation

Today, `human ml lora-persona` exists. It accepts `--persona <name>` and `--checkpoint <path>`, runs through `src/ml/lora.c`, and produces an adapter. But the adapter is for a **reference toy GPT** — not the frontier chat model the agent actually invokes. The gap is the wiring between training output and inference input.

This is the single biggest narrative win available to v2: "private model adapted to you" vs "private system prompt." Apple Intelligence has it; Gemini cannot do it on-device; we can.

## Prior art

- Apple Intelligence on-device LoRA (rumored architecture).
- MLX LoRA reference implementation (Apple Inc., 2024).
- DPO (Rafailov et al. 2023) — direct preference optimization without RM.
- CLAUDE.md mission table — M3 listed as "very hard, narrative doesn't match code."

## Design

### Vtable

```c
/* include/human/ml/learner.h */

typedef enum hu_training_signal_kind {
    HU_TRAIN_DPO_PAIR = 0,         /* (preferred, dispreferred) */
    HU_TRAIN_PERSONA_DELTA,        /* style adjustment */
    HU_TRAIN_CASE_OUTCOME,         /* (plan, reward) */
} hu_training_signal_kind_t;

typedef struct hu_dpo_pair {
    char prompt[1024];
    char preferred[1024];
    char dispreferred[1024];
    float weight;
} hu_dpo_pair_t;

typedef struct hu_training_signal {
    hu_training_signal_kind_t kind;
    union {
        hu_dpo_pair_t dpo;
        struct { hu_persona_delta_t delta; } persona;
        struct { int64_t case_id; float reward; } case_outcome;
    } as;
    int64_t observed_at;
} hu_training_signal_t;

typedef struct hu_learner_config {
    char base_model_path[256];
    char adapter_output_path[256];
    char model_version[64];        /* propagates to KV-cache invalidation */
    int rank;                      /* LoRA rank, default 8 */
    int max_steps;                 /* default 200 */
    float learning_rate;           /* default 1e-4 */
    int batch_size;                /* default 4 */
    bool dp_enabled;               /* W15: DP-SGD */
    float dp_epsilon;              /* W15: privacy budget */
    int64_t budget_ms;             /* total wall budget */
} hu_learner_config_t;

typedef struct hu_learner_report {
    size_t signals_consumed;
    size_t steps_completed;
    float final_loss;
    int64_t adapter_bytes;
    char adapter_path[256];
    char model_version[64];
    char last_error[128];
} hu_learner_report_t;

typedef struct hu_learner_vtable {
    const char *name;              /* "mlx", "ggml", "cpu" */
    bool (*available)(void);
    hu_error_t (*train)(void *ctx, const hu_learner_config_t *cfg,
                        const hu_training_signal_t *signals, size_t signals_count,
                        hu_learner_report_t *out_report);
    void (*deinit)(void *ctx);
} hu_learner_vtable_t;

typedef struct hu_learner {
    hu_learner_vtable_t *vt;
    void *ctx;
} hu_learner_t;

hu_error_t hu_learner_open_default(hu_allocator_t *alloc, hu_learner_t **out);  /* picks best */
hu_error_t hu_learner_open_named(hu_allocator_t *alloc, const char *backend_name,
                                  hu_learner_t **out);

/* Convenience: build training signals from existing v1 stores. */
hu_error_t hu_learner_signals_from_verifier_flags(hu_memory_t *m, hu_allocator_t *alloc,
                                                   const char *contact_id, size_t cid_len,
                                                   hu_training_signal_t **out, size_t *out_count);
hu_error_t hu_learner_signals_from_persona_deltas(hu_memory_t *m, hu_allocator_t *alloc,
                                                   const char *contact_id, size_t cid_len,
                                                   hu_training_signal_t **out, size_t *out_count);
hu_error_t hu_learner_signals_from_case_outcomes(hu_memory_t *m, hu_allocator_t *alloc,
                                                  const char *contact_id, size_t cid_len,
                                                  hu_training_signal_t **out, size_t *out_count);
```

### Adapter loading at inference

Provider extension:
```c
hu_error_t (*load_adapter)(void *ctx, const char *adapter_path);
hu_error_t (*unload_adapter)(void *ctx);
const char *(*active_adapter)(void *ctx);
```

When the daemon starts, it asks the learner for the latest adapter for the user; loads it via the active provider. Adapter is signed with `model_version` so cache invalidation is automatic.

### Training data flow

```
W4 verifier flags         W5 persona deltas         W3 case outcomes
        │                          │                          │
        └─── hu_learner_signals_from_* ───┐
                                          │
                                  hu_learner.train()
                                          │
                                  adapter @ ~/.human/adapters/<contact>/
                                          │
                                  W14 sleep scheduler invokes daily
                                          │
                                  provider.load_adapter()
                                          │
                                          ▼
                                Inference uses new adapter
```

## Phases

1. `learner.h` + abstract types.
2. CPU-fallback backend (deterministic, slow, for tests).
3. MLX backend (`src/ml/learner_mlx.c`) wrapping existing `src/ml/lora.c`.
4. ggml/llama.cpp backend (`src/ml/learner_ggml.c`).
5. Provider extension method on `hu_provider_t` for adapter loading.
6. Wire `human ml lora-persona` CLI through the new vtable.
7. Daemon hook to invoke training on schedule.
8. Adversarial tests + offline benchmark.

## Test plan

- `test_w13_cpu_backend_trains_deterministic_adapter`: tiny model, synthetic data, exact reproducibility.
- `test_w13_signals_from_verifier_flags_skips_already_consumed`: idempotent.
- `test_w13_dpo_pairs_have_no_self_inconsistencies`: preferred ≠ dispreferred.
- `test_w13_adapter_load_round_trip`.
- `test_w13_adapter_invalidates_kv_cache_on_version_bump`.
- `test_w13_adversarial_training_data_poisoning_resists`: 50% adversarial signals → adapter still helps on benign queries.
- `test_w13_dp_epsilon_budget_enforced`: requesting more steps than budget → error.
- `test_w13_e2e_persona_delta_to_adapter_to_inference`: full loop, A/B preference test.

## Success metric

- DPO-trained adapter measurable preference: ≥60% blind A/B preference for adapted vs base on personalized prompt suite.
- Training time for 200 steps on 100 signals: ≤ 5 minutes on M-series Mac (MLX).
- Adapter size: ≤ 25 MB.
- KV-cache (W10) correctly invalidates on adapter swap.
- Binary size delta ≤ +120 KB (gated behind HU_ENABLE_LEARNING).

## Risks

| Risk | Mitigation |
|------|------------|
| MLX/ggml backends differ in adapter format | `hu_learner_t` exposes only `adapter_path` + `model_version`; provider chooses how to load |
| Training too aggressive → adapter overfits to recent prompts | Step budget + early-stopping on validation loss |
| Adapter encodes leaked secrets from the conversation | W15 DP-SGD adds noise; secrets-detection lint on training data before it enters the trainer |
| Adapter size balloons | Rank cap; weights stored in fp16 |
| Training fails silently | Every report has `last_error`; CLI surfaces it; daemon logs to `~/.human/learning.log` |

## Out of scope

- Multi-user adapter mixing (federated). v2 is per-user only.
- Non-LoRA fine-tuning (full SFT). Always low-rank.
- Adapter compression beyond fp16.

## Binary size budget: +120 KB (gated behind HU_ENABLE_LEARNING).
