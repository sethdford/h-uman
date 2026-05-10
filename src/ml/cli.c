/* ML CLI subcommands: train, experiment, prepare, status, dpo-train, lora-persona. */

#include "human/ml/cli.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/ml/checkpoint.h"
#include "human/ml/dataloader.h"
#include "human/ml/dpo.h"
#include "human/ml/experiment.h"
#include "human/ml/lora.h"
#include "human/ml/ml.h"
#include "human/ml/model.h"
#include "human/ml/optimizer.h"
#include "human/ml/prepare.h"
#include "human/ml/tokenizer_ml.h"
#include "human/ml/train.h"
#include "human/persona.h"
#include "human/provider.h"
#include "human/providers/factory.h"
#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int parse_int_arg(const char *val, int *out) {
    if (!val || !out)
        return -1;
    char *end = NULL;
    long n = strtol(val, &end, 10);
    if (end == val || *end != '\0' || n < 0)
        return -1;
    *out = (int)n;
    return 0;
}

static const char *get_opt(const char **argv, int argc, int i, const char *opt) {
    if (i + 1 < argc && strcmp(argv[i], opt) == 0)
        return argv[i + 1];
    return NULL;
}

hu_error_t hu_ml_cli_train(hu_allocator_t *alloc, int argc, const char **argv) {
    (void)alloc;
    const char *config_path = NULL;
    for (int i = 1; i < argc; i++) {
        const char *v = get_opt(argv, argc, i, "--config");
        if (v) {
            config_path = v;
            break;
        }
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: human ml train [--config <path>] [--help]\n");
            return HU_OK;
        }
    }
#ifdef HU_IS_TEST
    (void)config_path;
    return HU_OK;
#else
    if (!config_path)
        config_path = "config.json";

    FILE *f = fopen(config_path, "rb");
    if (!f) {
        hu_log_error("ml", NULL, "Cannot open config: %s", config_path);
        return HU_ERR_INVALID_ARGUMENT;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) {
        fclose(f);
        hu_log_error("ml", NULL, "Config file too large or empty: %s", config_path);
        return HU_ERR_INVALID_ARGUMENT;
    }
    char *json_buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!json_buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    if (fread(json_buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        alloc->free(alloc->ctx, json_buf, (size_t)sz + 1);
        return HU_ERR_INVALID_ARGUMENT;
    }
    fclose(f);
    json_buf[sz] = '\0';

    hu_json_value_t *root = NULL;
    hu_error_t jerr = hu_json_parse(alloc, json_buf, (size_t)sz, &root);
    alloc->free(alloc->ctx, json_buf, (size_t)sz + 1);
    if (jerr != HU_OK || !root) {
        hu_log_error("ml", NULL, "Invalid JSON in config: %s", config_path);
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_experiment_config_t cfg = hu_experiment_config_default();

    const char *v;
    v = hu_json_get_string(root, "data_dir");
    const char *data_dir = v ? v : ".";

    double dv;
    if ((dv = hu_json_get_number(root, "batch_size", 0.0)) > 0)
        cfg.training.device_batch_size = (size_t)dv;
    if ((dv = hu_json_get_number(root, "max_steps", 0.0)) > 0)
        cfg.training.max_steps = (size_t)dv;
    if ((dv = hu_json_get_number(root, "time_budget_secs", 0.0)) > 0)
        cfg.training.time_budget_secs = (int)dv;

    v = hu_json_get_string(root, "checkpoint_path");
    if (v)
        cfg.training.checkpoint_path = v;

    hu_model_t model = {0};
    hu_error_t err = hu_gpt_create(alloc, &cfg.gpt, &model);
    if (err != HU_OK) {
        hu_json_free(alloc, root);
        hu_log_error("ml", NULL, "Model creation failed");
        return err;
    }

    hu_ml_optimizer_t optimizer = {0};
    err = hu_muon_adamw_create(alloc, &cfg.optimizer, &optimizer);
    if (err != HU_OK) {
        model.vtable->deinit(model.ctx, alloc);
        hu_json_free(alloc, root);
        hu_log_error("ml", NULL, "Optimizer creation failed");
        return err;
    }

    hu_ml_dataloader_t *train_loader = NULL;
    err = hu_ml_dataloader_create(alloc, data_dir, cfg.training.device_batch_size,
                                  cfg.gpt.sequence_len, "train", &train_loader);
    if (err != HU_OK) {
        optimizer.vtable->deinit(optimizer.ctx, alloc);
        model.vtable->deinit(model.ctx, alloc);
        hu_json_free(alloc, root);
        hu_log_error("ml", NULL, "Dataloader creation failed for %s", data_dir);
        return err;
    }

    printf("Training: batch_size=%zu, max_steps=%zu, data=%s\n", cfg.training.device_batch_size,
           cfg.training.max_steps, data_dir);

    hu_ml_train_result_t result = {0};
    err =
        hu_ml_train(alloc, &model, &optimizer, train_loader, NULL, &cfg.training, NULL, 0, &result);

    printf("Training %s: %zu steps, %.2f bpb, %.1fs\n", err == HU_OK ? "complete" : "failed",
           result.num_steps, result.val_bpb, result.training_seconds);

    hu_ml_dataloader_deinit(train_loader);
    optimizer.vtable->deinit(optimizer.ctx, alloc);
    model.vtable->deinit(model.ctx, alloc);
    hu_json_free(alloc, root);
    return err;
#endif
}

hu_error_t hu_ml_cli_experiment(hu_allocator_t *alloc, int argc, const char **argv) {
    const char *config_path = NULL;
    const char *data_dir = NULL;
    int max_iterations = 10;
    for (int i = 1; i < argc; i++) {
        const char *v = get_opt(argv, argc, i, "--config");
        if (v)
            config_path = v;
        v = get_opt(argv, argc, i, "--data-dir");
        if (v)
            data_dir = v;
        v = get_opt(argv, argc, i, "--max-iterations");
        if (v && parse_int_arg(v, &max_iterations) != 0) {
            hu_log_error("ml", NULL, "Invalid --max-iterations: %s", v);
            return HU_ERR_INVALID_ARGUMENT;
        }
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: human experiment [--config <path>] [--max-iterations <N>] "
                   "[--data-dir <path>] [--help]\n");
            return HU_OK;
        }
    }
#ifdef HU_IS_TEST
    (void)alloc;
    (void)config_path;
    (void)data_dir;
    (void)max_iterations;
    return HU_OK;
#else
    if (!data_dir)
        data_dir = ".";
    hu_experiment_loop_config_t loop_cfg = {0};
    loop_cfg.max_iterations = max_iterations;
    loop_cfg.base_config = hu_experiment_config_default();
    loop_cfg.data_dir = data_dir;
    loop_cfg.convergence_threshold = 0.0;
    (void)config_path;
    return hu_experiment_loop(alloc, &loop_cfg, NULL, NULL);
#endif
}

hu_error_t hu_ml_cli_prepare(hu_allocator_t *alloc, int argc, const char **argv) {
    const char *input_dir = NULL;
    const char *output_dir = NULL;
    int vocab_size = 8192;
    for (int i = 1; i < argc; i++) {
        const char *v = get_opt(argv, argc, i, "--input");
        if (v)
            input_dir = v;
        v = get_opt(argv, argc, i, "--output");
        if (v)
            output_dir = v;
        v = get_opt(argv, argc, i, "--vocab-size");
        if (v && parse_int_arg(v, &vocab_size) != 0) {
            hu_log_error("ml", NULL, "Invalid --vocab-size: %s", v);
            return HU_ERR_INVALID_ARGUMENT;
        }
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: human ml prepare [--input <dir>] [--output <dir>] "
                   "[--vocab-size <N>] [--help]\n");
            return HU_OK;
        }
    }
#ifdef HU_IS_TEST
    (void)alloc;
    (void)input_dir;
    (void)output_dir;
    (void)vocab_size;
    return HU_OK;
#else
    if (!input_dir || !output_dir) {
        hu_log_error("ml", NULL, "prepare requires --input and --output");
        return HU_ERR_INVALID_ARGUMENT;
    }
    hu_bpe_tokenizer_t *tok = NULL;
    hu_error_t err = hu_bpe_tokenizer_create(alloc, &tok);
    if (err != HU_OK)
        return err;
    /* Use byte-level tokenizer; optional BPE training would need corpus scan */
    (void)vocab_size;
    err = hu_ml_prepare_tokenize_dir(alloc, tok, input_dir, output_dir);
    hu_bpe_tokenizer_deinit(tok);
    return err;
#endif
}

hu_error_t hu_ml_cli_status(hu_allocator_t *alloc, int argc, const char **argv) {
    (void)alloc;
    (void)argc;
    (void)argv;
#ifdef HU_IS_TEST
    printf("No experiments found\n");
    return HU_OK;
#else
    const char *path = "results.tsv";
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("No experiments found\n");
        return HU_OK;
    }
    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] != '\0' && line[0] != '#') {
            printf("%s", line);
            count++;
        }
    }
    fclose(f);
    if (count == 0)
        printf("No experiments found\n");
    return HU_OK;
#endif
}

hu_error_t hu_ml_cli_dpo_train(hu_allocator_t *alloc, int argc, const char **argv) {
    const char *db_path = NULL;
    const char *provider_name = NULL;
    const char *model = NULL;
    int batch_size = 20;
    for (int i = 1; i < argc; i++) {
        const char *v = get_opt(argv, argc, i, "--db");
        if (v) {
            db_path = v;
            i++;
            continue;
        }
        v = get_opt(argv, argc, i, "--provider");
        if (v) {
            provider_name = v;
            i++;
            continue;
        }
        v = get_opt(argv, argc, i, "--model");
        if (v) {
            model = v;
            i++;
            continue;
        }
        v = get_opt(argv, argc, i, "--batch-size");
        if (v) {
            if (parse_int_arg(v, &batch_size) != 0) {
                hu_log_error("ml", NULL, "Invalid --batch-size: %s", v);
                return HU_ERR_INVALID_ARGUMENT;
            }
            i++;
            continue;
        }
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: human ml dpo-train [--db <path>] [--provider <name>] "
                   "[--model <name>] [--batch-size <N>] [--help]\n");
            return HU_OK;
        }
    }
#ifdef HU_IS_TEST
    (void)alloc;
    (void)db_path;
    (void)provider_name;
    (void)model;
    (void)batch_size;
    printf("[dpo] test mode: skipped\n");
    return HU_OK;
#else
#ifdef HU_ENABLE_SQLITE
    if (!db_path)
        db_path = "memory.db";
    if (!provider_name) {
        fprintf(stderr, "dpo-train requires --provider\n");
        return HU_ERR_INVALID_ARGUMENT;
    }

    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", db_path);
        return HU_ERR_IO;
    }

    hu_dpo_collector_t collector;
    hu_error_t err = hu_dpo_collector_create(alloc, db, 10000, &collector);
    if (err != HU_OK) {
        sqlite3_close(db);
        return err;
    }

    hu_provider_t provider = {0};
    size_t pname_len = strlen(provider_name);
    err = hu_provider_create(alloc, provider_name, pname_len, NULL, 0, NULL, 0, &provider);
    if (err != HU_OK) {
        fprintf(stderr, "Cannot create provider '%s': %d\n", provider_name, err);
        hu_dpo_collector_deinit(&collector);
        sqlite3_close(db);
        return err;
    }

    size_t model_len = model ? strlen(model) : 0;
    hu_dpo_train_result_t result = {0};
    printf("[dpo] Running DPO training step (provider=%s, batch=%d)...\n", provider_name,
           batch_size);

    err = hu_dpo_train_step(&collector, alloc, &provider, model, model_len, 0.1, (size_t)batch_size,
                            &result);

    if (err == HU_OK) {
        printf("[dpo] Training complete:\n");
        printf("  Pairs evaluated: %zu\n", result.pairs_evaluated);
        printf("  Pairs aligned:   %zu\n", result.pairs_aligned);
        printf("  Alignment score: %.2f%%\n", result.alignment_score * 100.0);
        printf("  Loss:            %.4f\n", result.loss);
    } else {
        fprintf(stderr, "[dpo] Training failed: %d\n", err);
    }

    if (provider.vtable && provider.vtable->deinit)
        provider.vtable->deinit(provider.ctx, alloc);
    hu_dpo_collector_deinit(&collector);
    sqlite3_close(db);
    return err;
#else
    (void)alloc;
    (void)db_path;
    (void)provider_name;
    (void)model;
    (void)batch_size;
    fprintf(stderr, "dpo-train requires HU_ENABLE_SQLITE\n");
    return HU_ERR_NOT_SUPPORTED;
#endif
#endif
}

hu_error_t hu_ml_cli_prepare_conversations(hu_allocator_t *alloc, int argc, const char **argv) {
    const char *chat_db = NULL;
    const char *memory_db = NULL;
    const char *output_dir = NULL;
    for (int i = 1; i < argc; i++) {
        const char *v = get_opt(argv, argc, i, "--chat-db");
        if (v) {
            chat_db = v;
            i++;
            continue;
        }
        v = get_opt(argv, argc, i, "--memory-db");
        if (v) {
            memory_db = v;
            i++;
            continue;
        }
        v = get_opt(argv, argc, i, "--output");
        if (v) {
            output_dir = v;
            i++;
            continue;
        }
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: human ml prepare-conversations [--chat-db <path>] "
                   "[--memory-db <path>] --output <dir> [--help]\n");
            return HU_OK;
        }
    }
#ifdef HU_IS_TEST
    (void)alloc;
    (void)chat_db;
    (void)memory_db;
    (void)output_dir;
    printf("[prepare-conversations] test mode: skipped\n");
    return HU_OK;
#else
    if (!output_dir) {
        fprintf(stderr, "prepare-conversations requires --output\n");
        return HU_ERR_INVALID_ARGUMENT;
    }
    hu_bpe_tokenizer_t *tok = NULL;
    hu_error_t err = hu_bpe_tokenizer_create(alloc, &tok);
    if (err != HU_OK)
        return err;
    size_t processed = 0;
    err = hu_ml_prepare_conversations(alloc, tok, chat_db, memory_db, output_dir, &processed);
    hu_bpe_tokenizer_deinit(tok);
    if (err == HU_OK)
        printf("[prepare-conversations] Done: %zu messages processed\n", processed);
    else
        fprintf(stderr, "[prepare-conversations] Failed: %d\n", err);
    return err;
#endif
}

hu_error_t hu_ml_cli_lora_persona(hu_allocator_t *alloc, int argc, const char **argv) {
    const char *persona_name = NULL;
    const char *checkpoint_path = NULL;
    const char *output_path = NULL;
    int rank = 8;
    int max_steps = 200;
    for (int i = 1; i < argc; i++) {
        const char *v = get_opt(argv, argc, i, "--persona");
        if (v) {
            persona_name = v;
            i++;
            continue;
        }
        v = get_opt(argv, argc, i, "--checkpoint");
        if (v) {
            checkpoint_path = v;
            i++;
            continue;
        }
        v = get_opt(argv, argc, i, "--output");
        if (v) {
            output_path = v;
            i++;
            continue;
        }
        v = get_opt(argv, argc, i, "--rank");
        if (v) {
            parse_int_arg(v, &rank);
            i++;
            continue;
        }
        v = get_opt(argv, argc, i, "--max-steps");
        if (v) {
            parse_int_arg(v, &max_steps);
            i++;
            continue;
        }
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: human ml lora-persona --persona <name> "
                   "[--checkpoint <path>] [--output <path>] "
                   "[--rank <N>] [--max-steps <N>] [--help]\n");
            printf("\n  --checkpoint  Optional HUML base GPT checkpoint to warm-start "
                   "before LoRA attaches.\n");
            printf("                Must match the reference GPT dims (see "
                   "hu_experiment_config_default).\n");
            printf("                NOTE: this is the reference HUML GPT, NOT a "
                   "frontier model.\n");
            printf("                Frontier model fine-tuning is tracked in\n");
            printf("                docs/plans/2026-05-10-m3-frontier-model-bridge.md\n");
            return HU_OK;
        }
    }
#ifdef HU_IS_TEST
    (void)alloc;
    (void)persona_name;
    (void)checkpoint_path;
    (void)output_path;
    (void)rank;
    (void)max_steps;
    printf("[lora-persona] test mode: skipped\n");
    printf("[lora-persona] honest-gap doc: docs/plans/2026-05-10-m3-frontier-model-bridge.md\n");
    return HU_OK;
#else
    if (!persona_name) {
        fprintf(stderr, "lora-persona requires --persona <name>\n");
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_persona_t persona = {0};
    hu_error_t err = hu_persona_load(alloc, persona_name, strlen(persona_name), &persona);
    if (err != HU_OK) {
        fprintf(stderr, "Failed to load persona '%s': %d\n", persona_name, err);
        return err;
    }

    size_t total_examples = 0;
    for (size_t b = 0; b < persona.example_banks_count; b++)
        total_examples += persona.example_banks[b].examples_count;

    if (total_examples == 0) {
        fprintf(stderr, "Persona '%s' has no example banks to train on\n", persona_name);
        hu_persona_deinit(alloc, &persona);
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_experiment_config_t cfg = hu_experiment_config_default();

    hu_model_t model = {0};
    err = hu_gpt_create(alloc, &cfg.gpt, &model);
    if (err != HU_OK) {
        fprintf(stderr, "Model creation failed: %d\n", err);
        hu_persona_deinit(alloc, &persona);
        return err;
    }

    /* Honest caveat — this trains a LoRA adapter on the reference HUML GPT,
     * NOT on a frontier model. Tracked in
     * docs/plans/2026-05-10-m3-frontier-model-bridge.md (M3 honest gap). */
    printf("[lora-persona] NOTE: trains LoRA on the reference HUML GPT.\n"
           "[lora-persona]       This is not a frontier model fine-tune.\n"
           "[lora-persona]       For Llama/Qwen/Mistral fine-tuning, see\n"
           "[lora-persona]       docs/plans/2026-05-10-m3-frontier-model-bridge.md\n");

    /* Optional warm-start: load a HUML reference-GPT checkpoint as the base
     * before attaching LoRA. We use a throwaway optimizer to satisfy the
     * checkpoint loader's group-count invariant; once the base params are
     * restored, the temp optimizer is freed and a fresh LoRA-only optimizer
     * is created below. */
    if (checkpoint_path && checkpoint_path[0]) {
        hu_ml_optimizer_t base_opt = {0};
        hu_error_t base_err = hu_muon_adamw_create(alloc, &cfg.optimizer, &base_opt);
        if (base_err != HU_OK) {
            fprintf(stderr, "[lora-persona] failed to create warm-start optimizer: %d\n",
                    base_err);
            model.vtable->deinit(model.ctx, alloc);
            hu_persona_deinit(alloc, &persona);
            return base_err;
        }
        base_err = hu_gpt_register_params(&model, &base_opt);
        if (base_err == HU_OK) {
            base_err =
                hu_ml_checkpoint_load(alloc, checkpoint_path, &model, &base_opt);
        }
        base_opt.vtable->deinit(base_opt.ctx, alloc);
        if (base_err != HU_OK) {
            fprintf(stderr,
                    "[lora-persona] failed to load checkpoint '%s': %d "
                    "(must be a HUML v1 or v2 file produced by hu_ml_checkpoint_save "
                    "with matching dims: vocab=%zu n_layer=%zu n_embd=%zu)\n",
                    checkpoint_path, base_err, cfg.gpt.vocab_size, cfg.gpt.n_layer,
                    cfg.gpt.n_embd);
            model.vtable->deinit(model.ctx, alloc);
            hu_persona_deinit(alloc, &persona);
            return base_err;
        }
        printf("[lora-persona] loaded base checkpoint: %s\n", checkpoint_path);
    }

    hu_lora_config_t lora_cfg = {
        .rank = (size_t)rank,
        .alpha = (float)rank,
        .dropout = 0.05f,
        .targets = HU_LORA_TARGET_QV,
    };
    hu_lora_adapter_t *adapter = NULL;
    err =
        hu_lora_create(alloc, &lora_cfg, cfg.gpt.n_embd, cfg.gpt.n_embd, cfg.gpt.n_layer, &adapter);
    if (err != HU_OK) {
        fprintf(stderr, "LoRA adapter creation failed: %d\n", err);
        model.vtable->deinit(model.ctx, alloc);
        hu_persona_deinit(alloc, &persona);
        return err;
    }

    /* Attach LoRA to Q and V projections */
    err = hu_gpt_attach_lora(&model, adapter, NULL, adapter, NULL, NULL, NULL);
    if (err != HU_OK) {
        fprintf(stderr, "LoRA attach failed: %d\n", err);
        hu_lora_destroy(alloc, adapter);
        model.vtable->deinit(model.ctx, alloc);
        hu_persona_deinit(alloc, &persona);
        return err;
    }

    hu_ml_optimizer_t optimizer = {0};
    err = hu_muon_adamw_create(alloc, &cfg.optimizer, &optimizer);
    if (err != HU_OK) {
        hu_lora_destroy(alloc, adapter);
        model.vtable->deinit(model.ctx, alloc);
        hu_persona_deinit(alloc, &persona);
        return err;
    }

    err = hu_lora_register_params(adapter, &optimizer);
    if (err != HU_OK) {
        optimizer.vtable->deinit(optimizer.ctx, alloc);
        hu_lora_destroy(alloc, adapter);
        model.vtable->deinit(model.ctx, alloc);
        hu_persona_deinit(alloc, &persona);
        return err;
    }

    hu_bpe_tokenizer_t *tok = NULL;
    err = hu_bpe_tokenizer_create(alloc, &tok);
    if (err != HU_OK) {
        optimizer.vtable->deinit(optimizer.ctx, alloc);
        hu_lora_destroy(alloc, adapter);
        model.vtable->deinit(model.ctx, alloc);
        hu_persona_deinit(alloc, &persona);
        return err;
    }

    printf("[lora-persona] Training on %zu examples from persona '%s' "
           "(rank=%d, steps=%d)\n",
           total_examples, persona_name, rank, max_steps);

    float best_loss = 1e9f;
    for (int step = 0; step < max_steps; step++) {
        float step_loss = 0.0f;
        size_t examples_this_step = 0;

        if (optimizer.vtable->zero_grad)
            optimizer.vtable->zero_grad(optimizer.ctx);

        for (size_t b = 0; b < persona.example_banks_count; b++) {
            const hu_persona_example_bank_t *bank = &persona.example_banks[b];
            for (size_t e = 0; e < bank->examples_count; e++) {
                const hu_persona_example_t *ex = &bank->examples[e];
                if (!ex->incoming || !ex->response)
                    continue;

                int32_t *in_ids = NULL;
                size_t in_count = 0;
                err = hu_bpe_tokenizer_encode(tok, ex->incoming, strlen(ex->incoming), &in_ids,
                                              &in_count);
                if (err != HU_OK || in_count == 0) {
                    if (in_ids)
                        alloc->free(alloc->ctx, in_ids, in_count * sizeof(int32_t));
                    continue;
                }

                int32_t *out_ids = NULL;
                size_t out_count = 0;
                err = hu_bpe_tokenizer_encode(tok, ex->response, strlen(ex->response), &out_ids,
                                              &out_count);
                if (err != HU_OK || out_count == 0) {
                    alloc->free(alloc->ctx, in_ids, in_count * sizeof(int32_t));
                    if (out_ids)
                        alloc->free(alloc->ctx, out_ids, out_count * sizeof(int32_t));
                    continue;
                }

                size_t seq_len = in_count + out_count;
                if (seq_len > cfg.gpt.sequence_len)
                    seq_len = cfg.gpt.sequence_len;

                int32_t *seq = (int32_t *)alloc->alloc(alloc->ctx, seq_len * sizeof(int32_t));
                if (!seq) {
                    alloc->free(alloc->ctx, in_ids, in_count * sizeof(int32_t));
                    alloc->free(alloc->ctx, out_ids, out_count * sizeof(int32_t));
                    continue;
                }

                size_t copy_in = in_count < seq_len ? in_count : seq_len;
                memcpy(seq, in_ids, copy_in * sizeof(int32_t));
                size_t copy_out = seq_len - copy_in;
                if (copy_out > out_count)
                    copy_out = out_count;
                memcpy(seq + copy_in, out_ids, copy_out * sizeof(int32_t));

                hu_ml_tensor_t input_tensor = {
                    .data = seq,
                    .shape = {1, (int)seq_len, 0, 0},
                    .ndim = 2,
                    .dtype = HU_ML_DTYPE_I32,
                };
                hu_ml_tensor_t output_tensor = {0};

                /* Forward pass (includes LoRA adapter) */
                hu_error_t fwd_err =
                    model.vtable->forward(model.ctx, &input_tensor, &output_tensor);
                if (fwd_err == HU_OK && output_tensor.data && seq_len > 1) {
                    float *logits = (float *)output_tensor.data;
                    float example_loss = 0.0f;
                    size_t resp_tokens = 0;
                    for (size_t t = copy_in; t < seq_len - 1; t++) {
                        int32_t target = seq[t + 1];
                        if (target >= 0 && (size_t)target < cfg.gpt.vocab_size) {
                            float *row = logits + t * cfg.gpt.vocab_size;
                            float max_val = row[0];
                            for (size_t v2 = 1; v2 < cfg.gpt.vocab_size; v2++)
                                if (row[v2] > max_val)
                                    max_val = row[v2];
                            float sum_exp = 0.0f;
                            for (size_t v2 = 0; v2 < cfg.gpt.vocab_size; v2++)
                                sum_exp += expf(row[v2] - max_val);
                            float log_prob = (row[target] - max_val) - logf(sum_exp);
                            example_loss -= log_prob;
                            resp_tokens++;
                        }
                    }
                    if (resp_tokens > 0) {
                        step_loss += example_loss / (float)resp_tokens;
                        examples_this_step++;
                    }

                    /* Backward pass: compute gradients for LoRA params */
                    if (model.vtable->backward) {
                        hu_ml_tensor_t grad_out = output_tensor;
                        (void)model.vtable->backward(model.ctx, &grad_out);
                    }
                }

                /* Free output tensor every iteration */
                if (output_tensor.data && output_tensor.size_bytes > 0)
                    alloc->free(alloc->ctx, output_tensor.data, output_tensor.size_bytes);

                alloc->free(alloc->ctx, seq, seq_len * sizeof(int32_t));
                alloc->free(alloc->ctx, out_ids, out_count * sizeof(int32_t));
                alloc->free(alloc->ctx, in_ids, in_count * sizeof(int32_t));
            }
        }

        /* Optimizer step: update LoRA weights */
        if (examples_this_step > 0 && optimizer.vtable->step) {
            hu_ml_tensor_t *params = NULL;
            size_t param_count = 0;
            if (model.vtable->get_params)
                model.vtable->get_params(model.ctx, &params, &param_count);
            if (params && param_count > 0)
                optimizer.vtable->step(optimizer.ctx, params, params, param_count);
        }

        float avg_loss = (examples_this_step > 0) ? step_loss / (float)examples_this_step : 0.0f;
        if (avg_loss < best_loss && avg_loss > 0.0f)
            best_loss = avg_loss;

        if (step % 50 == 0 || step == max_steps - 1)
            printf("  step %d/%d  loss=%.4f  best=%.4f\n", step + 1, max_steps, avg_loss,
                   best_loss);
    }

    /* Save LoRA adapter */
    char default_out[512];
    if (!output_path) {
        snprintf(default_out, sizeof(default_out), "lora-persona-%s.bin", persona_name);
        output_path = default_out;
    }
    err = hu_lora_save(adapter, output_path);
    if (err == HU_OK)
        printf("[lora-persona] Saved adapter to %s (%zu params)\n", output_path,
               hu_lora_num_params(adapter));
    else
        fprintf(stderr, "[lora-persona] Save failed: %d\n", err);

    hu_bpe_tokenizer_deinit(tok);
    optimizer.vtable->deinit(optimizer.ctx, alloc);
    hu_lora_destroy(alloc, adapter);
    model.vtable->deinit(model.ctx, alloc);
    hu_persona_deinit(alloc, &persona);
    return err;
#endif
}

hu_error_t hu_ml_cli_train_feed_predictor(hu_allocator_t *alloc, int argc, const char **argv) {
    const char *db_path = NULL;
    const char *output_path = NULL;
    int max_steps = 500;
    int lookback_days = 30;
    for (int i = 1; i < argc; i++) {
        const char *v = get_opt(argv, argc, i, "--db");
        if (v) {
            db_path = v;
            i++;
            continue;
        }
        v = get_opt(argv, argc, i, "--output");
        if (v) {
            output_path = v;
            i++;
            continue;
        }
        v = get_opt(argv, argc, i, "--max-steps");
        if (v) {
            parse_int_arg(v, &max_steps);
            i++;
            continue;
        }
        v = get_opt(argv, argc, i, "--lookback-days");
        if (v) {
            parse_int_arg(v, &lookback_days);
            i++;
            continue;
        }
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: human ml train-feed-predictor --db <path> "
                   "[--output <path>] [--max-steps <N>] "
                   "[--lookback-days <N>] [--help]\n");
            return HU_OK;
        }
    }
#ifdef HU_IS_TEST
    (void)alloc;
    (void)db_path;
    (void)output_path;
    (void)max_steps;
    (void)lookback_days;
    printf("[train-feed-predictor] test mode: skipped\n");
    return HU_OK;
#else
    if (!db_path) {
        fprintf(stderr, "train-feed-predictor requires --db <path>\n");
        return HU_ERR_INVALID_ARGUMENT;
    }

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        fprintf(stderr, "Cannot open feed database: %s\n", db_path);
        return HU_ERR_IO;
    }

    /* Query recent feed items for training data (source as topic signal) */
    const char *sql = "SELECT content, source, content_type FROM feed_items "
                      "WHERE ingested_at > ? AND content IS NOT NULL "
                      "ORDER BY ingested_at DESC LIMIT 10000";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "SQL prepare failed: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return HU_ERR_IO;
    }

    int64_t cutoff = (int64_t)time(NULL) - (int64_t)lookback_days * 86400;
    sqlite3_bind_int64(stmt, 1, cutoff);

    hu_bpe_tokenizer_t *tok = NULL;
    hu_error_t err = hu_bpe_tokenizer_create(alloc, &tok);
    if (err != HU_OK) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return err;
    }

    /* Collect tokenized sequences: <|topic|>TOPIC<|content|>CONTENT */
    size_t seq_cap = 4096;
    size_t seq_count = 0;
    int32_t *all_tokens = (int32_t *)alloc->alloc(alloc->ctx, seq_cap * sizeof(int32_t));
    if (!all_tokens) {
        hu_bpe_tokenizer_deinit(tok);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return HU_ERR_OUT_OF_MEMORY;
    }

    size_t items_processed = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *content = (const char *)sqlite3_column_text(stmt, 0);
        const char *source = (const char *)sqlite3_column_text(stmt, 1);
        const char *content_type = (const char *)sqlite3_column_text(stmt, 2);
        if (!content)
            continue;
        const char *src = source ? source : "unknown";
        const char *ctype = content_type ? content_type : "";

        /* Format: "<|source|>SOURCE<|type|>TYPE<|content|>CONTENT\n" */
        size_t text_cap = strlen(src) + strlen(ctype) + strlen(content) + 48;
        char *text = (char *)alloc->alloc(alloc->ctx, text_cap);
        if (!text)
            continue;

        int written =
            snprintf(text, text_cap, "<|source|>%s<|type|>%s<|content|>%s\n", src, ctype, content);
        if (written <= 0) {
            alloc->free(alloc->ctx, text, text_cap);
            continue;
        }

        int32_t *ids = NULL;
        size_t id_count = 0;
        err = hu_bpe_tokenizer_encode(tok, text, (size_t)written, &ids, &id_count);
        alloc->free(alloc->ctx, text, text_cap);
        if (err != HU_OK || id_count == 0) {
            if (ids)
                alloc->free(alloc->ctx, ids, id_count * sizeof(int32_t));
            continue;
        }

        /* Grow token buffer if needed */
        while (seq_count + id_count > seq_cap) {
            size_t new_cap = seq_cap * 2;
            int32_t *new_buf = (int32_t *)alloc->alloc(alloc->ctx, new_cap * sizeof(int32_t));
            if (!new_buf) {
                alloc->free(alloc->ctx, ids, id_count * sizeof(int32_t));
                goto done_collect;
            }
            memcpy(new_buf, all_tokens, seq_count * sizeof(int32_t));
            alloc->free(alloc->ctx, all_tokens, seq_cap * sizeof(int32_t));
            all_tokens = new_buf;
            seq_cap = new_cap;
        }

        memcpy(all_tokens + seq_count, ids, id_count * sizeof(int32_t));
        seq_count += id_count;
        alloc->free(alloc->ctx, ids, id_count * sizeof(int32_t));
        items_processed++;
    }
done_collect:
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    if (items_processed == 0 || seq_count == 0) {
        fprintf(stderr, "No feed data found in last %d days\n", lookback_days);
        alloc->free(alloc->ctx, all_tokens, seq_cap * sizeof(int32_t));
        hu_bpe_tokenizer_deinit(tok);
        return HU_ERR_NOT_FOUND;
    }

    printf("[train-feed-predictor] Collected %zu items, %zu tokens\n", items_processed, seq_count);

    /* Write tokenized data to temp .bin for training (PID-unique dir) */
    char tmp_dir[128];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/hu-feed-train-%d", (int)getpid());
    char train_path[256], val_path[256];
    snprintf(train_path, sizeof(train_path), "%s/train.bin", tmp_dir);
    snprintf(val_path, sizeof(val_path), "%s/val.bin", tmp_dir);

    if (mkdir(tmp_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "Cannot create temp dir %s\n", tmp_dir);
        alloc->free(alloc->ctx, all_tokens, seq_cap * sizeof(int32_t));
        hu_bpe_tokenizer_deinit(tok);
        return HU_ERR_IO;
    }

    /* 90/10 train/val split */
    size_t split = (seq_count * 9) / 10;
    FILE *f = fopen(train_path, "wb");
    if (!f) {
        fprintf(stderr, "Cannot write %s\n", train_path);
        alloc->free(alloc->ctx, all_tokens, seq_cap * sizeof(int32_t));
        hu_bpe_tokenizer_deinit(tok);
        return HU_ERR_IO;
    }
    fwrite(all_tokens, sizeof(int32_t), split, f);
    fclose(f);

    f = fopen(val_path, "wb");
    if (!f) {
        fprintf(stderr, "Cannot write %s\n", val_path);
        (void)remove(train_path);
        (void)rmdir(tmp_dir);
        alloc->free(alloc->ctx, all_tokens, seq_cap * sizeof(int32_t));
        hu_bpe_tokenizer_deinit(tok);
        return HU_ERR_IO;
    }
    fwrite(all_tokens + split, sizeof(int32_t), seq_count - split, f);
    fclose(f);

    alloc->free(alloc->ctx, all_tokens, seq_cap * sizeof(int32_t));

    /* Train model */
    hu_experiment_config_t cfg = hu_experiment_config_default();
    cfg.training.max_steps = (size_t)max_steps;
    cfg.training.checkpoint_path = output_path ? output_path : "feed-predictor.huml";

    hu_model_t model = {0};
    err = hu_gpt_create(alloc, &cfg.gpt, &model);
    if (err != HU_OK) {
        hu_bpe_tokenizer_deinit(tok);
        return err;
    }

    hu_ml_optimizer_t optimizer = {0};
    err = hu_muon_adamw_create(alloc, &cfg.optimizer, &optimizer);
    if (err != HU_OK) {
        model.vtable->deinit(model.ctx, alloc);
        hu_bpe_tokenizer_deinit(tok);
        return err;
    }

    hu_ml_dataloader_t *train_loader = NULL;
    err = hu_ml_dataloader_create(alloc, tmp_dir, cfg.training.device_batch_size,
                                  cfg.gpt.sequence_len, "train", &train_loader);
    if (err != HU_OK) {
        optimizer.vtable->deinit(optimizer.ctx, alloc);
        model.vtable->deinit(model.ctx, alloc);
        hu_bpe_tokenizer_deinit(tok);
        return err;
    }

    printf("[train-feed-predictor] Training topic predictor (steps=%d)\n", max_steps);

    hu_ml_train_result_t result = {0};
    err =
        hu_ml_train(alloc, &model, &optimizer, train_loader, NULL, &cfg.training, NULL, 0, &result);

    printf("[train-feed-predictor] %s: %zu steps, %.2f bpb, %.1fs\n",
           err == HU_OK ? "Done" : "Failed", result.num_steps, result.val_bpb,
           result.training_seconds);

    hu_ml_dataloader_deinit(train_loader);
    optimizer.vtable->deinit(optimizer.ctx, alloc);
    model.vtable->deinit(model.ctx, alloc);
    hu_bpe_tokenizer_deinit(tok);

    /* Cleanup temp files */
    (void)remove(train_path);
    (void)remove(val_path);
    (void)rmdir(tmp_dir);

    return err;
#endif
}

/* ── W13: apply-adapter — close the LoRA loop ────────────────────────── */

#include "human/providers/huml.h"

hu_error_t hu_ml_cli_apply_adapter(hu_allocator_t *alloc, int argc, const char **argv) {
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;

    const char *adapter_path = NULL;
    const char *adapter_id = NULL;
    bool unload_after = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--adapter") == 0 && i + 1 < argc) {
            adapter_path = argv[++i];
        } else if (strcmp(argv[i], "--id") == 0 && i + 1 < argc) {
            adapter_id = argv[++i];
        } else if (strcmp(argv[i], "--unload-after") == 0) {
            unload_after = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: human ml apply-adapter --adapter PATH [--id ID] "
                   "[--unload-after]\n\n"
                   "  --adapter PATH    Path to a LoRA adapter file produced by\n"
                   "                    `human ml lora-persona`.\n"
                   "  --id ID           Identifier the provider should associate with\n"
                   "                    the adapter (defaults to the basename of PATH).\n"
                   "  --unload-after    Unload immediately after loading; useful for\n"
                   "                    smoke-testing the lifecycle.\n\n"
                   "Closes the W13 loop: trained adapter on disk -> "
                   "hu_provider_load_adapter -> active_adapter() reports the id.\n"
                   "Phase 4 of the FIX 15 frontier-bridge plan adds chat-time merging.\n");
            return HU_OK;
        } else {
            fprintf(stderr, "Unknown apply-adapter arg: %s\n", argv[i]);
            return HU_ERR_INVALID_ARGUMENT;
        }
    }
    if (!adapter_path) {
        fprintf(stderr, "Usage: human ml apply-adapter --adapter PATH [--id ID] "
                        "[--unload-after]\n");
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Default id = basename(PATH) so a typical run with just --adapter
     * produces a meaningful active_adapter() value. */
    char id_buf[64];
    if (!adapter_id) {
        const char *slash = strrchr(adapter_path, '/');
        const char *base = slash ? slash + 1 : adapter_path;
        size_t copy = strlen(base);
        if (copy >= sizeof(id_buf))
            copy = sizeof(id_buf) - 1;
        memcpy(id_buf, base, copy);
        id_buf[copy] = '\0';
        adapter_id = id_buf;
    }

    hu_huml_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    /* No checkpoint path: the provider's lazy-load path won't fire because
     * we're not calling chat. apply-adapter exists to verify the loop
     * mechanics; chat-time inference with the loaded adapter is Phase 4. */
    hu_provider_t prov;
    memset(&prov, 0, sizeof(prov));
    hu_error_t err = hu_huml_provider_create(alloc, &cfg, &prov);
    if (err != HU_OK) {
        fprintf(stderr, "huml provider create failed: %s\n", hu_error_string(err));
        return err;
    }

    err = hu_provider_load_adapter(&prov, alloc, adapter_path, strlen(adapter_path),
                                   adapter_id, strlen(adapter_id));
    if (err != HU_OK) {
        fprintf(stderr, "load_adapter failed: %s\n", hu_error_string(err));
        if (prov.vtable && prov.vtable->deinit)
            prov.vtable->deinit(prov.ctx, alloc);
        return err;
    }

    const char *active = hu_provider_active_adapter(&prov);
    printf("{\"loaded\":true,\"adapter_id\":\"%s\",\"adapter_path\":\"%s\"}\n",
           active ? active : "(null)", adapter_path);

    if (unload_after) {
        err = hu_provider_unload_adapter(&prov, adapter_id, strlen(adapter_id));
        if (err != HU_OK) {
            fprintf(stderr, "unload_adapter failed: %s\n", hu_error_string(err));
        } else {
            const char *after = hu_provider_active_adapter(&prov);
            printf("{\"unloaded\":true,\"adapter_id_after\":\"%s\"}\n",
                   after ? after : "(null)");
        }
    }

    if (prov.vtable && prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, alloc);
    return err;
}
