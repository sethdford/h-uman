/* Training loop — forward, loss gradient, backward, optimizer step, evaluation. */

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

static double wall_seconds(void)
{
#if defined(__APPLE__) || (defined(__linux__) && defined(CLOCK_MONOTONIC))
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
    return 0.0;
}

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
    if (!model->vtable || !model->vtable->forward || !model->vtable->backward)
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
    double t_budget = (double)config->time_budget_secs;
    size_t total_tokens = 0, num_steps = 0;
    double smoothed_loss = 0.0;
    int converged = 1;

    size_t accum_steps = config->grad_accum_steps > 0 ? config->grad_accum_steps : 1;
    size_t micro_step = 0;
    float accum_scale = 1.0f / (float)accum_steps;

    optimizer->vtable->zero_grad(optimizer->ctx);

    for (;;) {
        hu_ml_batch_t batch = {0};
        hu_error_t err = hu_ml_dataloader_next(train_loader, &batch);
        if (err != HU_OK || !batch.input_ids || !batch.target_ids ||
            batch.batch_size == 0 || batch.seq_len == 0)
            break;

        size_t batch_tokens = batch.batch_size * batch.seq_len;
        size_t logits_size = batch_tokens * vocab_size * sizeof(float);

        hu_ml_tensor_t input = {
            .data = batch.input_ids,
            .shape = {batch.batch_size, batch.seq_len, 0, 0},
            .ndim = 2, .dtype = HU_ML_DTYPE_I32,
            .size_bytes = batch_tokens * sizeof(int32_t),
        };
        hu_ml_tensor_t output = {0};

        err = model->vtable->forward(model->ctx, &input, &output);
        if (err != HU_OK) { hu_ml_batch_free(alloc, &batch); return err; }

        float *logits = (float *)output.data;
        if (!logits) { hu_ml_batch_free(alloc, &batch); return HU_ERR_INVALID_ARGUMENT; }

        /* Compute cross-entropy loss and d_logits (softmax - onehot) */
        float *d_logits = (float *)alloc->alloc(alloc->ctx, logits_size);
        if (!d_logits) {
            alloc->free(alloc->ctx, logits, logits_size);
            hu_ml_batch_free(alloc, &batch);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memset(d_logits, 0, logits_size);

        double batch_nats = 0.0;
        size_t n_valid = 0;
        for (size_t i = 0; i < batch_tokens; i++) {
            int32_t target = batch.target_ids[i];
            if (target < 0 || (size_t)target >= vocab_size) continue;
            if (token_bytes && token_bytes[target] == 0) continue;

            float *li = logits + i * vocab_size;
            float *di = d_logits + i * vocab_size;

            /* Softmax + cross-entropy in one pass */
            float mx = li[0];
            for (size_t k = 1; k < vocab_size; k++) if (li[k] > mx) mx = li[k];
            double sum = 0.0;
            for (size_t k = 0; k < vocab_size; k++) { di[k] = expf(li[k] - mx); sum += di[k]; }
            for (size_t k = 0; k < vocab_size; k++) di[k] /= (float)sum;

            batch_nats += -log((double)di[target]);
            di[target] -= 1.0f;

            /* Scale by 1/n_valid and accumulation factor */
            n_valid++;
        }

        double batch_loss = (n_valid > 0) ? (batch_nats / (double)n_valid) : 0.0;
        if (batch_loss != batch_loss || batch_loss > 1e6) {
            converged = 0;
            alloc->free(alloc->ctx, d_logits, logits_size);
            alloc->free(alloc->ctx, logits, logits_size);
            hu_ml_batch_free(alloc, &batch);
            break;
        }

        /* Scale gradients */
        if (n_valid > 0) {
            float grad_scale = accum_scale / (float)n_valid;
            for (size_t i = 0; i < batch_tokens * vocab_size; i++)
                d_logits[i] *= grad_scale;
        }

        if (num_steps == 0) smoothed_loss = batch_loss;
        else smoothed_loss = 0.99 * smoothed_loss + 0.01 * batch_loss;

        /* Backward pass — fills gradient buffers */
        hu_ml_tensor_t grad_tensor = {
            .data = d_logits,
            .shape = {batch.batch_size, batch.seq_len, vocab_size, 0},
            .ndim = 3, .dtype = HU_ML_DTYPE_F32,
            .size_bytes = logits_size,
        };
        err = model->vtable->backward(model->ctx, &grad_tensor);
        alloc->free(alloc->ctx, d_logits, logits_size);
        alloc->free(alloc->ctx, logits, logits_size);
        hu_ml_batch_free(alloc, &batch);
        if (err != HU_OK) return err;

        total_tokens += batch_tokens;
        micro_step++;

        /* Optimizer step after accumulation */
        if (micro_step >= accum_steps) {
            /* Apply LR schedule based on time progress */
            if (t_budget > 0.0 && optimizer->vtable->set_lr_multiplier) {
                double elapsed = wall_seconds() - t_start;
                float progress = (float)(elapsed / t_budget);
                if (progress > 1.0f) progress = 1.0f;
                float mult = hu_ml_lr_schedule(progress,
                    config->warmup_ratio, config->warmdown_ratio,
                    config->final_lr_frac);
                optimizer->vtable->set_lr_multiplier(optimizer->ctx, mult);
            }

            err = optimizer->vtable->step(optimizer->ctx, NULL, NULL, 0);
            if (err != HU_OK) return err;
            optimizer->vtable->zero_grad(optimizer->ctx);
            micro_step = 0;
            num_steps++;
        }

        double elapsed = wall_seconds() - t_start;
        if (t_budget > 0.0 && elapsed >= t_budget) break;
    }

    /* Flush remaining micro-steps */
    if (micro_step > 0) {
        optimizer->vtable->step(optimizer->ctx, NULL, NULL, 0);
        optimizer->vtable->zero_grad(optimizer->ctx);
        num_steps++;
    }

    result->training_seconds = wall_seconds() - t_start;
    result->total_tokens = total_tokens;
    result->num_steps = num_steps;
    result->converged = converged;

    if (val_loader && token_bytes && config->eval_tokens > 0) {
        hu_ml_eval_result_t eval_result = {0};
        hu_error_t err = hu_ml_evaluate_bpb(alloc, model, val_loader, token_bytes, vocab_size,
                                             config->eval_tokens, &eval_result);
        if (err == HU_OK) result->val_bpb = eval_result.val_bpb;
    }

    result->total_seconds = wall_seconds() - t_start;
    return HU_OK;
}
