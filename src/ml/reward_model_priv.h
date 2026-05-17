/* src/ml/reward_model_priv.h — Phase 3 Task 2 (PRIVATE)
 *
 * Internal layout for the HUML-backend hu_reward_model_t. NOT part of the
 * public API and never installed in include/human/ml/. Included by:
 *   - src/ml/reward_model.c (defines the layout, exports the cast helper)
 *   - src/ml/reward_model_train.c (Task 3; takes the SGD step on
 *     ctx->value_head against the policy-frozen backbone).
 *
 * Why this is private, not HU_IS_TEST (plan §H2): the value head is the
 * only trainable surface for HUML RMs in Phase 3, so its weights must be
 * mutable from the training translation unit. Exposing the layout via a
 * private header beats the alternatives:
 *   1. A HU_IS_TEST-only accessor — would couple production-only code to
 *      a test-time #define and the training loop is NOT test-only code.
 *   2. A second public vtable method (e.g. get_value_head) — would leak an
 *      internal abstraction onto every backend (notably MLX, where the
 *      Python subprocess owns the weights and a C-side accessor cannot
 *      return them).
 *   3. Friend-style direct struct exposure in the public header — would
 *      let out-of-tree callers depend on the toy GPT layout (vocab_size,
 *      hidden_dim) that's a HUML-only implementation detail.
 * The private header is what every binding lib in this repo already does
 * for backend-specific layouts (cf. src/ml/lora.c, src/ml/gpt.c).
 */
#ifndef HU_ML_REWARD_MODEL_PRIV_H
#define HU_ML_REWARD_MODEL_PRIV_H

#include "human/ml/model.h"
#include "human/ml/reward_model.h"
#include "human/ml/value_head.h"
#include "human/ml/ml.h" /* hu_gpt_config_t */

typedef struct huml_rm_ctx {
    hu_model_t backbone;          /* toy GPT created via hu_gpt_create */
    hu_value_head_t value_head;   /* trained by Task 3 */
    hu_gpt_config_t gpt_cfg;
    size_t hidden_dim;
    size_t vocab_size;
} huml_rm_ctx_t;

/* Cast helper used by reward_model_train.c. Returns NULL if `rm` is not the
 * HUML variant (e.g. MLX). reward_model_train.c is HUML-only in Phase 3;
 * MLX training lives in scripts/rm_mlx_train.py. The vtable identity check
 * is sole gate — there is no way to forge a HUML-shaped ctx without
 * registering the HUML vtable, so the cast is safe whenever this returns
 * non-NULL. */
huml_rm_ctx_t *hu_reward_model_huml_ctx_or_null(hu_reward_model_t *rm);

#endif /* HU_ML_REWARD_MODEL_PRIV_H */
