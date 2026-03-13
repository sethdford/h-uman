/* Autonomous experiment loop — core of autoresearch ported to C.
 *
 * Each iteration: build model from config -> train -> evaluate -> keep/discard.
 * The loop explores the config space, tracking the best result.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/dataloader.h"
#include "human/ml/evaluator.h"
#include "human/ml/experiment.h"
#include "human/ml/ml.h"
#include "human/ml/model.h"
#include "human/ml/optimizer.h"
#include "human/ml/prepare.h"
#include "human/ml/tokenizer_ml.h"
#include "human/ml/train.h"
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ─── Helpers ───────────────────────────────────────────────────────────── */

static uint32_t hash_experiment_config(const hu_experiment_config_t *config)
{
    const unsigned char *p = (const unsigned char *)config;
    uint32_t h = 5381u;
    for (size_t i = 0; i < sizeof(hu_experiment_config_t); i++)
        h = ((h << 5) + h) + (unsigned)p[i];
    return h;
}

static const char *status_string(hu_experiment_status_t status)
{
    switch (status) {
    case HU_EXPERIMENT_KEEP:
        return "keep";
    case HU_EXPERIMENT_DISCARD:
        return "discard";
    case HU_EXPERIMENT_CRASH:
        return "crash";
    default:
        return "unknown";
    }
}

static double now_seconds(void)
{
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
#else
    return (double)clock() / (double)CLOCKS_PER_SEC;
#endif
}

/* Apply a mutation to the config for the next experiment. */
static void mutate_config(hu_experiment_config_t *cfg, int iteration)
{
    uint32_t seed = (uint32_t)iteration * 2654435761u;

    switch (seed % 8) {
    case 0:
        if (cfg->gpt.n_layer > 2)
            cfg->gpt.n_layer += (seed >> 8) % 2 ? 2 : -2;
        break;
    case 1: {
        float delta = ((seed >> 12) % 2) ? 1.2f : 0.8f;
        cfg->optimizer.matrix_lr *= delta;
        if (cfg->optimizer.matrix_lr < 0.001f)
            cfg->optimizer.matrix_lr = 0.001f;
        if (cfg->optimizer.matrix_lr > 0.2f)
            cfg->optimizer.matrix_lr = 0.2f;
        break;
    }
    case 2: {
        float delta = ((seed >> 16) % 2) ? 1.3f : 0.7f;
        cfg->optimizer.embedding_lr *= delta;
        if (cfg->optimizer.embedding_lr < 0.01f)
            cfg->optimizer.embedding_lr = 0.01f;
        if (cfg->optimizer.embedding_lr > 2.0f)
            cfg->optimizer.embedding_lr = 2.0f;
        break;
    }
    case 3:
        cfg->optimizer.weight_decay = ((seed >> 20) % 2) ? 0.1f : 0.3f;
        break;
    case 4:
        cfg->optimizer.warmdown_ratio = ((seed >> 24) % 3) * 0.25f;
        break;
    case 5: {
        size_t new_dim = cfg->gpt.n_embd + (((seed >> 8) % 2) ? 128 : -128);
        if (new_dim >= 128 && new_dim <= 2048) {
            cfg->gpt.n_embd = new_dim;
            cfg->gpt.n_head = new_dim / cfg->gpt.head_dim;
            cfg->gpt.n_kv_head = cfg->gpt.n_head;
        }
        break;
    }
    case 6:
        cfg->gpt.activation = (hu_ml_activation_t)((seed >> 12) % 3);
        break;
    case 7:
        cfg->optimizer.adam_beta1 = ((seed >> 16) % 2) ? 0.8f : 0.9f;
        break;
    }
}

/* Run a single experiment: create model, train, evaluate. */
static hu_error_t run_single_experiment(hu_allocator_t *alloc,
                                        const hu_experiment_config_t *cfg,
                                        const char *data_dir,
                                        hu_experiment_result_t *result)
{
    hu_error_t err;
    double t_start = now_seconds();

    memset(result, 0, sizeof(*result));
    result->config = *cfg;

    hu_model_t model = {0};
    err = hu_gpt_create(alloc, &cfg->gpt, &model);
    if (err != HU_OK) {
        result->status = HU_EXPERIMENT_CRASH;
        snprintf(result->description, sizeof(result->description),
                 "model creation failed: %d", (int)err);
        return HU_OK;
    }

    result->config.gpt.vocab_size = cfg->gpt.vocab_size;
    size_t num_params = model.vtable->num_params(model.ctx);

    hu_ml_optimizer_t optimizer = {0};
    err = hu_muon_adamw_create(alloc, &cfg->optimizer, &optimizer);
    if (err != HU_OK) {
        model.vtable->deinit(model.ctx, alloc);
        result->status = HU_EXPERIMENT_CRASH;
        snprintf(result->description, sizeof(result->description),
                 "optimizer creation failed: %d", (int)err);
        return HU_OK;
    }

    hu_ml_dataloader_t *train_dl = NULL;
    err = hu_ml_dataloader_create(alloc, data_dir, cfg->training.device_batch_size,
                                  cfg->gpt.sequence_len, "train", &train_dl);
    if (err != HU_OK) {
        optimizer.vtable->deinit(optimizer.ctx, alloc);
        model.vtable->deinit(model.ctx, alloc);
        result->status = HU_EXPERIMENT_CRASH;
        snprintf(result->description, sizeof(result->description),
                 "train dataloader failed: %d", (int)err);
        return HU_OK;
    }

    hu_ml_dataloader_t *val_dl = NULL;
    err = hu_ml_dataloader_create(alloc, data_dir, cfg->training.device_batch_size,
                                  cfg->gpt.sequence_len, "val", &val_dl);
    if (err != HU_OK) {
        hu_ml_dataloader_deinit(train_dl);
        optimizer.vtable->deinit(optimizer.ctx, alloc);
        model.vtable->deinit(model.ctx, alloc);
        result->status = HU_EXPERIMENT_CRASH;
        snprintf(result->description, sizeof(result->description),
                 "val dataloader failed: %d", (int)err);
        return HU_OK;
    }

    hu_ml_train_result_t train_result = {0};
    err = hu_ml_train(alloc, &model, &optimizer, train_dl, val_dl,
                      &cfg->training, NULL, cfg->gpt.vocab_size, &train_result);

    result->val_bpb = train_result.val_bpb;
    result->peak_memory_mb = train_result.peak_memory_mb;
    result->training_seconds = train_result.training_seconds;

    if (err != HU_OK || !train_result.converged) {
        result->status = HU_EXPERIMENT_CRASH;
        snprintf(result->description, sizeof(result->description),
                 "training failed: %d (params=%zuM)",
                 (int)err, num_params / 1000000);
    }

    hu_ml_dataloader_deinit(val_dl);
    hu_ml_dataloader_deinit(train_dl);
    optimizer.vtable->deinit(optimizer.ctx, alloc);
    model.vtable->deinit(model.ctx, alloc);

    double t_end = now_seconds();
    result->training_seconds = t_end - t_start;

    return HU_OK;
}

/* ─── Public API ─────────────────────────────────────────────────────────── */

hu_error_t hu_experiment_loop(hu_allocator_t *alloc,
                              const hu_experiment_loop_config_t *config,
                              hu_experiment_loop_callback_t callback,
                              void *user_data)
{
    if (!alloc || !config)
        return HU_ERR_INVALID_ARGUMENT;
    if (config->max_iterations <= 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (!config->data_dir)
        return HU_ERR_INVALID_ARGUMENT;

    hu_experiment_config_t best_config = config->base_config;
    double best_bpb = 1e9;
    int best_iter = -1;

    hu_experiment_config_t current_config = config->base_config;

    for (int i = 0; i < config->max_iterations; i++) {
        hu_experiment_result_t result = {0};
        result.iteration = i;

        if (i > 0)
            mutate_config(&current_config, i);

        hu_error_t err = run_single_experiment(alloc, &current_config,
                                               config->data_dir, &result);
        if (err != HU_OK) {
            result.status = HU_EXPERIMENT_CRASH;
            snprintf(result.description, sizeof(result.description),
                     "experiment infrastructure error: %d", (int)err);
        }

        if (result.status != HU_EXPERIMENT_CRASH) {
            if (result.val_bpb < best_bpb) {
                result.status = HU_EXPERIMENT_KEEP;
                best_bpb = result.val_bpb;
                best_config = current_config;
                best_iter = i;
                if (result.description[0] == '\0')
                    snprintf(result.description, sizeof(result.description),
                             "improved bpb=%.6f (iter %d)", result.val_bpb, i);
            } else {
                result.status = HU_EXPERIMENT_DISCARD;
                current_config = best_config;
                if (result.description[0] == '\0')
                    snprintf(result.description, sizeof(result.description),
                             "no improvement bpb=%.6f vs best=%.6f",
                             result.val_bpb, best_bpb);
            }
        }

        if (callback)
            callback(&result, user_data);

        if (config->convergence_threshold > 0.0 && best_bpb <= config->convergence_threshold)
            break;
    }

    (void)best_iter;
    return HU_OK;
}

hu_error_t hu_experiment_result_to_tsv(const hu_experiment_result_t *result,
                                       char *buf, size_t buf_size)
{
    if (!result || !buf || buf_size == 0)
        return HU_ERR_INVALID_ARGUMENT;

    uint32_t config_hash = hash_experiment_config(&result->config);
    double peak_memory_gb = result->peak_memory_mb / 1024.0;
    const char *status = status_string(result->status);

    int n = snprintf(buf, buf_size,
                    "%u\t%.6f\t%.1f\t%s\t%s\n",
                    (unsigned)config_hash,
                    result->val_bpb,
                    peak_memory_gb,
                    status,
                    result->description);

    if (n < 0)
        return HU_ERR_INTERNAL;
    if ((size_t)n >= buf_size)
        return HU_ERR_INVALID_ARGUMENT;

    return HU_OK;
}
