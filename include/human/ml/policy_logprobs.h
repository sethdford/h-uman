/* include/human/ml/policy_logprobs.h
 *
 * Phase 2 Task 2: teacher-forced log π(y|x) on hu_model_t. Builds the
 * concatenated [prompt, response] sequence, runs forward through the
 * model's vtable to get logits at every position, and sums log-softmax
 * values at the response token positions. Used by the HUML DPO backend
 * (Task 4) and, paired with reference_model (Task 3), the log π_ref
 * counterparts that drive the DPO loss.
 *
 * Plan deviation note: the canonical plan snippet (lines 596-626) shows
 * `#include "human/allocator.h"` / `"human/error.h"`. Those headers do
 * not exist in this repo — the real paths are `human/core/allocator.h`
 * and `human/core/error.h`. Using the real paths so this header
 * compiles. (The same correction was applied to Task 1's rl_trainer.h.)
 */
#ifndef HUMAN_ML_POLICY_LOGPROBS_H
#define HUMAN_ML_POLICY_LOGPROBS_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/model.h"  /* hu_model_t */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compute log π(response | prompt) by teacher-forced forward + log-softmax
 * sum at the response token positions. Concatenates [prompt, response] into
 * one token sequence, runs forward to get logits at every position, then
 * sums log-softmax(logits[i])[response[i - len(prompt)]] for i in
 * [len(prompt), len(prompt)+len(response)).
 *
 * NOT thread-safe (uses model's internal forward buffers). */
hu_error_t hu_policy_logprobs(hu_allocator_t *alloc, hu_model_t *model,
                               const int32_t *prompt, size_t prompt_len,
                               const int32_t *response, size_t response_len,
                               double *out_logprob);

#ifdef __cplusplus
}
#endif
#endif /* HUMAN_ML_POLICY_LOGPROBS_H */
