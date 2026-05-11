#ifndef HU_ML_LEARNER_H
#define HU_ML_LEARNER_H

/* W13 — Learning loop.
 *
 * Closes the M3 mission gap (CLAUDE.md): on-device gradient-based personal-
 * isation that actually changes the *chat-time model's* behavior, not just its
 * system prompt. The `hu_learner_t` vtable abstracts MLX (Apple Silicon),
 * llama.cpp (Linux/CUDA), and a deterministic CPU fallback. Backends consume
 * `hu_training_signal_t` arrays (DPO pairs from W4 verifier flags, persona
 * deltas from W5, case outcomes from W3) and emit a LoRA adapter file at
 * `cfg.adapter_output_path`, signed with `cfg.model_version` so the inference
 * cache (W10) can invalidate atomically on swap.
 *
 * Deliberate non-goals in this commit:
 *   - No provider vtable extension. Real adapter loading happens through a
 *     path-based discovery flow that the daemon (W14) will wire later. The
 *     PLANNED provider methods are documented at the bottom of this file.
 *   - No SFT, no full fine-tuning. Always low-rank.
 *   - No federated mixing. Per-user adapters only.
 *
 * Determinism contract: every backend whose `available()` returns true must
 * be reproducible — same seed, same signals, same `model_version` → same
 * `final_loss` and same adapter file bytes. The CPU fallback uses an
 * embedded splitmix64 PRNG and never touches `rand()` or `/dev/urandom`. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/persona/persona_deltas.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward decl — W7 graph-backed memory facade (`human/memory/memory.h`).
 * Distinct from legacy `hu_memory_t` in `human/memory.h` (Phase 0).
 * Incomplete `struct hu_memory_facade` only — do not duplicate the typedef
 * from memory/memory.h here. */
struct hu_memory_facade;

typedef enum hu_training_signal_kind {
    HU_TRAIN_DPO_PAIR = 0,        /* (preferred, dispreferred) — from W4 flags */
    HU_TRAIN_PERSONA_DELTA = 1,   /* style adjustment — from W5 applied deltas */
    HU_TRAIN_CASE_OUTCOME = 2,    /* (case_id, reward) — from W3 outcomes */
    HU_TRAIN_KIND_MAX
} hu_training_signal_kind_t;

typedef struct hu_dpo_pair {
    char prompt[1024];
    char preferred[1024];
    char dispreferred[1024];
    float weight;                 /* margin/confidence; default 1.0 */
} hu_dpo_pair_t;

typedef struct hu_training_signal {
    hu_training_signal_kind_t kind;
    union {
        hu_dpo_pair_t dpo;
        struct {
            hu_persona_delta_t delta;
        } persona;
        struct {
            int64_t case_id;
            float reward;         /* 0.0–1.0; >0.5 is "good case to imitate" */
        } case_outcome;
    } as;
    int64_t observed_at;          /* unix ms, for watermark/dedup by callers */
} hu_training_signal_t;

typedef struct hu_learner_config {
    char base_model_path[256];
    char adapter_output_path[256];
    char model_version[64];       /* propagates into adapter file + report */
    int rank;                     /* LoRA rank, default 8 */
    int max_steps;                /* default 200 */
    float learning_rate;          /* default 1e-4 */
    int batch_size;               /* default 4 */
    bool dp_enabled;              /* W15: DP-SGD — all backends (mlx, ggml, cpu) MUST
                                   * honor this flag. When true, gradient updates are
                                   * clipped per-sample and Gaussian noise is added
                                   * before the optimizer step. The privacy accountant
                                   * tracks cumulative (epsilon, delta) and aborts
                                   * training when the budget is exhausted. */
    float dp_epsilon;             /* W15: privacy budget; > 0 required if dp_enabled.
                                   * Recommended range: 1.0–10.0. Lower values give
                                   * stronger privacy but slower convergence. The CPU
                                   * backend enforces this; MLX/ggml backends MUST
                                   * also implement DP-SGD clipping + noise when this
                                   * flag is set. */
    float dp_clip_norm;            /* W15: per-sample gradient clipping max norm.
                                    * 0 = use default (1.0). Only applies when
                                    * dp_enabled is true. */
    int64_t budget_ms;            /* total wall budget; 0 = short-circuit */
    uint64_t seed;                /* seeds the backend PRNG; 0 → default */

    /* Frontier LoRA fields (Bridge B — MLX backend). When data_dir is set,
     * the MLX backend uses it directly as `--data <dir>` and ignores the
     * signal array. This lets callers point at pre-existing JSONL data
     * (e.g. ~/.human/training-data/finetune/) instead of synthesizing
     * signals. */
    char data_dir[256];           /* pre-existing JSONL data directory */
    int num_layers;               /* LoRA layers to fine-tune; 0 = backend default */
    int max_seq_length;           /* max sequence length; 0 = backend default */
    int save_every;               /* checkpoint save frequency; 0 = don't pass */
} hu_learner_config_t;

hu_learner_config_t hu_learner_default_config(void);

/* ──────────────────────────────────────────────────────────────────────────
 * W15 — Rényi DP privacy accountant.
 *
 * Tracks cumulative privacy spend across multiple training rounds using
 * basic composition (advanced composition / RDP conversion is a future
 * refinement). Each call to `record_query` represents one training run
 * that consumed `epsilon_step` of the budget. The accountant accumulates
 * linearly (sequential composition theorem).
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct hu_dp_accountant {
    double epsilon_spent;         /* cumulative epsilon consumed */
    double delta;                 /* fixed delta (per-run, not cumulative) */
    int queries_count;            /* number of training rounds recorded */
} hu_dp_accountant_t;

void hu_dp_accountant_init(hu_dp_accountant_t *a, double delta);
void hu_dp_accountant_record_query(hu_dp_accountant_t *a, double epsilon_step);
double hu_dp_accountant_total_epsilon(const hu_dp_accountant_t *a);

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
    const char *name;             /* "mlx", "ggml", "cpu" */
    bool (*available)(void);
    hu_error_t (*train)(void *ctx, const hu_learner_config_t *cfg,
                        const hu_training_signal_t *signals, size_t signals_count,
                        hu_learner_report_t *out_report);
    void (*deinit)(void *ctx);
} hu_learner_vtable_t;

typedef struct hu_learner {
    const hu_learner_vtable_t *vt;
    void *ctx;
    hu_allocator_t *alloc;
    /* W13 wire-up (learner_bridge.c): pending signals queued by signal-source
     * adapters between training cycles. The W14 sleep scheduler drains this
     * via hu_learner_pending_drain() and feeds it to vt->train. NULL until
     * the first emit; capacity grows up to HU_LEARNER_PENDING_MAX. */
    struct hu_training_signal *pending;
    size_t pending_count;
    size_t pending_cap;
    /* Idempotency watermarks: the bridge drops anything with id/ts at or
     * below these values, so replaying the same source produces no extra
     * signals. 0 means "no signals consumed yet". */
    int64_t pending_persona_delta_id_high;
    int64_t pending_outcome_ts_high;
    /* W15 privacy accountant — tracks cumulative DP budget across training
     * rounds. Initialized lazily on first DP-enabled train call. */
    hu_dp_accountant_t dp_accountant;
} hu_learner_t;

/* Hard cap on the per-learner pending buffer. Once full, new signals are
 * dropped on the floor and the watermark still advances — this prevents
 * unbounded growth when the scheduler is offline for a long time. */
#define HU_LEARNER_PENDING_MAX 128

/* Constructors. `hu_learner_open_default` picks the best available backend
 * (mlx → ggml → cpu) and never returns HU_ERR_NOT_SUPPORTED — the CPU
 * fallback is always available. `hu_learner_open_named` returns
 * HU_ERR_NOT_FOUND for unknown backend names and HU_ERR_NOT_SUPPORTED when a
 * recognised backend declines to initialise on this host. */
hu_error_t hu_learner_open_default(hu_allocator_t *alloc, hu_learner_t **out);
hu_error_t hu_learner_open_named(hu_allocator_t *alloc, const char *backend_name,
                                 hu_learner_t **out);

/* Dispatching API. */
hu_error_t hu_learner_train(hu_learner_t *l, const hu_learner_config_t *cfg,
                            const hu_training_signal_t *signals, size_t signals_count,
                            hu_learner_report_t *out_report);

void hu_learner_close(hu_learner_t *l);

/* ──────────────────────────────────────────────────────────────────────────
 * Convenience signal builders.
 *
 * Each function reads a v1 store via the facade's underlying graph and
 * emits a freshly-allocated signal array. The reads are PURE — calling a
 * builder twice returns the same set of signals (idempotent). Real
 * consumers must track their own watermark (e.g. by `observed_at`) to avoid
 * retraining on the same data.
 *
 * Free with `hu_learner_signals_free`.
 * ──────────────────────────────────────────────────────────────────────── */

/* W4 verifier flags → DPO pairs. Today we surface them as quarantined
 * persona deltas (the v1 capture point for "the agent suggested X but the
 * verifier rejected it"). preferred = "" (the absence), dispreferred =
 * delta.value. Future revisions will read a dedicated flag log. */
hu_error_t hu_learner_signals_from_verifier_flags(struct hu_memory_facade *m, hu_allocator_t *alloc,
                                                  const char *contact_id, size_t cid_len,
                                                  hu_training_signal_t **out, size_t *out_count);

/* W5 applied persona deltas → positive style adaptation signals. */
hu_error_t hu_learner_signals_from_persona_deltas(struct hu_memory_facade *m, hu_allocator_t *alloc,
                                                  const char *contact_id, size_t cid_len,
                                                  hu_training_signal_t **out, size_t *out_count);

/* W3 case outcomes → reward signal. Reward is derived from the outcome
 * string ("ok"/"good"/"success" → 1.0, "bad"/"pushed back"/"failed" → 0.0,
 * everything else → 0.5). */
hu_error_t hu_learner_signals_from_case_outcomes(struct hu_memory_facade *m, hu_allocator_t *alloc,
                                                 const char *contact_id, size_t cid_len,
                                                 hu_training_signal_t **out, size_t *out_count);

void hu_learner_signals_free(hu_allocator_t *alloc, hu_training_signal_t *signals,
                             size_t count);

/* ──────────────────────────────────────────────────────────────────────────
 * PLANNED — provider extension method signatures (NOT installed in this
 * commit). Wiring `load_adapter` onto every provider touches all 9 native
 * providers + 41 OpenAI-compatible adapters and is too risky for one PR.
 *
 *     hu_error_t (*load_adapter)(void *ctx, const char *adapter_path);
 *     hu_error_t (*unload_adapter)(void *ctx);
 *     const char *(*active_adapter)(void *ctx);
 *
 * Today the discovery flow is path-based: the W14 sleep scheduler invokes
 * the learner, reads `report.adapter_path` + `report.model_version`, and
 * forwards them to providers that opt-in via a future migration. The
 * `model_version` field is the cache-invalidation key for W10's KV cache.
 * ──────────────────────────────────────────────────────────────────────── */

/* Adapter file format (used by the CPU backend; MLX/ggml backends MAY use
 * their own native formats but MUST also accept this fallback for
 * round-tripping in tests). Little-endian throughout.
 *
 *   offset 0   :  4 bytes  magic "HLAD"
 *   offset 4   :  4 bytes  version (currently 1)
 *   offset 8   : 64 bytes  model_version, NUL-padded
 *   offset 72  :  8 bytes  rank (uint64)
 *   offset 80  :  8 bytes  num_weights (uint64)
 *   offset 88  :  N*4 bytes weights as little-endian fp32
 *
 * Total size: 88 + num_weights*4 bytes. Always <= 25 MB by construction
 * (rank * 2 * 4096 * 4 ~ 64 KB at rank=8). */
#define HU_LEARNER_ADAPTER_MAGIC "HLAD"
#define HU_LEARNER_ADAPTER_VERSION 1u

#ifdef __cplusplus
}
#endif

#endif /* HU_ML_LEARNER_H */
