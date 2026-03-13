#ifndef HU_ML_OPTIMIZER_H
#define HU_ML_OPTIMIZER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/ml.h"
#include "human/ml/model.h"
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Optimizer vtable interface
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_ml_optimizer_vtable hu_ml_optimizer_vtable_t;

typedef struct hu_ml_optimizer {
    void *ctx;
    const hu_ml_optimizer_vtable_t *vtable;
} hu_ml_optimizer_t;

typedef struct hu_ml_optimizer_vtable {
    hu_error_t (*step)(void *ctx, hu_ml_tensor_t *params,
                       const hu_ml_tensor_t *grads, size_t count);
    void (*zero_grad)(void *ctx);
    void (*set_lr_multiplier)(void *ctx, float multiplier);
    void (*deinit)(void *ctx, hu_allocator_t *alloc);
} hu_ml_optimizer_vtable_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Param kinds and MuonAdamW
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum hu_param_kind {
    HU_PARAM_EMBEDDING,
    HU_PARAM_UNEMBEDDING,
    HU_PARAM_MATRIX,
    HU_PARAM_SCALAR,
} hu_param_kind_t;

/* LR schedule helper (warmup / constant / warmdown). */
float hu_ml_lr_schedule(float progress, float warmup_ratio, float warmdown_ratio,
                        float final_lr_frac);

hu_error_t hu_muon_adamw_create(hu_allocator_t *alloc,
                                const hu_optimizer_config_t *config,
                                hu_ml_optimizer_t *out);

hu_error_t hu_muon_adamw_add_param(hu_ml_optimizer_t *opt, float *param,
                                   float *grad, size_t rows, size_t cols,
                                   hu_param_kind_t kind);

#endif
