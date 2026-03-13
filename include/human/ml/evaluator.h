#ifndef HU_ML_EVALUATOR_H
#define HU_ML_EVALUATOR_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/dataloader.h"
#include "human/ml/model.h"
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * BPB evaluation interface
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_ml_eval_result {
    double val_bpb;
    double total_nats;
    size_t total_bytes;
    size_t eval_tokens;
} hu_ml_eval_result_t;

hu_error_t hu_ml_evaluate_bpb(hu_allocator_t *alloc, hu_model_t *model,
                             hu_ml_dataloader_t *val_loader,
                             const int32_t *token_bytes, size_t vocab_size,
                             size_t eval_tokens, hu_ml_eval_result_t *result);

#endif
