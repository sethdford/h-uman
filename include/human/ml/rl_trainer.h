/* US-7.10 — RL trainer vtable (Init #06 phase 1).
 *
 * Minimal 3-member vtable behind which SimPO, ORPO, and GRPO-2 live.
 * Sprint 7 lands only the vtable surface and one loss head (SimPO);
 * ORPO and GRPO-2 are stubbed at the CLI router level and return
 * exit code 2 with a "not yet implemented" message.
 *
 * The shape of the vtable is the contract from AC-7.10.1: exactly
 * three function pointers (`compute_loss`, `train_step`, `deinit`) and
 * the `hu_rl_trainer_type_t` enum. Future stories may widen the vtable
 * (e.g. add `prepare`, `save_adapter`) without breaking AC-7.10.1 —
 * the AC asserts only that those three exist, not that they're the
 * only ones.
 *
 * Design doc: `sprints/sprint-7/designs/US-7.10.md`.
 */
#ifndef HU_ML_RL_TRAINER_H
#define HU_ML_RL_TRAINER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/dpo.h"   /* hu_preference_pair_t */
#include "human/ml/model.h" /* hu_model_t */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Trainer type tags. Values are stable wire-level identifiers; do not
 * renumber without bumping a major version. */
typedef enum {
    HU_RL_TRAINER_DPO = 0,
    HU_RL_TRAINER_SIMPO = 1,
    HU_RL_TRAINER_ORPO = 2,
    HU_RL_TRAINER_GRPO2 = 3,
} hu_rl_trainer_type_t;

/* Logprob-injection seam for the golden test (AC-7.10.2) and for any
 * future caller that already holds tokenized logprobs (e.g. an MLX
 * bridge). The fields are `double` so the analytical fixture can be
 * computed in float64 in Python or by hand against the canonical
 * SimPO formula and matched in C without precision-loss flake. */
typedef struct hu_pref_pair_logprobs {
    double chosen_logprob_sum;
    size_t chosen_token_count;
    double rejected_logprob_sum;
    size_t rejected_token_count;
} hu_pref_pair_logprobs_t;

/* Vtable — exactly the three members named by AC-7.10.1. */
typedef struct hu_rl_trainer_vtable {
    /* Compute the loss for a pre-tokenized pair whose logprobs are
     * already known. Pure-double precision; no model forward pass. */
    hu_error_t (*compute_loss)(void *ctx, const hu_pref_pair_logprobs_t *lp, double *out_loss);

    /* End-to-end step: run the bound model's forward pass on
     * (prompt+chosen) and (prompt+rejected), sum + length-normalize
     * logprobs, hand off to `compute_loss`. v1 has no backward pass;
     * a later story will add one. In `HU_IS_TEST` builds the
     * implementation may accept a NULL model and emit a fixed mock
     * loss so CLI tests don't need to boot a real GPT. */
    hu_error_t (*train_step)(void *ctx, const hu_preference_pair_t *pair, double *out_loss);

    /* Release the head's internal state. Must be safe to call once.
     * The owning `hu_rl_trainer_deinit` helper additionally guards
     * against double-deinit by NULLing the vtable pointer. */
    void (*deinit)(void *ctx);
} hu_rl_trainer_vtable_t;

typedef struct hu_rl_trainer {
    void *ctx;
    const hu_rl_trainer_vtable_t *vtable;
    hu_rl_trainer_type_t type;
} hu_rl_trainer_t;

/* SimPO factory config. β and γ are the algorithm hyperparameters
 * (defaults in `sprints/sprint-7/designs/US-7.10.md`: β=0.1, γ=0.5).
 * `model` is nullable; required only when `train_step` is called
 * outside of `HU_IS_TEST`. */
typedef struct hu_simpo_config {
    float beta;
    float gamma;
    hu_model_t *model;
} hu_simpo_config_t;

/* AC-7.10.1 factory name (per the story; `hu_<module>_<algo>_<action>` is
 * a documented variant of the global naming standard for vtable-family
 * factories — see `src/ml/CLAUDE.md`). */
hu_error_t hu_rl_trainer_simpo_create(hu_allocator_t *alloc, const hu_simpo_config_t *cfg,
                                      hu_rl_trainer_t *out);

/* Idempotent: calling twice is a no-op. After deinit `trainer->vtable`
 * is NULL and `trainer->ctx` is NULL. */
void hu_rl_trainer_deinit(hu_rl_trainer_t *trainer);

/* Human-readable name (`"dpo"`, `"simpo"`, `"orpo"`, `"grpo2"`, or
 * `"unknown"`). Returned pointer points to static storage. */
const char *hu_rl_trainer_type_name(hu_rl_trainer_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* HU_ML_RL_TRAINER_H */
