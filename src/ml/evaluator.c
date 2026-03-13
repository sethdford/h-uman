/* BPB (bits per byte) evaluation — core metric from autoresearch. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/dataloader.h"
#include "human/ml/evaluator.h"
#include "human/ml/model.h"
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#define LN2 0.69314718055994530942

/* ─── Helpers ───────────────────────────────────────────────────────────── */

/* Log-sum-exp for numerical stability when computing cross-entropy. */
static double log_sum_exp(const float *logits, size_t vocab_size)
{
    float max_val = logits[0];
    for (size_t k = 1; k < vocab_size; k++)
        if (logits[k] > max_val)
            max_val = logits[k];

    double sum = 0.0;
    for (size_t k = 0; k < vocab_size; k++)
        sum += expf((double)(logits[k] - max_val));

    return (double)max_val + log(sum);
}

/* Per-token cross-entropy in nats: -log(p_target) = log_sum_exp - logits[target]. */
static double cross_entropy_nats(const float *logits, size_t vocab_size, int32_t target)
{
    if ((size_t)target >= vocab_size)
        return 0.0; /* invalid target, skip */
    double lse = log_sum_exp(logits, vocab_size);
    return lse - (double)logits[target];
}

/* ─── Public API ─────────────────────────────────────────────────────────── */

hu_error_t hu_ml_evaluate_bpb(hu_allocator_t *alloc, hu_model_t *model,
                             hu_ml_dataloader_t *val_loader,
                             const int32_t *token_bytes, size_t vocab_size,
                             size_t eval_tokens, hu_ml_eval_result_t *result)
{
    if (!alloc || !model || !val_loader || !token_bytes || !result)
        return HU_ERR_INVALID_ARGUMENT;

    if (vocab_size == 0 || eval_tokens == 0)
        return HU_ERR_INVALID_ARGUMENT;

    if (!model->vtable || !model->vtable->forward)
        return HU_ERR_INVALID_ARGUMENT;

    result->val_bpb = 0.0;
    result->total_nats = 0.0;
    result->total_bytes = 0;
    result->eval_tokens = 0;

    hu_ml_batch_t batch = {0};
    size_t tokens_consumed = 0;

    while (tokens_consumed < eval_tokens) {
        hu_error_t err = hu_ml_dataloader_next(val_loader, &batch);
        if (err != HU_OK)
            break;
        if (!batch.input_ids || !batch.target_ids || batch.batch_size == 0 || batch.seq_len == 0)
            break;

        size_t full_batch_tokens = batch.batch_size * batch.seq_len;
        size_t batch_tokens = full_batch_tokens;
        if (tokens_consumed + batch_tokens > eval_tokens)
            batch_tokens = eval_tokens - tokens_consumed;

        size_t logits_size = full_batch_tokens * vocab_size * sizeof(float);

        hu_ml_tensor_t input = {
            .data = batch.input_ids,
            .shape = {batch.batch_size, batch.seq_len, 0, 0},
            .ndim = 2,
            .dtype = HU_ML_DTYPE_I32,
            .size_bytes = full_batch_tokens * sizeof(int32_t),
        };

        hu_ml_tensor_t output = {
            .data = NULL,
            .shape = {batch.batch_size, batch.seq_len, vocab_size, 0},
            .ndim = 3,
            .dtype = HU_ML_DTYPE_F32,
            .size_bytes = logits_size,
        };

        err = model->vtable->forward(model->ctx, &input, &output);
        if (err != HU_OK) {
            hu_ml_batch_free(alloc, &batch);
            return err;
        }

        float *logits = (float *)output.data;
        if (!logits) {
            hu_ml_batch_free(alloc, &batch);
            return HU_ERR_INVALID_ARGUMENT;
        }

        for (size_t i = 0; i < batch_tokens; i++) {
            int32_t target = batch.target_ids[i];
            int32_t bytes = (target >= 0 && (size_t)target < vocab_size)
                                ? token_bytes[target]
                                : 0;
            if (bytes == 0)
                continue;

            double ce = cross_entropy_nats(&logits[i * vocab_size], vocab_size, target);
            result->total_nats += ce;
            result->total_bytes += (size_t)bytes;
        }

        result->eval_tokens += batch_tokens;
        tokens_consumed += batch_tokens;

        alloc->free(alloc->ctx, logits, logits_size);
        hu_ml_batch_free(alloc, &batch);
    }

    if (result->total_bytes > 0)
        result->val_bpb = result->total_nats / (LN2 * (double)result->total_bytes);

    return HU_OK;
}
