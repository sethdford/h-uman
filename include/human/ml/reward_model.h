/* include/human/ml/reward_model.h — Phase 3 Task 2
 *
 * Vtable-based reward model: scalar score over a (prompt, response) pair.
 *
 * Composition surface for Phase 3:
 *   HUML backend (this task): toy GPT backbone (cross-platform, gradient-
 *     checkable) producing last-position logits, fed into Task 1's
 *     hu_value_head_t for the scalar score. Scope (R4): validate the
 *     linear projection + backbone forward composition end-to-end, NOT
 *     semantic plausibility — that lives on the MLX-Qwen path (Task 8).
 *   MLX backend (Task 8): GGUF-backed Qwen (or compatible) backbone via
 *     scripts/rm_mlx_train.py; the C side calls into the Python subprocess
 *     the same way Phase 2 Task 6 wires hu_dpo_real_mlx. This task only
 *     stubs the factory at HU_ERR_NOT_SUPPORTED.
 *
 * Training (Task 3) lives in src/ml/reward_model_train.c. It reads the
 * private layout via src/ml/reward_model_priv.h (H2 fix — see plan
 * §H2 for why this is the chosen access pattern over HU_IS_TEST
 * accessors). The public API here is stable; the private header is
 * internal and never installed.
 */
#ifndef HU_ML_REWARD_MODEL_H
#define HU_ML_REWARD_MODEL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/dpo.h"        /* hu_preference_pair_t */
#include "human/ml/model.h"      /* hu_model_t */
#include "human/ml/value_head.h" /* hu_value_head_t */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HU_REWARD_MODEL_BACKEND_HUML = 1,
    HU_REWARD_MODEL_BACKEND_MLX  = 2,
} hu_reward_model_backend_t;

typedef struct {
    hu_reward_model_backend_t backend;
    size_t vocab_size;          /* HUML toy GPT vocab; MLX: ignored */
    size_t hidden_dim;          /* HUML: == vocab_size (last-position logits);
                                 * MLX: real Qwen hidden_dim */
    const char *backbone_path;  /* MLX: GGUF path; HUML: ignored */
    const char *value_head_path;/* Optional pre-trained value head; if NULL,
                                 * fresh Xavier init via hu_value_head_create. */
} hu_reward_model_config_t;

typedef struct hu_reward_model_vtable {
    /* score: scalar for a single (prompt, response). Both must be non-empty;
     * NULL / zero-length returns HU_ERR_INVALID_ARGUMENT.
     *
     * For HUML: tokenize "prompt response" as space-separated ints, run the
     * toy GPT forward, take the last-position [vocab_size]-vector of logits
     * as the hidden state, and project through hu_value_head_forward. */
    hu_error_t (*score)(void *ctx, hu_allocator_t *alloc,
                        const char *prompt, size_t prompt_len,
                        const char *response, size_t response_len,
                        double *out_score);
    /* score_batch: scores chosen and rejected for each pair.
     *
     * M3 contract for one-sided KTO pairs (chosen_len == 0 OR rejected_len == 0):
     * the corresponding output slot is set to NaN (NAN macro from <math.h>) and
     * the populated side is scored normally. Caller MUST filter NaN before any
     * Bradley-Terry training step. Mixed KTO + DPO arrays are safe to pass
     * without separate filtering. */
    hu_error_t (*score_batch)(void *ctx, hu_allocator_t *alloc,
                              const hu_preference_pair_t *pairs, size_t n,
                              double *out_chosen_scores,
                              double *out_rejected_scores);
    const char *(*name)(void *ctx);
    void (*deinit)(void *ctx, hu_allocator_t *alloc);
} hu_reward_model_vtable_t;

typedef struct {
    void *ctx;
    const hu_reward_model_vtable_t *vtable;
} hu_reward_model_t;

/* HUML factory: toy GPT backbone + Task 1's hu_value_head_t on last-position
 * logits. Cross-platform, gradient-checkable, scope-limited to validating
 * the linear projection + forward composition (see R4). Requires
 * config->backend == HU_REWARD_MODEL_BACKEND_HUML and
 * config->vocab_size == config->hidden_dim (because the HUML hidden state IS
 * the last-position logits vector; see plan §D3). */
hu_error_t hu_reward_model_create_huml(hu_allocator_t *alloc,
                                        const hu_reward_model_config_t *config,
                                        hu_reward_model_t *out);

/* MLX factory: returns HU_ERR_NOT_SUPPORTED in Task 2; Task 8 fills it in
 * by wiring scripts/rm_mlx_train.py for real Qwen-scale RM training. */
hu_error_t hu_reward_model_create_mlx(hu_allocator_t *alloc,
                                       const hu_reward_model_config_t *config,
                                       hu_reward_model_t *out);

/* Save / load — composes hu_value_head_save + backbone serialization.
 * Not implemented in Task 2; Task 9 lands the format pin. */
hu_error_t hu_reward_model_save(const hu_reward_model_t *rm, const char *dir);
hu_error_t hu_reward_model_load(hu_allocator_t *alloc, const char *dir,
                                 hu_reward_model_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HU_ML_REWARD_MODEL_H */
