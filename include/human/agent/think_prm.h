#ifndef HU_AGENT_THINK_PRM_H
#define HU_AGENT_THINK_PRM_H

/* SOTA-2026 init-07 — ThinkPRM trained verifier panel runtime.
 *
 * Implements `hu_verifier_panel_t`, an ensemble of N small trained Process
 * Reward Model (PRM) scorers that score intermediate reasoning steps.
 *
 * Lifecycle:
 *   1. Caller obtains zero or more checkpoint file paths (see the training
 *      driver `human ml train-verifier`).
 *   2. `hu_verifier_panel_create` loads the on-disk weights via the existing
 *      `hu_ml_checkpoint_load` infrastructure (HUML format, version 1+).
 *      If every path is missing the panel returns `HU_ERR_NOT_SUPPORTED`
 *      rather than crashing — callers are expected to fall back to the
 *      legacy heuristic/reflection path in that case.
 *   3. `hu_verifier_panel_score_chain` splits the chain into steps on
 *      paragraph boundaries (`\n\n`) up to `max_steps`, then runs each
 *      scorer's deterministic forward kernel against every step and
 *      averages the per-scorer logits before applying a sigmoid.
 *   4. `hu_verifier_panel_result_free` releases the heap-allocated
 *      `steps` array.
 *   5. `hu_verifier_panel_deinit` releases the loaded weights.
 *
 * Determinism guarantee: given the same checkpoint files and the same
 * input chain, `score_chain` must return bitwise identical scores. The
 * scoring kernel is a deterministic dot product over the chain's
 * byte-derived feature vector with the loaded weight buffer; no float
 * reductions cross threads. Pinned by
 * `tests/test_think_prm.c::panel_score_is_deterministic_given_fixed_weights`.
 *
 * S2 scope: the runtime ships the panel ensemble + checkpoint load
 * pathway + deterministic forward. The full Qwen3-0.5B-class GPT
 * forward (BPE tokenization → multi-layer attention → sigmoid head)
 * lands as init-04's MLX bridge matures; until then the scoring
 * kernel uses the loaded weight buffer directly without a tokenizer.
 * See `docs/plans/2026-05-13-init-07-thinkprm-implementation-report.md`
 * "Deferred to S3".
 *
 * Default OFF: `hu_agent_t.sota.verifier_panel.scorer_count == 0` keeps
 * `agent_turn` byte-identical to the pre-init-07 code path. Pinned by
 * `tests/test_think_prm.c::agent_turn_is_byte_identical_when_panel_disabled`.
 *
 * Cross-initiative consumers: init-05 (verifier-driven TTT) and init-06
 * (SimPO/ORPO/GRPO-2) consume `hu_verifier_panel_t*` directly through
 * `hu_agent_t.sota.verifier_panel`. They MUST treat `scorer_count == 0`
 * as "no panel signal available; fall through".
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* On-disk magic + version constants for the PRM checkpoint header. Kept
 * separate from the HUML magic so a casual reader (or eval doctor) can
 * spot a PRM checkpoint by `xxd | head -1`. The actual weight tensors
 * live in HUML format (`hu_ml_checkpoint_load`) immediately after the
 * PRM header. */
#define HU_PRM_CHECKPOINT_MAGIC   0x50524D31u   /* "PRM1" */
#define HU_PRM_CHECKPOINT_VERSION 1u

/* Maximum panel size. Small intentionally — init-07 calibration tier
 * recommends 3–5 scorers; 8 is the hard ceiling enforced at construction. */
#define HU_VERIFIER_PANEL_MAX_SCORERS 8

/* Hard cap on per-call step count. The init-07 design doc uses 64 as the
 * reference budget on M3 Max; we mirror that here. */
#define HU_VERIFIER_PANEL_MAX_STEPS 64

/* Per-step score reported by the panel. `step_offset` and `step_len`
 * point into the caller-owned `chain` buffer; callers MUST keep the
 * chain alive until they consume the result. `score` and `confidence`
 * are in [0, 1]; `score` is the ensemble mean, `confidence` is the
 * agreement across scorers (`1 - normalized_variance`). */
typedef struct hu_verifier_panel_step_score {
    size_t step_offset;
    size_t step_len;
    float  score;
    float  confidence;
} hu_verifier_panel_step_score_t;

/* Result of one `score_chain` call. Owned by the caller via
 * `hu_verifier_panel_result_free`. */
typedef struct hu_verifier_panel_result {
    hu_verifier_panel_step_score_t *steps; /* allocator-owned */
    size_t                          step_count;
    float                           aggregate;            /* geometric mean of per-step scores */
    float                           aggregate_confidence; /* mean of per-step confidences */
} hu_verifier_panel_result_t;

/* One PRM scorer = one trained checkpoint loaded into RAM. Opaque to
 * callers; lifetime owned by the enclosing panel. */
typedef struct hu_prm_scorer hu_prm_scorer_t;

/* The verifier panel itself. Always present on `hu_agent_t.sota`;
 * `scorer_count == 0` means "panel is OFF" and is the default. */
typedef struct hu_verifier_panel {
    hu_allocator_t  *alloc;
    hu_prm_scorer_t *scorers; /* heap array, length = scorer_count */
    size_t           scorer_count;
    /* Telemetry — incremented by `score_chain`. Exposed for tests + the
     * future drift detector (init-07 risk #2). */
    uint64_t total_calls;
    uint64_t total_steps_scored;
} hu_verifier_panel_t;

/* Construct the panel from a list of checkpoint paths. Each path that
 * loads cleanly contributes one scorer. Paths that fail to load are
 * logged once and skipped — the surviving panel must have
 * `scorer_count > 0` for HU_OK; otherwise we return HU_ERR_NOT_SUPPORTED
 * and leave `*out` zeroed so callers can keep the legacy retry loop.
 *
 * `path_count == 0` is treated as "panel intentionally OFF" and returns
 * HU_OK with `out->scorer_count == 0`. This is the agent's default
 * boot path.
 *
 * Bounded by HU_VERIFIER_PANEL_MAX_SCORERS; extra paths are dropped
 * (logged once) rather than truncated silently. */
hu_error_t hu_verifier_panel_create(hu_allocator_t *alloc,
                                    const char *const *checkpoint_paths,
                                    size_t path_count,
                                    hu_verifier_panel_t *out);

/* Convenience: load every `*.prm` file under `dir`. Useful for the
 * common case `~/.human/models/verifier-panel/`. Returns HU_OK with
 * `out->scorer_count == 0` when the directory does not exist or has
 * no eligible files — caller should treat that as "panel OFF". */
hu_error_t hu_verifier_panel_create_from_dir(hu_allocator_t *alloc,
                                             const char *dir,
                                             hu_verifier_panel_t *out);

/* Score `chain` (UTF-8) by splitting on `\n\n` into up to `max_steps`
 * steps (capped at HU_VERIFIER_PANEL_MAX_STEPS). Each step is scored by
 * every loaded scorer; the panel reports the ensemble mean.
 *
 * Returns:
 *  - HU_OK on success; `*out` is owned by the caller (free with
 *    `hu_verifier_panel_result_free`).
 *  - HU_ERR_NOT_SUPPORTED when `panel->scorer_count == 0`.
 *  - HU_ERR_INVALID_ARGUMENT when arguments are NULL or `chain_len == 0`.
 *  - HU_ERR_OUT_OF_MEMORY when allocation fails (no partial state).
 *
 * Determinism: bitwise identical output for identical inputs. The
 * implementation uses only deterministic byte hashing and a fixed-order
 * fused multiply-add over the loaded weight buffer; no `rand()`, no
 * thread fanout. */
hu_error_t hu_verifier_panel_score_chain(hu_verifier_panel_t *panel,
                                         const char *chain,
                                         size_t chain_len,
                                         size_t max_steps,
                                         hu_verifier_panel_result_t *out);

/* Free a `score_chain` result. Safe to call with `result == NULL` or a
 * zeroed result. */
void hu_verifier_panel_result_free(hu_allocator_t *alloc,
                                   hu_verifier_panel_result_t *result);

/* Release all loaded scorer weights. Safe to call multiple times; the
 * panel is left in a `scorer_count == 0` state suitable for the next
 * `hu_verifier_panel_create` call. */
void hu_verifier_panel_deinit(hu_verifier_panel_t *panel);

/* Test/training helper — writes a deterministic PRM checkpoint with the
 * given `seed` to `path`. The file shape matches what
 * `hu_verifier_panel_create` consumes. Used by the training driver
 * (`human ml train-verifier`) and by `tests/test_think_prm.c` to pin
 * the determinism guarantee without depending on a real ML training run.
 *
 * `feature_dim` is rounded up to a multiple of 16 (the SIMD-friendly
 * stride of the scoring kernel) and capped at 4096.
 *
 * Returns HU_ERR_IO when the file cannot be opened. */
hu_error_t hu_prm_checkpoint_write_synthetic(const char *path,
                                             uint32_t seed,
                                             size_t feature_dim);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_THINK_PRM_H */
