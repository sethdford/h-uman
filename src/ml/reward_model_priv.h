/* src/ml/reward_model_priv.h — Private context for HUML reward model
 *
 * Internal header (not installed). Shared between reward_model.c,
 * reward_model_train.c and other training-related modules. Opaque to callers.
 */
#ifndef HU_ML_REWARD_MODEL_PRIV_H
#define HU_ML_REWARD_MODEL_PRIV_H

#include "human/core/allocator.h"
#include "human/ml/ml.h"
#include "human/ml/model.h"
#include "human/ml/value_head.h"
#include <stddef.h>

typedef struct {
    hu_allocator_t *alloc;
    hu_model_t backbone;        /* Frozen GPT backbone */
    hu_value_head_t value_head; /* Trainable value head */
    hu_gpt_config_t gpt_cfg;    /* Config used to create backbone */
    size_t vocab_size;
    size_t hidden_dim;
} huml_rm_ctx_t;

#include "human/ml/reward_model.h" /* hu_reward_model_t, hu_preference_pair_t */

/* Score (prompt, response) AND return the last-position hidden vector that the
 * value head consumes — the [hidden_dim] logits row. `out_h` must be a
 * caller-allocated float[hidden_dim] (may be NULL to skip). Enables analytical
 * backprop (hu_value_head_backward) in the training loop instead of O(hidden_dim)
 * finite-difference forward passes per step. */
hu_error_t reward_model_huml_score_hidden(hu_reward_model_t *rm, hu_allocator_t *alloc,
                                          const char *prompt, size_t prompt_len,
                                          const char *response, size_t response_len,
                                          double *out_score, float *out_h);

/* Test helper: analytical gradient of the mean Bradley-Terry batch loss w.r.t.
 * the value-head weights. `out_dW` is caller-allocated float[hidden_dim]; both
 * out_dW and out_db are averaged over valid (two-sided) pairs to match
 * reward_model_compute_bt_loss_only_for_test. Used by the AC-101.4 gradient
 * check to compare against central finite differences. */
hu_error_t reward_model_compute_bt_grad_for_test(hu_reward_model_t *rm, hu_allocator_t *alloc,
                                                 const hu_preference_pair_t *pairs, size_t n,
                                                 float *out_dW, double *out_db);

#endif /* HU_ML_REWARD_MODEL_PRIV_H */
