#ifndef HU_ML_LORA_H
#define HU_ML_LORA_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/ml.h"
#include "human/ml/model.h"
#include "human/ml/optimizer.h"
#include <stddef.h>

typedef enum hu_lora_target {
    HU_LORA_TARGET_Q    = 1 << 0,
    HU_LORA_TARGET_K    = 1 << 1,
    HU_LORA_TARGET_V    = 1 << 2,
    HU_LORA_TARGET_O    = 1 << 3,
    HU_LORA_TARGET_UP   = 1 << 4,
    HU_LORA_TARGET_DOWN = 1 << 5,
    HU_LORA_TARGET_QV   = HU_LORA_TARGET_Q | HU_LORA_TARGET_V,
    HU_LORA_TARGET_ALL  = 0x3F,
} hu_lora_target_t;

typedef struct hu_lora_config {
    size_t rank;
    float alpha;
    float dropout;
    hu_lora_target_t targets;
} hu_lora_config_t;

typedef struct hu_lora_layer {
    float *A;         /* [rank x in_dim] */
    float *B;         /* [out_dim x rank] */
    float *grad_A;
    float *grad_B;
    size_t in_dim;
    size_t out_dim;
    size_t rank;
    float scale;      /* alpha / rank */
} hu_lora_layer_t;

typedef struct hu_lora_adapter hu_lora_adapter_t;

hu_error_t hu_lora_create(hu_allocator_t *alloc, const hu_lora_config_t *config,
                           size_t in_dim, size_t out_dim, size_t n_layers,
                           hu_lora_adapter_t **out);
void hu_lora_destroy(hu_allocator_t *alloc, hu_lora_adapter_t *adapter);

hu_error_t hu_lora_apply(const hu_lora_adapter_t *adapter, size_t layer_idx,
                          const float *input, size_t batch_tokens,
                          float *output);

hu_error_t hu_lora_backward(hu_lora_adapter_t *adapter, size_t layer_idx,
                             const float *input, const float *grad_output,
                             size_t batch_tokens, float *grad_input);

hu_error_t hu_lora_register_params(hu_lora_adapter_t *adapter, hu_ml_optimizer_t *opt);
size_t hu_lora_num_params(const hu_lora_adapter_t *adapter);

/* Adapter shape — used by callers (e.g. providers) to validate compatibility
 * with a target model before calling hu_gpt_attach_lora. Returns 0s when
 * adapter is NULL. Each pointer is optional. */
void hu_lora_get_dims(const hu_lora_adapter_t *adapter, size_t *out_in_dim,
                      size_t *out_out_dim, size_t *out_n_layers);

hu_error_t hu_lora_save(const hu_lora_adapter_t *adapter, const char *path);
hu_error_t hu_lora_load(hu_allocator_t *alloc, const char *path, hu_lora_adapter_t **out);

/* Test/debug: set layer weights (A, B) for known-value testing */
hu_error_t hu_lora_set_layer_weights(hu_lora_adapter_t *adapter, size_t layer_idx,
                                     const float *A, const float *B);

#endif
