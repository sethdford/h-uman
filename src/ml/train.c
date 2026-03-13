/* Training loop — forward pass, loss, optimizer step, evaluation. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/dataloader.h"
#include "human/ml/evaluator.h"
#include "human/ml/ml.h"
#include "human/ml/model.h"
#include "human/ml/optimizer.h"
#include "human/ml/train.h"
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__APPLE__) || defined(__linux__)
#include <time.h>
#endif

#define LN2 0.69314718055994530942

/* ─── Helpers ───────────────────────────────────────────────────────────── */

static double wall_seconds(void)
{
#if defined(__APPLE__) || (defined(__linux__) && defined(CLOCK_MONOTONIC))
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
    return 0.0;
}

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

static double cross_entropy_nats(const float *logits, size_t vocab_size, int32_t target)
{
    if ((size_t)target >= vocab_size)
        return 0.0;
    double lse = log_sum_exp(logits, vocab_size);
    return lse - (double)logits[target];
}

/* ─── Public API ─────────────────────────────────────────────────────────── */

hu_error_t hu_ml_train(hu_allocator_t *alloc, hu_model_t *model,
                       hu_ml_optimizer_t *optimizer,
                       hu_ml_dataloader_t *train_loader,
                       hu_ml_dataloader_t *val_loader,
                       const hu_training_config_t *config,
                       const int32_t *token_bytes, size_t vocab_size,
                       hu_ml_train_result_t *result)
{
    if (!alloc || !model || !optimizer || !train_loader || !config || !result)
        return HU_ERR_INVALID_ARGUMENT;

    if (!model->vtable || !model->vtable->forward)
        return HU_ERR_INVALID_ARGUMENT;

    if (!optimizer->vtable || !optimizer->vtable->step || !optimizer->vtable->zero_grad)
        return HU_ERR_INVALID_ARGUMENT;

    if (config->device_batch_size == 0 || config->time_budget_secs < 0)
        return HU_ERR_INVALID_ARGUMENT;

    if (token_bytes && vocab_size == 0)
        return HU_ERR_INVALID_ARGUMENT;

    memset(result, 0, sizeof(hu_ml_train_result_t));
    result->num_params = model->vtable->num_params(model->ctx);

    double t_start = wall_seconds();
    hu_error_t err;
    double t_budget = (double)config->time_budget_secs;
    size_t total_tokens = 0;
    size_t num_steps = 0;
    double smoothed_loss = 0.0;
    int converged = 1;

    /* Main training loop */
    for (;;) {
        hu_ml_batch_t batch = {0};
        err = hu_ml_dataloader_next(train_loader, &batch);
        if (err != HU_OK || !batch.input_ids || !batch.target_ids ||
            batch.batch_size == 0 || batch.seq_len == 0)
            break;

        size_t batch_tokens = batch.batch_size * batch.seq_len;
        size_t logits_size = batch_tokens * vocab_size * sizeof(float);

        hu_ml_tensor_t input = {
            .data = batch.input_ids,
            .shape = {batch.batch_size, batch.seq_len, 0, 0},
            .ndim = 2,
            .dtype = HU_ML_DTYPE_I32,
            .size_bytes = batch_tokens * sizeof(int32_t),
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

        float *logits_ptr = (float *)output.data;
        if (!logits_ptr) {
            hu_ml_batch_free(alloc, &batch);
            return HU_ERR_INVALID_ARGUMENT;
        }

        /* Compute cross-entropy loss (nats) */
        double batch_nats = 0.0;
        size_t n_valid = 0;
        for (size_t i = 0; i < batch_tokens; i++) {
            int32_t target = batch.target_ids[i];
            if (target < 0 || (size_t)target >= vocab_size)
                continue;
            if (token_bytes && token_bytes[target] == 0)
                continue;
            batch_nats += cross_entropy_nats(&logits_ptr[i * vocab_size], vocab_size, target);
            n_valid++;
        }

        double batch_loss = (n_valid > 0) ? (batch_nats / (double)n_valid) : 0.0;

        /* Check for NaN / divergence */
        if (batch_loss != batch_loss || batch_loss > 1e6) {
            converged = 0;
            alloc->free(alloc->ctx, logits_ptr, logits_size);
            hu_ml_batch_free(alloc, &batch);
            break;
        }

        /* Smoothed loss */
        if (num_steps == 0)
            smoothed_loss = batch_loss;
        else
            smoothed_loss = 0.99 * smoothed_loss + 0.01 * batch_loss;

        /* Backward stub: zero grads (no actual backward pass yet) */
        optimizer->vtable->zero_grad(optimizer->ctx);

        /* Optimizer step */
        err = optimizer->vtable->step(optimizer->ctx, NULL, NULL, 0);
        if (err != HU_OK) {
            alloc->free(alloc->ctx, logits_ptr, logits_size);
            hu_ml_batch_free(alloc, &batch);
            return err;
        }

        total_tokens += batch_tokens;
        num_steps++;

        alloc->free(alloc->ctx, logits_ptr, logits_size);
        hu_ml_batch_free(alloc, &batch);

        /* Time budget check */
        double elapsed = wall_seconds() - t_start;
        if (t_budget > 0.0 && elapsed >= t_budget)
            break;
    }

    double t_train_end = wall_seconds();
    result->training_seconds = t_train_end - t_start;
    result->total_tokens = total_tokens;
    result->num_steps = num_steps;
    result->converged = converged;
    result->peak_memory_mb = 0.0;
    result->mfu_percent = 0.0;

    /* Evaluation on val_loader if available */
    if (val_loader && token_bytes && config->eval_tokens > 0) {
        hu_ml_eval_result_t eval_result = {0};
        err = hu_ml_evaluate_bpb(alloc, model, val_loader, token_bytes, vocab_size,
                                 config->eval_tokens, &eval_result);
        if (err == HU_OK)
            result->val_bpb = eval_result.val_bpb;
    }

    result->total_seconds = wall_seconds() - t_start;
    return HU_OK;
}
