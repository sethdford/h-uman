/* include/human/ml/value_head.h — Phase 3 Task 1
 *
 * Linear value head: score = W . h + b, where W is shape [hidden_dim, 1]
 * (stored as float[hidden_dim]) and b is scalar.
 *
 * The value head is the only weight matrix learned on top of a frozen
 * backbone in Phase 3's reward-model composition (hu_reward_model_t,
 * Task 2) and Bradley-Terry RM training loop (Task 3). Forward and
 * backward use double accumulators internally for numerical stability;
 * persistent weights are float to match the rest of the ML subsystem
 * (HU_ML_DTYPE_F32 per include/human/ml/ml.h, hu_lora_layer_t's float A,
 * float B per include/human/ml/lora.h).
 *
 * Forward:  score = sum_i W[i] * h[i] + b
 * Backward: given dL_dscore, output
 *           dW[i] = h[i]   * dL_dscore
 *           db    =          dL_dscore
 *           dh[i] = W[i]   * dL_dscore
 *
 * Save format ("VHED" magic, little-endian / platform-native — same
 * convention as hu_lora_save in src/ml/lora.c:285-326):
 *   bytes [0..3]              : magic "VHED"
 *   bytes [4..7]              : uint32 hidden_dim
 *   bytes [8..8+4*hidden_dim) : float[hidden_dim] W
 *   bytes [...]               : float b
 */
#ifndef HU_ML_VALUE_HEAD_H
#define HU_ML_VALUE_HEAD_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float *W;          /* [hidden_dim] */
    float b;
    size_t hidden_dim;
} hu_value_head_t;

/* Construct with Xavier-Glorot initialization (stddev = sqrt(2 / hidden_dim)).
 * `out` is fully populated on success; on failure `out->W` is NULL and the
 * caller does not need to call deinit. */
hu_error_t hu_value_head_create(hu_allocator_t *alloc, size_t hidden_dim,
                                hu_value_head_t *out);

/* Forward: score = W . h + b. h is float[hidden_dim]. */
hu_error_t hu_value_head_forward(const hu_value_head_t *vh, const float *h,
                                 double *out_score);

/* Backward: given dL/dscore, compute dL/dW (out_dW: float[hidden_dim]),
 * dL/db (out_db: float*), dL/dh (out_dh: float[hidden_dim]).
 * Any of out_dW / out_db / out_dh may be NULL to skip that component. */
hu_error_t hu_value_head_backward(const hu_value_head_t *vh, const float *h,
                                  double dL_dscore,
                                  float *out_dW, float *out_db, float *out_dh);

/* Save / load: "VHED" magic + uint32 hidden_dim + float[hidden_dim] W + float b. */
hu_error_t hu_value_head_save(const hu_value_head_t *vh, const char *path);
hu_error_t hu_value_head_load(hu_allocator_t *alloc, const char *path,
                              hu_value_head_t *out);

void hu_value_head_deinit(hu_value_head_t *vh, hu_allocator_t *alloc);

#ifdef __cplusplus
}
#endif

#endif /* HU_ML_VALUE_HEAD_H */
