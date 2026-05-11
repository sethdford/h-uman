/* Autonomous experiment loop — core of autoresearch ported to C.
 *
 * Each iteration: build model from config -> train -> evaluate -> keep/discard.
 * The loop explores the config space, tracking the best result.
 */

#include "human/ml/experiment.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/ml/checkpoint.h"
#include "human/ml/dataloader.h"
#include "human/ml/evaluator.h"
#include "human/ml/experiment_store.h"
#include "human/ml/ml.h"
#include "human/ml/model.h"
#include "human/ml/optimizer.h"
#include "human/ml/prepare.h"
#include "human/ml/tokenizer_ml.h"
#include "human/ml/train.h"
#include "human/provider.h"
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ─── Helpers ───────────────────────────────────────────────────────────── */

static uint32_t hash_experiment_config(const hu_experiment_config_t *config) {
    const unsigned char *p = (const unsigned char *)config;
    uint32_t h = 5381u;
    for (size_t i = 0; i < sizeof(hu_experiment_config_t); i++)
        h = ((h << 5) + h) + (unsigned)p[i];
    return h;
}

static const char *status_string(hu_experiment_status_t status) {
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

static double now_seconds(void) {
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
#else
    return (double)clock() / (double)CLOCKS_PER_SEC;
#endif
}

/* Simple xorshift32 PRNG for config exploration */
static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Parse key=value from agent response and apply to config */
void hu_experiment_apply_agent_kv(hu_experiment_config_t *cfg, const char *key, const char *val) {
    if (!cfg || !key || !val)
        return;
    if (strcmp(key, "n_layer") == 0) {
        int v = atoi(val);
        if (v > 0 && v <= 64)
            cfg->gpt.n_layer = (size_t)v;
    } else if (strcmp(key, "n_embd") == 0) {
        int v = atoi(val);
        if (v > 0 && v <= 4096 && (size_t)v % cfg->gpt.head_dim == 0) {
            cfg->gpt.n_embd = (size_t)v;
            cfg->gpt.n_head = (size_t)v / cfg->gpt.head_dim;
            cfg->gpt.n_kv_head = cfg->gpt.n_head;
        }
    } else if (strcmp(key, "matrix_lr") == 0) {
        float v = (float)atof(val);
        if (v > 0 && v < 1.0f)
            cfg->optimizer.matrix_lr = v;
    } else if (strcmp(key, "embedding_lr") == 0) {
        float v = (float)atof(val);
        if (v > 0 && v < 10.0f)
            cfg->optimizer.embedding_lr = v;
    } else if (strcmp(key, "weight_decay") == 0) {
        float v = (float)atof(val);
        if (v >= 0 && v < 1.0f)
            cfg->optimizer.weight_decay = v;
    } else if (strcmp(key, "activation") == 0) {
        if (strcmp(val, "relu_sq") == 0)
            cfg->gpt.activation = HU_ML_ACT_RELU_SQ;
        else if (strcmp(val, "gelu") == 0)
            cfg->gpt.activation = HU_ML_ACT_GELU;
        else if (strcmp(val, "swiglu") == 0)
            cfg->gpt.activation = HU_ML_ACT_SWIGLU;
    }
}

/* Ask the provider for a config mutation suggestion */
static int agent_suggest_mutation(hu_allocator_t *alloc, hu_experiment_config_t *cfg,
                                  void *provider_ptr, const char *persona,
                                  const hu_experiment_result_t *last_result) {
#ifdef HU_IS_TEST
    (void)alloc;
    (void)cfg;
    (void)provider_ptr;
    (void)persona;
    (void)last_result;
    return 0;
#else
    if (!provider_ptr || !alloc)
        return 0;
    hu_provider_t *prov = (hu_provider_t *)provider_ptr;
    if (!prov->vtable || !prov->vtable->chat_with_system)
        return 0;

    char sys_prompt[1024];
    snprintf(sys_prompt, sizeof(sys_prompt),
             "You are %s, an AI research assistant. Suggest ONE config mutation as key=value. "
             "Keys: n_layer, n_embd, matrix_lr, embedding_lr, weight_decay, activation. "
             "Current: n_layer=%zu n_embd=%zu lr=%.4f bpb=%.4f",
             persona ? persona : "researcher", cfg->gpt.n_layer, cfg->gpt.n_embd,
             cfg->optimizer.matrix_lr, last_result ? last_result->val_bpb : 0.0);

    char *response = NULL;
    size_t resp_len = 0;
    hu_error_t err = prov->vtable->chat_with_system(prov->ctx, alloc, sys_prompt,
                                                    strlen(sys_prompt), "Suggest a mutation.", 19,
                                                    NULL, 0, 0.7, &response, &resp_len);
    if (err != HU_OK || !response)
        return 0;

    /* Parse key=value from response */
    char *eq = strchr(response, '=');
    if (eq && eq > response) {
        *eq = '\0';
        char *key = response;
        while (*key == ' ')
            key++;
        char *val = eq + 1;
        while (*val == ' ')
            val++;
        char *end = val + strlen(val) - 1;
        while (end > val && (*end == '\n' || *end == ' '))
            *end-- = '\0';
        hu_experiment_apply_agent_kv(cfg, key, val);
    }
    alloc->free(alloc->ctx, response, resp_len);
    return 1;
#endif
}

/* Apply a mutation to the config for the next experiment.
 * Uses iteration + config hash to avoid deterministic cycling. */
static void mutate_config(hu_experiment_config_t *cfg, int iteration) {
    uint32_t seed = hash_experiment_config(cfg) ^ ((uint32_t)iteration * 2654435761u);
    if (seed == 0)
        seed = 1;

    switch (xorshift32(&seed) % 8) {
    case 0:
        if (cfg->gpt.n_layer > 2)
            cfg->gpt.n_layer += (xorshift32(&seed) % 2) ? 2 : -2;
        break;
    case 1: {
        float delta = (xorshift32(&seed) % 2) ? 1.2f : 0.8f;
        cfg->optimizer.matrix_lr *= delta;
        if (cfg->optimizer.matrix_lr < 0.001f)
            cfg->optimizer.matrix_lr = 0.001f;
        if (cfg->optimizer.matrix_lr > 0.2f)
            cfg->optimizer.matrix_lr = 0.2f;
        break;
    }
    case 2: {
        float delta = (xorshift32(&seed) % 2) ? 1.3f : 0.7f;
        cfg->optimizer.embedding_lr *= delta;
        if (cfg->optimizer.embedding_lr < 0.01f)
            cfg->optimizer.embedding_lr = 0.01f;
        if (cfg->optimizer.embedding_lr > 2.0f)
            cfg->optimizer.embedding_lr = 2.0f;
        break;
    }
    case 3:
        cfg->optimizer.weight_decay = (xorshift32(&seed) % 2) ? 0.1f : 0.3f;
        break;
    case 4:
        cfg->optimizer.warmdown_ratio = (xorshift32(&seed) % 3) * 0.25f;
        break;
    case 5: {
        size_t hd = cfg->gpt.head_dim;
        if (hd == 0)
            break;
        size_t delta = hd; /* step in multiples of head_dim */
        size_t new_dim =
            cfg->gpt.n_embd + ((xorshift32(&seed) % 2) ? delta : (size_t)(-(int64_t)delta));
        if (new_dim >= hd && new_dim <= 2048 && new_dim % hd == 0) {
            cfg->gpt.n_embd = new_dim;
            cfg->gpt.n_head = new_dim / hd;
            cfg->gpt.n_kv_head = cfg->gpt.n_head;
        }
        break;
    }
    case 6:
        cfg->gpt.activation = (hu_ml_activation_t)((xorshift32(&seed)) % 3);
        break;
    case 7:
        cfg->optimizer.adam_beta1 = (xorshift32(&seed) % 2) ? 0.8f : 0.9f;
        break;
    }
}

/* Run a single experiment: create model, train, evaluate. */
static hu_error_t run_single_experiment(hu_allocator_t *alloc, const hu_experiment_config_t *cfg,
                                        const char *data_dir, hu_experiment_result_t *result) {
    hu_error_t err;
    double t_start = now_seconds();

    memset(result, 0, sizeof(*result));
    result->config = *cfg;

    hu_model_t model = {0};
    err = hu_gpt_create(alloc, &cfg->gpt, &model);
    if (err != HU_OK) {
        result->status = HU_EXPERIMENT_CRASH;
        snprintf(result->description, sizeof(result->description), "model creation failed: %d",
                 (int)err);
        return HU_OK;
    }

    result->config.gpt.vocab_size = cfg->gpt.vocab_size;
    size_t num_params = model.vtable->num_params(model.ctx);

    hu_ml_optimizer_t optimizer = {0};
    err = hu_muon_adamw_create(alloc, &cfg->optimizer, &optimizer);
    if (err != HU_OK) {
        model.vtable->deinit(model.ctx, alloc);
        result->status = HU_EXPERIMENT_CRASH;
        snprintf(result->description, sizeof(result->description), "optimizer creation failed: %d",
                 (int)err);
        return HU_OK;
    }

    hu_ml_dataloader_t *train_dl = NULL;
    err = hu_ml_dataloader_create(alloc, data_dir, cfg->training.device_batch_size,
                                  cfg->gpt.sequence_len, "train", &train_dl);
    if (err != HU_OK) {
        optimizer.vtable->deinit(optimizer.ctx, alloc);
        model.vtable->deinit(model.ctx, alloc);
        result->status = HU_EXPERIMENT_CRASH;
        snprintf(result->description, sizeof(result->description), "train dataloader failed: %d",
                 (int)err);
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
        snprintf(result->description, sizeof(result->description), "val dataloader failed: %d",
                 (int)err);
        return HU_OK;
    }

    /* Register model params with optimizer so gradients flow */
    err = hu_gpt_register_params(&model, &optimizer);
    if (err != HU_OK) {
        hu_ml_dataloader_deinit(val_dl);
        hu_ml_dataloader_deinit(train_dl);
        optimizer.vtable->deinit(optimizer.ctx, alloc);
        model.vtable->deinit(model.ctx, alloc);
        result->status = HU_EXPERIMENT_CRASH;
        snprintf(result->description, sizeof(result->description), "param registration failed: %d",
                 (int)err);
        return HU_OK;
    }

    if (cfg->training.checkpoint_path && cfg->training.checkpoint_path[0]) {
        hu_error_t ld_err =
            hu_ml_checkpoint_load(alloc, cfg->training.checkpoint_path, &model, &optimizer);
        if (ld_err == HU_OK)
            hu_log_info("experiment", NULL, "resumed from checkpoint: %s",
                        cfg->training.checkpoint_path);
    }

    /* Phase 0 fix — derive a real token_bytes table so hu_ml_train's CE
     * objective actually runs and val_bpb is meaningful. Without this the
     * autonomous keep/discard decision compares zero against zero across
     * iterations and is effectively random. We try to load a real tokenizer
     * from data_dir → ~/.human/models → default byte-level. If its vocab
     * mismatches the model's vocab (which the loop shouldn't mutate but
     * could), fall back to a 1-byte-per-token table sized to the model so
     * hu_ml_train's bounds checks never trip and BPB stays well-defined.
     * See spec §1.5.2 issue #3. */
    hu_bpe_tokenizer_t *tok = NULL;
    int32_t *token_bytes = NULL;
    size_t token_bytes_count = 0;
    {
        int32_t *derived = NULL;
        size_t derived_count = 0;
        hu_error_t terr =
            hu_ml_prepare_load_default_tokenizer(alloc, data_dir, &tok, &derived, &derived_count);
        if (terr == HU_OK && derived_count == cfg->gpt.vocab_size) {
            token_bytes = derived;
            token_bytes_count = derived_count;
        } else {
            if (derived)
                alloc->free(alloc->ctx, derived, derived_count * sizeof(int32_t));
            if (tok) {
                hu_bpe_tokenizer_deinit(tok);
                tok = NULL;
            }
            if (cfg->gpt.vocab_size > 0) {
                token_bytes = (int32_t *)alloc->alloc(
                    alloc->ctx, cfg->gpt.vocab_size * sizeof(int32_t));
                if (token_bytes) {
                    for (size_t i = 0; i < cfg->gpt.vocab_size; i++)
                        token_bytes[i] = 1;
                    token_bytes_count = cfg->gpt.vocab_size;
                }
            }
        }
    }

    hu_ml_train_result_t train_result = {0};
    err = hu_ml_train(alloc, &model, &optimizer, train_dl, val_dl, &cfg->training, token_bytes,
                      token_bytes_count, &train_result);

    result->val_bpb = train_result.val_bpb;
    result->peak_memory_mb = train_result.peak_memory_mb;

    if (err != HU_OK || !train_result.converged) {
        result->status = HU_EXPERIMENT_CRASH;
        snprintf(result->description, sizeof(result->description),
                 "training failed: %d (params=%zuM)", (int)err, num_params / 1000000);
    }

    if (token_bytes)
        alloc->free(alloc->ctx, token_bytes, token_bytes_count * sizeof(int32_t));
    if (tok)
        hu_bpe_tokenizer_deinit(tok);
    hu_ml_dataloader_deinit(val_dl);
    hu_ml_dataloader_deinit(train_dl);
    optimizer.vtable->deinit(optimizer.ctx, alloc);
    model.vtable->deinit(model.ctx, alloc);

    result->training_seconds = now_seconds() - t_start;

    return HU_OK;
}

/* ─── Public API ─────────────────────────────────────────────────────────── */

hu_error_t hu_experiment_loop(hu_allocator_t *alloc, const hu_experiment_loop_config_t *config,
                              hu_experiment_loop_callback_t callback, void *user_data) {
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
    hu_experiment_result_t prev_result = {0};

    /* Copy LR schedule from optimizer config to training config */
    current_config.training.warmup_ratio = current_config.optimizer.warmup_ratio;
    current_config.training.warmdown_ratio = current_config.optimizer.warmdown_ratio;
    current_config.training.final_lr_frac = current_config.optimizer.final_lr_frac;
    /* Wire n_embd for dmodel LR scaling */
    current_config.optimizer.n_embd = current_config.gpt.n_embd;

    /* Open experiment store if a results path is given (non-fatal on failure) */
    hu_experiment_store_t *store = NULL;
    if (config->results_path) {
        hu_error_t store_err = hu_experiment_store_open(alloc, config->results_path, &store);
        if (store_err != HU_OK)
            store = NULL;
    }

    for (int i = 0; i < config->max_iterations; i++) {
        hu_experiment_result_t result = {0};
        result.iteration = i;

        if (i > 0) {
            int used_agent = 0;
            if (config->provider)
                used_agent = agent_suggest_mutation(alloc, &current_config, config->provider,
                                                    config->persona, &prev_result);
            if (!used_agent)
                mutate_config(&current_config, i);
            current_config.optimizer.n_embd = current_config.gpt.n_embd;
        }

        hu_error_t err = run_single_experiment(alloc, &current_config, config->data_dir, &result);
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
                             "no improvement bpb=%.6f vs best=%.6f", result.val_bpb, best_bpb);
            }
        }

        if (store)
            hu_experiment_store_save(store, &result);

        if (callback)
            callback(&result, user_data);

        prev_result = result;

        if (config->convergence_threshold > 0.0 && best_bpb <= config->convergence_threshold)
            break;
    }

    if (store)
        hu_experiment_store_close(store);

    (void)best_iter;
    return HU_OK;
}

hu_error_t hu_experiment_result_to_tsv(const hu_experiment_result_t *result, char *buf,
                                       size_t buf_size) {
    if (!result || !buf || buf_size == 0)
        return HU_ERR_INVALID_ARGUMENT;

    uint32_t config_hash = hash_experiment_config(&result->config);
    double peak_memory_gb = result->peak_memory_mb / 1024.0;
    const char *status = status_string(result->status);

    int n = snprintf(buf, buf_size, "%u\t%.6f\t%.1f\t%s\t%s\n", (unsigned)config_hash,
                     result->val_bpb, peak_memory_gb, status, result->description);

    if (n < 0)
        return HU_ERR_INTERNAL;
    if ((size_t)n >= buf_size)
        return HU_ERR_INVALID_ARGUMENT;

    return HU_OK;
}
