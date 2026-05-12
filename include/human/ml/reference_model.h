/* include/human/ml/reference_model.h
 *
 * Phase 2 Task 3: builds the frozen reference policy π_ref by cloning an
 * existing GPT model's parameters into a fresh model. Used by hu_dpo_real_huml
 * (Task 4) to anchor the policy-vs-reference log-ratio in the DPO loss.
 *
 * Plan deviation note: the canonical plan snippet (lines 796-825) shows
 * `#include "human/allocator.h"` / `"human/error.h"`. Those headers do not
 * exist in this repo — the real paths are `human/core/allocator.h` and
 * `human/core/error.h`. Same correction as Tasks 1 + 2.
 */
#ifndef HUMAN_ML_REFERENCE_MODEL_H
#define HUMAN_ML_REFERENCE_MODEL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/model.h"
#include "human/ml/ml.h"  /* hu_gpt_config_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Build a frozen reference model π_ref by:
 *  1. Creating a fresh GPT with the same config as `base`
 *  2. Enumerating base's params via base.vtable->get_params
 *  3. Deep-copying float buffers into the new model's matching params
 *  4. NOT registering the new model with any optimizer (caller's discipline)
 *
 * The returned hu_model_t is functionally identical to base at clone time.
 * Subsequent mutations to base do NOT propagate to the reference. */
hu_error_t hu_reference_model_create_from(hu_allocator_t *alloc,
                                           hu_model_t *base,
                                           const hu_gpt_config_t *config,
                                           hu_model_t *out);

#ifdef __cplusplus
}
#endif
#endif /* HUMAN_ML_REFERENCE_MODEL_H */
