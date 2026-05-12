/* ML CLI subcommands: train, experiment, prepare, status, dpo-train, lora-persona. */

#include "human/ml/cli.h"
#include "human/ml/cli_dpo.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/ml/checkpoint.h"
#include "human/ml/dataloader.h"
#include "human/ml/fidelity.h"
#include "human/core/string.h"
#include "human/memory/graph.h"
#include "human/memory/memory.h"
#include "human/memory/personal_model.h"
#include "human/ml/dpo.h"
#include "human/ml/experiment.h"
#include "human/ml/learner.h"
#include "human/ml/lora.h"
#include "human/ml/m3_frontier_adapter.h"
#include "human/ml/ml.h"
#include "human/ml/model.h"
#include "human/ml/optimizer.h"
#include "human/ml/prepare.h"
#include "human/ml/tokenizer_ml.h"
#include "human/ml/train.h"
#include "human/ml/training_data_extractor.h"
#include "human/agent/scheduler_status_json.h"
#include "human/config.h"
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

/* Free a synthetic example array (incoming/response strdup'd by caller).
 * Safe to call with NULL/0 — used by lora-persona cleanup paths. */
static void free_delta_examples(hu_allocator_t *alloc, hu_persona_example_t *ex, size_t n) {
    if (!ex || n == 0)
        return;
    for (size_t i = 0; i < n; i++) {
        if (ex[i].incoming)
            alloc->free(alloc->ctx, ex[i].incoming, strlen(ex[i].incoming) + 1);
        if (ex[i].response)
            alloc->free(alloc->ctx, ex[i].response, strlen(ex[i].response) + 1);
    }
    alloc->free(alloc->ctx, ex, n * sizeof(*ex));
}

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

static int parse_float_arg(const char *val, float *out) {
    if (!val || !out)
        return -1;
    char *end = NULL;
    double d = strtod(val, &end);
    if (end == val || *end != '\0')
        return -1;
    *out = (float)d;
    return 0;
}

static const char *get_opt(const char **argv, int argc, int i, const char *opt) {
    if (i + 1 < argc && strcmp(argv[i], opt) == 0)
        return argv[i + 1];
    return NULL;
}

/* Phase 0 helper — load a BPE tokenizer using the project convention
 * (data_dir/tokenizer.vocab → ~/.human/models/tokenizer.vocab → default
 * 256-byte byte-level BPE) and derive the token_bytes table for BPB.
 *
 * On success, *out_tok and *out_token_bytes are owned by the caller and
 * must be freed with hu_bpe_tokenizer_deinit and alloc->free. *out_count
 * is the tokenizer's vocab_size — callers should align cfg.gpt.vocab_size
 * to this value before creating the GPT model so the model's vocab matches
 * the tokenizer's. See spec §1.5.2 issues #1, #2. */
static hu_error_t derive_token_bytes_for_data_dir(
    hu_allocator_t *alloc, const char *data_dir, hu_bpe_tokenizer_t **out_tok,
    int32_t **out_token_bytes, size_t *out_count) {
    if (!alloc || !out_tok || !out_token_bytes || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_tok = NULL;
    *out_token_bytes = NULL;
    *out_count = 0;

    hu_bpe_tokenizer_t *tok = NULL;
    hu_error_t err = hu_bpe_tokenizer_create(alloc, &tok);
    if (err != HU_OK)
        return err;

    char path[1024];
    int loaded = 0;
    if (data_dir && data_dir[0]) {
        int n = snprintf(path, sizeof(path), "%s/tokenizer.vocab", data_dir);
        if (n > 0 && (size_t)n < sizeof(path) &&
            hu_bpe_tokenizer_load(tok, path) == HU_OK) {
            loaded = 1;
        }
    }
    if (!loaded) {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            int n = snprintf(path, sizeof(path),
                             "%s/.human/models/tokenizer.vocab", home);
            if (n > 0 && (size_t)n < sizeof(path))
                (void)hu_bpe_tokenizer_load(tok, path);
        }
        /* On both failures, tok keeps its default 256-byte byte-level vocab
         * — every token is one byte, BPB is well-defined. */
    }

    int32_t *token_bytes = NULL;
    size_t count = 0;
    err = hu_ml_prepare_token_bytes(alloc, tok, &token_bytes, &count);
    if (err != HU_OK) {
        hu_bpe_tokenizer_deinit(tok);
        return err;
    }

    *out_tok = tok;
    *out_token_bytes = token_bytes;
    *out_count = count;
    return HU_OK;
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
    if ((dv = hu_json_get_number(root, "eval_tokens", 0.0)) > 0)
        cfg.training.eval_tokens = (size_t)dv;
    /* GPT model dimensions — useful for tiny CPU smoke tests.
     * Defaults (from hu_experiment_config_default) produce an 8-layer
     * 512-embd model that is too slow on CPU for a quick smoke. */
    if ((dv = hu_json_get_number(root, "n_layer", 0.0)) > 0)
        cfg.gpt.n_layer = (size_t)dv;
    if ((dv = hu_json_get_number(root, "n_head", 0.0)) > 0)
        cfg.gpt.n_head = (size_t)dv;
    if ((dv = hu_json_get_number(root, "n_kv_head", 0.0)) > 0)
        cfg.gpt.n_kv_head = (size_t)dv;
    if ((dv = hu_json_get_number(root, "n_embd", 0.0)) > 0)
        cfg.gpt.n_embd = (size_t)dv;
    if ((dv = hu_json_get_number(root, "head_dim", 0.0)) > 0)
        cfg.gpt.head_dim = (size_t)dv;
    if ((dv = hu_json_get_number(root, "sequence_len", 0.0)) > 0)
        cfg.gpt.sequence_len = (size_t)dv;

    v = hu_json_get_string(root, "checkpoint_path");
    if (v)
        cfg.training.checkpoint_path = v;

    /* Phase 0 fix — load tokenizer first so we can align cfg.gpt.vocab_size
     * to the tokenizer's actual vocab and pass real token_bytes to
     * hu_ml_train. Without this both args are NULL/0 and the CE objective
     * is degenerate (gpt_backward at src/ml/gpt.c:567 hard-fails on the
     * zero-shape grad tensor). See spec §1.5.2 issues #1, #2. */
    hu_bpe_tokenizer_t *tok = NULL;
    int32_t *token_bytes = NULL;
    size_t token_bytes_count = 0;
    hu_error_t err = derive_token_bytes_for_data_dir(alloc, data_dir, &tok,
                                                     &token_bytes, &token_bytes_count);
    if (err != HU_OK) {
        hu_json_free(alloc, root);
        hu_log_error("ml", NULL, "tokenizer load failed: %d", (int)err);
        return err;
    }
    cfg.gpt.vocab_size = token_bytes_count;

    hu_model_t model = {0};
    err = hu_gpt_create(alloc, &cfg.gpt, &model);
    if (err != HU_OK) {
        alloc->free(alloc->ctx, token_bytes, token_bytes_count * sizeof(int32_t));
        hu_bpe_tokenizer_deinit(tok);
        hu_json_free(alloc, root);
        hu_log_error("ml", NULL, "Model creation failed");
        return err;
    }

    hu_ml_optimizer_t optimizer = {0};
    err = hu_muon_adamw_create(alloc, &cfg.optimizer, &optimizer);
    if (err != HU_OK) {
        model.vtable->deinit(model.ctx, alloc);
        alloc->free(alloc->ctx, token_bytes, token_bytes_count * sizeof(int32_t));
        hu_bpe_tokenizer_deinit(tok);
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
        alloc->free(alloc->ctx, token_bytes, token_bytes_count * sizeof(int32_t));
        hu_bpe_tokenizer_deinit(tok);
        hu_json_free(alloc, root);
        hu_log_error("ml", NULL, "Dataloader creation failed for %s", data_dir);
        return err;
    }

    /* Phase 0 fix continued — also build a val_loader so hu_ml_train can
     * actually compute BPB at the end of the run. Without this val_bpb
     * stays 0.0 even when training succeeds (train.c:235 requires
     * val_loader && token_bytes). Falls back to NULL on missing val split. */
    hu_ml_dataloader_t *val_loader = NULL;
    if (cfg.training.eval_tokens > 0) {
        hu_error_t verr = hu_ml_dataloader_create(alloc, data_dir,
                                                  cfg.training.device_batch_size,
                                                  cfg.gpt.sequence_len, "val", &val_loader);
        if (verr != HU_OK) {
            hu_log_warn("ml", NULL,
                        "Val dataloader unavailable (%d) — BPB will be 0", (int)verr);
            val_loader = NULL;
        }
    }

    printf("Training: batch_size=%zu, max_steps=%zu, vocab_size=%zu, data=%s\n",
           cfg.training.device_batch_size, cfg.training.max_steps, cfg.gpt.vocab_size, data_dir);

    hu_ml_train_result_t result = {0};
    err = hu_ml_train(alloc, &model, &optimizer, train_loader, val_loader, &cfg.training,
                      token_bytes, token_bytes_count, &result);

    printf("Training %s: %zu steps, %.2f bpb, %.1fs\n", err == HU_OK ? "complete" : "failed",
           result.num_steps, result.val_bpb, result.training_seconds);

    if (val_loader)
        hu_ml_dataloader_deinit(val_loader);
    hu_ml_dataloader_deinit(train_loader);
    optimizer.vtable->deinit(optimizer.ctx, alloc);
    model.vtable->deinit(model.ctx, alloc);
    alloc->free(alloc->ctx, token_bytes, token_bytes_count * sizeof(int32_t));
    hu_bpe_tokenizer_deinit(tok);
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

/* Print the W14 scheduler snapshot the running daemon last persisted to
 * ~/.human/scheduler.status. Uses `hu_scheduler_status_parse_json` (same as
 * `human doctor scheduler`) so key order and whitespace never drift between tools. */
static void print_scheduler_status_block(void) {
    const char *home = getenv("HOME");
    if (!home || !*home)
        return;
    char path[512];
    int n = snprintf(path, sizeof(path), "%s/.human/scheduler.status", home);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char buf[4096];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = '\0';

    unsigned long long jp = 0, jc = 0;
    long long bat = 0, ue = 0;
    char ac[16] = {0};
    if (hu_scheduler_status_parse_json(buf, &jp, &jc, &bat, ac, sizeof(ac), &ue) != HU_OK) {
        printf("[w14-scheduler] (%s)\n"
               "  (unparseable status JSON — run `human doctor scheduler`)\n\n",
               path);
        return;
    }

    printf("[w14-scheduler] (%s)\n", path);
    printf("  jobs_pending:         %llu\n", (unsigned long long)jp);
    printf("  jobs_completed_today: %llu\n", (unsigned long long)jc);
    if (bat >= 0)
        printf("  battery_pct:          %lld%%\n", (long long)bat);
    else
        printf("  battery_pct:          (unknown)\n");
    printf("  on_ac_power:          %s\n", (ac[0] && strcmp(ac, "true") == 0) ? "yes" : "no");
    if (ue > 0) {
        time_t age_sec = time(NULL) - (time_t)ue;
        printf("  status_age:           %lds\n", (long)age_sec);
    }
    printf("\n");
}

hu_error_t hu_ml_cli_status(hu_allocator_t *alloc, int argc, const char **argv) {
    (void)alloc;
    (void)argc;
    (void)argv;
#ifdef HU_IS_TEST
    print_scheduler_status_block();
    printf("No experiments found\n");
    return HU_OK;
#else
    print_scheduler_status_block();
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

/* Phase 2 Task 8: hu_ml_cli_dpo_train's pre-Phase-2 body has been
 * extracted into hu_ml_cli_dpo_judge in src/ml/cli_dpo.c (the legacy
 * provider-scored path). The CLI verb `human ml dpo-train` now routes
 * through hu_ml_cli_dpo_real, which dispatches the new hu_rl_trainer_t
 * vtable from Tasks 1/4/6. The legacy semantics are still reachable via
 * `human ml dpo-judge` or `human ml dpo-train --legacy-judge`.
 *
 * This forwarder preserves the existing public symbol so any external
 * callers of hu_ml_cli_dpo_train (including the cmd_ml dispatch in
 * src/main.c, until that's updated) keep linking. */
hu_error_t hu_ml_cli_dpo_train(hu_allocator_t *alloc, int argc, const char **argv) {
    return hu_ml_cli_dpo_real(alloc, argc, argv);
}

hu_error_t hu_ml_cli_prepare_conversations(hu_allocator_t *alloc, int argc, const char **argv) {
    const char *chat_db = NULL;
    const char *memory_db = NULL;
    const char *output_dir = NULL;
    int correction_window = 0;
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
        v = get_opt(argv, argc, i, "--correction-window");
        if (v) {
            parse_int_arg(v, &correction_window);
            i++;
            continue;
        }
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: human ml prepare-conversations [--chat-db <path>] "
                   "[--memory-db <path>] --output <dir> "
                   "[--correction-window <secs>] [--help]\n");
            return HU_OK;
        }
    }
#ifdef HU_IS_TEST
    (void)alloc;
    (void)chat_db;
    (void)memory_db;
    (void)output_dir;
    (void)correction_window;
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

    /* Also run DPO pair extraction from user corrections. */
    {
        const char *db_path = memory_db ? memory_db : chat_db;
        if (!db_path) {
            char default_db[512];
            const char *home = getenv("HOME");
            if (home) {
                snprintf(default_db, sizeof(default_db), "%s/.human/memory.db", home);
                db_path = default_db;
            }
        }
        if (db_path) {
            size_t dpo_count = 0;
            hu_error_t dpo_err = hu_training_data_extract_dpo(
                alloc, db_path, correction_window, &dpo_count);
            if (dpo_err == HU_OK && dpo_count > 0)
                printf("[prepare-conversations] Extracted %zu DPO pairs from corrections\n",
                       dpo_count);
            else if (dpo_err == HU_OK)
                printf("[prepare-conversations] No new DPO pairs found\n");
            else if (dpo_err != HU_ERR_NOT_SUPPORTED)
                fprintf(stderr, "[prepare-conversations] DPO extraction warning: %d\n", dpo_err);
        }
    }

    return err;
#endif
}

hu_error_t hu_ml_cli_lora_persona(hu_allocator_t *alloc, int argc, const char **argv) {
    const char *persona_name = NULL;
    const char *checkpoint_path = NULL;
    const char *output_path = NULL;
    /* M3 Bridge A.0 — when set, skip training entirely and just export
     * the persona example bank as Alpaca JSONL. This is the on-ramp
     * users need to feed the bank into llama.cpp/finetune (or any
     * compatible toolchain) and produce a real GGUF LoRA adapter the
     * daemon's personalization block can load. No model is loaded;
     * the path is the only side effect. */
    const char *export_jsonl_path = NULL;
    /* Phase A1.3 — when set, derive per-channel example banks from
     * the user's conversation history before any export/training step.
     * Combined with --export-jsonl, this is the one-command path:
     *   history db → quality-filtered banks → Alpaca JSONL
     * with no hand-authored bank required. The derived banks REPLACE
     * any existing banks on the loaded persona for the duration of
     * this CLI invocation; the persona file on disk is not touched. */
    const char *from_history_db = NULL;
    /* Per-channel cap for --from-history. 0 means use the default
     * (HU_PERSONA_BANKS_FROM_HIST_DEFAULT_MAX_PER_CHANNEL = 32). */
    int from_history_max_per_channel = 0;
    /* Phase A1.4 — when true and --from-history derived new banks,
     * write the modified persona back to disk via
     * hu_persona_creator_write. Without --persist the derived banks
     * exist only for the duration of this invocation; with it they
     * survive into the next daemon start and the next prompt build. */
    bool persist_persona = false;
    /* P0 #3 — signal-builder wiring. When --from-deltas is set, we
     * read applied persona deltas from a memory DB and treat them as
     * additional training examples. The previously-dead
     * `hu_learner_signals_from_persona_deltas` builder becomes a real
     * data source for LoRA. */
    const char *from_deltas_db = NULL;
    const char *signal_contact = NULL;
    /* Bridge B — MLX frontier LoRA. When --backend mlx is set, we skip
     * in-process HUML training and shell out to mlx_lm.lora for real
     * Gemma fine-tuning on Apple Silicon. */
    const char *backend = NULL;
    const char *mlx_model = NULL;
    const char *data_dir = NULL;
    int num_layers = 8;
    int max_seq_length = 2048;
    int save_every = 100;
    float learning_rate = 0.0f;
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
        v = get_opt(argv, argc, i, "--from-deltas");
        if (v) {
            from_deltas_db = v;
            i++;
            continue;
        }
        v = get_opt(argv, argc, i, "--contact");
        if (v) {
            signal_contact = v;
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
        v = get_opt(argv, argc, i, "--export-jsonl");
        if (v) {
            export_jsonl_path = v;
            i++;
            continue;
        }
        v = get_opt(argv, argc, i, "--from-history");
        if (v) {
            from_history_db = v;
            i++;
            continue;
        }
        v = get_opt(argv, argc, i, "--from-history-max");
        if (v) {
            parse_int_arg(v, &from_history_max_per_channel);
            i++;
            continue;
        }
        if (strcmp(argv[i], "--persist") == 0) {
            persist_persona = true;
            i++;
            continue;
        }
        v = get_opt(argv, argc, i, "--backend");
        if (v) {
            backend = v;
            i++;
            continue;
        }
        v = get_opt(argv, argc, i, "--model");
        if (v) {
            mlx_model = v;
            i++;
            continue;
        }
        v = get_opt(argv, argc, i, "--data-dir");
        if (v) {
            data_dir = v;
            i++;
            continue;
        }
        v = get_opt(argv, argc, i, "--num-layers");
        if (v) {
            parse_int_arg(v, &num_layers);
            i++;
            continue;
        }
        v = get_opt(argv, argc, i, "--max-seq-length");
        if (v) {
            parse_int_arg(v, &max_seq_length);
            i++;
            continue;
        }
        v = get_opt(argv, argc, i, "--save-every");
        if (v) {
            parse_int_arg(v, &save_every);
            i++;
            continue;
        }
        v = get_opt(argv, argc, i, "--learning-rate");
        if (v) {
            parse_float_arg(v, &learning_rate);
            i++;
            continue;
        }
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: human ml lora-persona --persona <name> "
                   "[--checkpoint <path>] [--output <path>] "
                   "[--from-deltas <memory.db> --contact <id>] "
                   "[--from-history <memory.db>] "
                   "[--from-history-max <N>] "
                   "[--persist] "
                   "[--rank <N>] [--max-steps <N>] "
                   "[--export-jsonl <path>] "
                   "[--backend mlx] [--model <hf-id>] [--data-dir <path>] "
                   "[--num-layers <N>] [--max-seq-length <N>] "
                   "[--save-every <N>] [--learning-rate <f>] [--help]\n");
            printf("\n  --checkpoint   Optional HUML base GPT checkpoint to warm-start "
                   "before LoRA attaches.\n");
            printf("                 Must match the reference GPT dims (see "
                   "hu_experiment_config_default).\n");
            printf("                 NOTE: this is the reference HUML GPT, NOT a "
                   "frontier model.\n");
            printf("                 Frontier model fine-tuning is tracked in\n");
            printf("                 %s\n", hu_ml_lora_persona_caveat_doc_path());
            printf("\n  --from-deltas  Augment the persona example bank with applied\n");
            printf("                 persona deltas read from a memory database.\n");
            printf("                 Requires --contact to scope the read.\n");
            printf("\n  --export-jsonl Skip training entirely; export the persona's\n");
            printf("                 example banks to <path> as Alpaca JSONL\n");
            printf("                 ({\"instruction\":..,\"input\":..,\"output\":..}).\n");
            printf("                 Use this to feed the bank into llama.cpp/finetune,\n");
            printf("                 axolotl, unsloth, or mlx-lm.lora and produce a\n");
            printf("                 GGUF LoRA the daemon can load via\n");
            printf("                 personalization.lora_adapter_path.\n");
            printf("\n  --from-history Derive per-channel example banks from the\n");
            printf("                 user's conversation history (the messages\n");
            printf("                 table in <memory.db>). Pairs are PII-redacted,\n");
            printf("                 quality-filtered (length / Shannon entropy /\n");
            printf("                 unique-byte ratio), and within-run deduplicated\n");
            printf("                 before they enter the bank. Replaces the\n");
            printf("                 persona's loaded banks for this run only;\n");
            printf("                 the persona JSON on disk is not modified.\n");
            printf("  --from-history-max  Per-channel example cap (default 32).\n");
            printf("\n  --persist      Write the modified persona (with --from-history-\n");
            printf("                 derived banks) back to ~/.human/personas/<name>.json\n");
            printf("                 so it survives this CLI invocation. Without\n");
            printf("                 --persist the derived banks are scratch state\n");
            printf("                 and the persona JSON on disk is not touched.\n");
            printf("\n  --backend mlx  Use the MLX backend for frontier Gemma LoRA\n");
            printf("                 fine-tuning on Apple Silicon. Shells out to\n");
            printf("                 python3 -m mlx_lm.lora.\n");
            printf("  --model <id>   HF model ID or local path for --backend mlx.\n");
            printf("                 Default: mlx-community/gemma-4-31b-it-4bit\n");
            printf("  --data-dir     Pre-existing JSONL data directory with\n");
            printf("                 train.jsonl/valid.jsonl. If omitted, persona\n");
            printf("                 example bank is exported automatically.\n");
            printf("  --num-layers   Number of model layers to fine-tune (default 8).\n");
            printf("  --max-seq-length  Max sequence length (default 2048).\n");
            printf("  --save-every   Checkpoint save frequency (default 100).\n");
            printf("  --learning-rate  Learning rate (default 1e-5 for mlx).\n");
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
    (void)export_jsonl_path;
    (void)from_history_db;
    (void)from_history_max_per_channel;
    (void)persist_persona;
    (void)backend;
    (void)mlx_model;
    (void)data_dir;
    (void)num_layers;
    (void)max_seq_length;
    (void)save_every;
    (void)learning_rate;
    printf("[lora-persona] test mode: skipped\n");
    printf("[lora-persona] honest-gap doc: %s\n", hu_ml_lora_persona_caveat_doc_path());
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

    /* Phase A1.3 — derive banks from conversation history. Runs upstream
     * of every other path (--export-jsonl, --backend mlx, in-process
     * training) so a single invocation can go history → JSONL with no
     * hand-authored persona bank. The derived banks REPLACE whatever
     * was loaded from the persona JSON for this run only; the on-disk
     * persona file is untouched. */
    if (from_history_db && from_history_db[0]) {
        hu_persona_example_bank_t *new_banks = NULL;
        size_t new_count = 0;
        hu_error_t hist_err = hu_persona_banks_extract_from_history(
            alloc, from_history_db,
            (size_t)from_history_max_per_channel,
            &new_banks, &new_count);
        if (hist_err != HU_OK) {
            fprintf(stderr, "[lora-persona] --from-history failed (%d): %s\n",
                    hist_err, hu_error_string(hist_err));
            hu_persona_deinit(alloc, &persona);
            return hist_err;
        }

        /* Drop the previously-loaded banks (allocator-owned by `alloc`,
         * same as the new ones we're about to install). The free
         * function is null-safe; an empty persona is fine here. */
        hu_persona_example_banks_free(alloc, persona.example_banks,
                                      persona.example_banks_count);
        persona.example_banks = new_banks;
        persona.example_banks_count = new_count;

        size_t total_examples = 0;
        for (size_t i = 0; i < new_count; i++)
            total_examples += new_banks[i].examples_count;
        printf("[lora-persona] --from-history: derived %zu example(s) across "
               "%zu channel(s) from %s\n",
               total_examples, new_count, from_history_db);

        /* Phase A1.4 — persist derived banks to disk so they survive
         * the next daemon start. Without --persist these are scratch
         * state and disappear at process exit. We only call the
         * writer when --persist is set AND banks were actually derived
         * (new_count > 0) so the user's hand-authored persona JSON is
         * never overwritten with an empty banks array. */
        if (persist_persona && new_count > 0) {
            hu_error_t save_err = hu_persona_creator_write(alloc, &persona);
            if (save_err != HU_OK) {
                fprintf(stderr,
                        "[lora-persona] --persist write failed (%d): %s\n",
                        save_err, hu_error_string(save_err));
                hu_persona_deinit(alloc, &persona);
                return save_err;
            }
            printf("[lora-persona] --persist: wrote %zu bank(s) to "
                   "~/.human/personas/%s.json\n",
                   new_count, persona_name);
        } else if (persist_persona) {
            printf("[lora-persona] --persist: no banks derived "
                   "(history empty after quality gates); persona JSON "
                   "left untouched.\n");
        }
    }

    /* M3 Bridge A.0 — export-only path. No model is loaded, no training
     * runs; we just write the bank as Alpaca JSONL and exit. This is
     * deliberately kept upstream of the training-loop setup so users
     * can pull a JSONL on any machine without a HUML checkpoint or ML
     * link dependencies surfacing. */
    if (export_jsonl_path && export_jsonl_path[0]) {
        size_t exported = 0;
        hu_error_t exp_err = hu_persona_bank_export_jsonl(&persona, export_jsonl_path,
                                                           strlen(export_jsonl_path),
                                                           &exported);
        if (exp_err != HU_OK) {
            fprintf(stderr, "[lora-persona] export failed (%d): %s\n", exp_err,
                    hu_error_string(exp_err));
            hu_persona_deinit(alloc, &persona);
            return exp_err;
        }
        printf("[lora-persona] exported %zu example(s) to %s (Alpaca JSONL)\n",
               exported, export_jsonl_path);
        printf("[lora-persona] feed into llama.cpp/finetune, axolotl, unsloth,\n");
        printf("               or mlx-lm.lora to produce a GGUF LoRA the daemon\n");
        printf("               can load via personalization.lora_adapter_path.\n");
        hu_persona_deinit(alloc, &persona);
        return HU_OK;
    }

    /* ── Bridge B — MLX frontier LoRA training ─────────────────────────
     * When --backend mlx is set, we bypass the in-process HUML training
     * loop entirely and use the MLX learner backend to shell out to
     * `python3 -m mlx_lm.lora` for real Gemma fine-tuning. */
    if (backend && strcmp(backend, "mlx") == 0) {
#ifdef HU_ENABLE_LEARNING
        hu_learner_t *learner = NULL;
        hu_error_t lerr = hu_learner_open_named(alloc, "mlx", &learner);
        if (lerr != HU_OK) {
            fprintf(stderr, "[lora-persona] MLX backend not available on this platform.\n");
            if (lerr == HU_ERR_NOT_SUPPORTED)
                fprintf(stderr, "              Requires macOS on Apple Silicon with "
                                "mlx_lm installed.\n"
                                "              Install: pip install mlx-lm\n");
            hu_persona_deinit(alloc, &persona);
            return lerr;
        }

        hu_learner_config_t lcfg = hu_learner_default_config();
        snprintf(lcfg.base_model_path, sizeof(lcfg.base_model_path), "%s",
                 mlx_model ? mlx_model : "mlx-community/gemma-4-31b-it-4bit");
        lcfg.rank = rank;
        lcfg.max_steps = max_steps;
        lcfg.learning_rate = learning_rate > 0.0f ? learning_rate : 1e-5f;
        lcfg.batch_size = 1;
        lcfg.num_layers = num_layers;
        lcfg.max_seq_length = max_seq_length;
        lcfg.save_every = save_every;
        lcfg.budget_ms = -1;

        char default_adapter[512];
        if (output_path) {
            snprintf(lcfg.adapter_output_path, sizeof(lcfg.adapter_output_path),
                     "%s", output_path);
        } else {
            const char *home = getenv("HOME");
            snprintf(default_adapter, sizeof(default_adapter),
                     "%s/.human/training-data/adapters/lora-persona-%s",
                     home ? home : ".", persona_name);
            snprintf(lcfg.adapter_output_path, sizeof(lcfg.adapter_output_path),
                     "%s", default_adapter);
        }

        if (data_dir) {
            snprintf(lcfg.data_dir, sizeof(lcfg.data_dir), "%s", data_dir);
        } else {
            char tmp_dir[256];
            snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/hu-lora-frontier-%d", (int)getpid());
            mkdir(tmp_dir, 0755);
            char train_path[512];
            snprintf(train_path, sizeof(train_path), "%s/train.jsonl", tmp_dir);
            size_t exported = 0;
            hu_error_t exp_err = hu_persona_bank_export_jsonl(
                &persona, train_path, strlen(train_path), &exported);
            if (exp_err != HU_OK || exported == 0) {
                fprintf(stderr, "[lora-persona] failed to export persona bank "
                                "for frontier training: %s\n",
                        hu_error_string(exp_err != HU_OK ? exp_err
                                                         : HU_ERR_INVALID_ARGUMENT));
                hu_learner_close(learner);
                hu_persona_deinit(alloc, &persona);
                return exp_err != HU_OK ? exp_err : HU_ERR_INVALID_ARGUMENT;
            }
            snprintf(lcfg.data_dir, sizeof(lcfg.data_dir), "%s", tmp_dir);
            printf("[lora-persona] exported %zu persona example(s) to %s\n",
                   exported, train_path);
        }

        printf("[lora-persona] MLX frontier LoRA training:\n"
               "  model:          %s\n"
               "  data:           %s\n"
               "  adapter:        %s\n"
               "  rank:           %d\n"
               "  iters:          %d\n"
               "  num-layers:     %d\n"
               "  max-seq-length: %d\n"
               "  learning-rate:  %g\n"
               "  batch-size:     %d\n",
               lcfg.base_model_path, lcfg.data_dir, lcfg.adapter_output_path,
               lcfg.rank, lcfg.max_steps, lcfg.num_layers, lcfg.max_seq_length,
               (double)lcfg.learning_rate, lcfg.batch_size);

        hu_learner_report_t report;
        memset(&report, 0, sizeof(report));
        err = hu_learner_train(learner, &lcfg, NULL, 0, &report);
        if (err == HU_OK) {
            printf("[lora-persona] frontier training complete:\n"
                   "  steps:     %zu\n"
                   "  loss:      %.4f\n"
                   "  adapter:   %s\n"
                   "  size:      %lld bytes\n",
                   report.steps_completed, (double)report.final_loss,
                   report.adapter_path, (long long)report.adapter_bytes);
        } else {
            fprintf(stderr, "[lora-persona] frontier training failed: %s\n",
                    report.last_error[0] ? report.last_error : hu_error_string(err));
        }

        if (!data_dir) {
            char rm_path[512];
            snprintf(rm_path, sizeof(rm_path), "%s/train.jsonl", lcfg.data_dir);
            unlink(rm_path);
            rmdir(lcfg.data_dir);
        }

        hu_learner_close(learner);
        hu_persona_deinit(alloc, &persona);
        return err;
#else
        (void)mlx_model;
        (void)data_dir;
        (void)num_layers;
        (void)max_seq_length;
        (void)save_every;
        (void)learning_rate;
        fprintf(stderr, "[lora-persona] --backend mlx requires HU_ENABLE_LEARNING=ON\n");
        hu_persona_deinit(alloc, &persona);
        return HU_ERR_NOT_SUPPORTED;
#endif
    }

    /* P0 #3 — pull applied persona deltas as additional training pairs.
     * Each delta becomes one synthetic (incoming, response) example:
     *   incoming = "[<channel>] <kind>"  (e.g. "[slack] tone")
     *   response = delta.value           (the new style instruction)
     * We hold the synthesized examples in `delta_examples` so the
     * standard training loop can iterate them just like persona-bank
     * examples; they're freed at the end alongside the persona. */
    hu_persona_example_t *delta_examples = NULL;
    size_t delta_examples_count = 0;
    if (from_deltas_db && from_deltas_db[0]) {
#ifdef HU_ENABLE_SQLITE
        if (!signal_contact) {
            fprintf(stderr,
                    "[lora-persona] --from-deltas requires --contact <id>\n");
            hu_persona_deinit(alloc, &persona);
            return HU_ERR_INVALID_ARGUMENT;
        }
        hu_graph_t *signal_graph = NULL;
        hu_memory_facade_t *signal_mem = NULL;
        hu_error_t s_err =
            hu_graph_open(alloc, from_deltas_db, strlen(from_deltas_db), &signal_graph);
        if (s_err != HU_OK) {
            fprintf(stderr,
                    "[lora-persona] failed to open --from-deltas db '%s': %d\n",
                    from_deltas_db, s_err);
            hu_persona_deinit(alloc, &persona);
            return s_err;
        }
        s_err = hu_memory_facade_open(alloc, signal_graph, &signal_mem);
        if (s_err != HU_OK) {
            hu_graph_close(signal_graph, alloc);
            hu_persona_deinit(alloc, &persona);
            return s_err;
        }
        hu_training_signal_t *signals = NULL;
        size_t signals_n = 0;
        s_err = hu_learner_signals_from_persona_deltas(
            signal_mem, alloc, signal_contact, strlen(signal_contact), &signals, &signals_n);
        if (s_err == HU_OK && signals_n > 0) {
            delta_examples =
                (hu_persona_example_t *)alloc->alloc(alloc->ctx, signals_n * sizeof(*delta_examples));
            if (!delta_examples) {
                hu_learner_signals_free(alloc, signals, signals_n);
                hu_memory_facade_close(signal_mem, alloc);
                hu_graph_close(signal_graph, alloc);
                hu_persona_deinit(alloc, &persona);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memset(delta_examples, 0, signals_n * sizeof(*delta_examples));
            for (size_t i = 0; i < signals_n; i++) {
                if (signals[i].kind != HU_TRAIN_PERSONA_DELTA)
                    continue;
                const hu_persona_delta_t *d = &signals[i].as.persona.delta;
                if (d->value[0] == '\0')
                    continue;
                char in_buf[256];
                const char *kind_str = "tone";
                switch (d->kind) {
                case HU_PERSONA_DELTA_TONE:        kind_str = "tone"; break;
                case HU_PERSONA_DELTA_VOCAB_AVOID: kind_str = "vocab-avoid"; break;
                case HU_PERSONA_DELTA_LENGTH:      kind_str = "length"; break;
                case HU_PERSONA_DELTA_BOUNDARY:    kind_str = "boundary"; break;
                default: kind_str = "style"; break;
                }
                snprintf(in_buf, sizeof(in_buf), "[%s] %s", d->key[0] ? d->key : "any",
                         kind_str);
                delta_examples[delta_examples_count].incoming =
                    hu_strndup(alloc, in_buf, strlen(in_buf));
                delta_examples[delta_examples_count].response =
                    hu_strndup(alloc, d->value, strlen(d->value));
                delta_examples_count++;
            }
        }
        hu_learner_signals_free(alloc, signals, signals_n);
        hu_memory_facade_close(signal_mem, alloc);
        hu_graph_close(signal_graph, alloc);
#else
        fprintf(stderr,
                "[lora-persona] --from-deltas requires HU_ENABLE_SQLITE\n");
        hu_persona_deinit(alloc, &persona);
        return HU_ERR_NOT_SUPPORTED;
#endif
    }

    size_t total_examples = 0;
    for (size_t b = 0; b < persona.example_banks_count; b++)
        total_examples += persona.example_banks[b].examples_count;
    total_examples += delta_examples_count;

    if (total_examples == 0) {
        fprintf(stderr, "Persona '%s' has no example banks to train on\n", persona_name);
        free_delta_examples(alloc, delta_examples, delta_examples_count);
        hu_persona_deinit(alloc, &persona);
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (delta_examples_count > 0)
        printf("[lora-persona] augmented with %zu applied-delta example pairs\n",
               delta_examples_count);

    hu_experiment_config_t cfg = hu_experiment_config_default();

    hu_model_t model = {0};
    err = hu_gpt_create(alloc, &cfg.gpt, &model);
    if (err != HU_OK) {
        fprintf(stderr, "Model creation failed: %d\n", err);
        free_delta_examples(alloc, delta_examples, delta_examples_count);
        hu_persona_deinit(alloc, &persona);
        return err;
    }

    /* Honest caveat — this trains a LoRA adapter on the reference HUML GPT,
     * NOT on a frontier model. Source of truth in
     * src/ml/m3_frontier_adapter.c, pinned by tests in test_ml.c. */
    fputs(hu_ml_lora_persona_caveat_block(), stdout);

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
            free_delta_examples(alloc, delta_examples, delta_examples_count);
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
            free_delta_examples(alloc, delta_examples, delta_examples_count);
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
        free_delta_examples(alloc, delta_examples, delta_examples_count);
        hu_persona_deinit(alloc, &persona);
        return err;
    }

    /* Attach LoRA to Q and V projections */
    err = hu_gpt_attach_lora(&model, adapter, NULL, adapter, NULL, NULL, NULL);
    if (err != HU_OK) {
        fprintf(stderr, "LoRA attach failed: %d\n", err);
        hu_lora_destroy(alloc, adapter);
        model.vtable->deinit(model.ctx, alloc);
        free_delta_examples(alloc, delta_examples, delta_examples_count);
        hu_persona_deinit(alloc, &persona);
        return err;
    }

    hu_ml_optimizer_t optimizer = {0};
    err = hu_muon_adamw_create(alloc, &cfg.optimizer, &optimizer);
    if (err != HU_OK) {
        hu_lora_destroy(alloc, adapter);
        model.vtable->deinit(model.ctx, alloc);
        free_delta_examples(alloc, delta_examples, delta_examples_count);
        hu_persona_deinit(alloc, &persona);
        return err;
    }

    err = hu_lora_register_params(adapter, &optimizer);
    if (err != HU_OK) {
        optimizer.vtable->deinit(optimizer.ctx, alloc);
        hu_lora_destroy(alloc, adapter);
        model.vtable->deinit(model.ctx, alloc);
        free_delta_examples(alloc, delta_examples, delta_examples_count);
        hu_persona_deinit(alloc, &persona);
        return err;
    }

    hu_bpe_tokenizer_t *tok = NULL;
    err = hu_bpe_tokenizer_create(alloc, &tok);
    if (err != HU_OK) {
        optimizer.vtable->deinit(optimizer.ctx, alloc);
        hu_lora_destroy(alloc, adapter);
        model.vtable->deinit(model.ctx, alloc);
        free_delta_examples(alloc, delta_examples, delta_examples_count);
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

        /* Iterate persona example banks in pass 0, delta-derived
         * examples in pass 1. Same training body for both. */
        for (int pass = 0; pass < 2; pass++) {
        size_t pass_count = pass == 0 ? persona.example_banks_count : delta_examples_count;
        for (size_t b = 0; b < pass_count; b++) {
            size_t inner = pass == 0 ? persona.example_banks[b].examples_count : 1;
            for (size_t e = 0; e < inner; e++) {
                const hu_persona_example_t *ex = pass == 0 ? &persona.example_banks[b].examples[e]
                                                            : &delta_examples[b];
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
        } /* pass */

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
    free_delta_examples(alloc, delta_examples, delta_examples_count);
    hu_persona_deinit(alloc, &persona);
    return err;
#endif
}

/* Track D D2.2 — offline persona-fidelity baseline.
 *
 * Builds a synthetic communication-style fingerprint from the
 * persona's overlay hints (when present) or falls back to the
 * personal model on disk, then scores every response in the
 * persona's example_banks against the fingerprint. Reports per-
 * example and aggregate fidelity in [0,1].
 *
 * The point of this command is NOT to grade the persona — it's to
 * establish a baseline number that future LoRA-vs-baseline runs can
 * compare against. The number reported here is the upper bound a
 * frontier model can plausibly achieve without any personalization
 * (the responses in the bank were authored to match the persona),
 * so a post-LoRA score above this baseline means the adapter is
 * actively pulling the model toward persona fidelity. */
hu_error_t hu_ml_cli_lora_baseline(hu_allocator_t *alloc, int argc, const char **argv) {
    const char *persona_name = NULL;
    for (int i = 1; i < argc; i++) {
        const char *v = get_opt(argv, argc, i, "--persona");
        if (v) {
            persona_name = v;
            i++;
            continue;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: human ml lora-baseline --persona <name>\n\n"
                   "Score a persona's example bank against its communication-\n"
                   "style fingerprint. Pure CPU, no provider call. Reports\n"
                   "mean / min / max fidelity in [0,1] as a baseline for\n"
                   "future LoRA A/B comparisons.\n\n"
                   "  --persona <name>  Persona name in ~/.human/personas/\n");
            printf("\nDoc: %s\n", hu_ml_lora_persona_caveat_doc_path());
            return HU_OK;
        }
    }
    if (!persona_name) {
        fprintf(stderr, "lora-baseline requires --persona <name>\n");
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_persona_t persona = {0};
    hu_error_t err = hu_persona_load(alloc, persona_name, strlen(persona_name), &persona);
    if (err != HU_OK) {
        fprintf(stderr, "[lora-baseline] failed to load persona '%s': %d\n", persona_name, err);
        return err;
    }

    /* Synthesize a communication-style fingerprint. We try the
     * personal_model on disk first (real EWMA from observed user
     * messages); if that's unavailable, fall back to a permissive
     * default that matches "casual lowercase chat" — enough to
     * exercise the scorer end-to-end without claiming the score
     * means the user's actual register. */
    hu_communication_style_t target;
    memset(&target, 0, sizeof(target));
    bool synthetic_fingerprint = true;
    {
        char pm_path[1024];
        if (hu_personal_model_resolve_default_path(pm_path, sizeof(pm_path))) {
            hu_personal_model_t loaded;
            if (hu_personal_model_load(&loaded, pm_path) == HU_OK &&
                loaded.style.sample_count > 0U) {
                target = loaded.style;
                synthetic_fingerprint = false;
            }
        }
    }
    if (synthetic_fingerprint) {
        target.formality = 0.3f;
        target.verbosity = 0.5f;
        target.emoji_frequency = 0.2f;
        target.humor_receptivity = 0.6f;
        target.lowercase_ratio = 0.85f;
        target.abbreviation_ratio = 0.2f;
        target.avg_message_length = 60;
        target.sample_count = 1; /* tip the "no fingerprint" guard. */
        printf("[lora-baseline] no personal_model.bin found — using synthetic fingerprint.\n");
    } else {
        printf("[lora-baseline] fingerprint loaded from personal model "
               "(samples=%u, avg_len=%u).\n",
               (unsigned)target.sample_count, (unsigned)target.avg_message_length);
    }

    /* Walk every example response in every bank, score each, and
     * accumulate. Empty / NULL responses are skipped so they don't
     * drag the mean toward 0. */
    size_t scored = 0;
    float sum = 0.f;
    float min_score = 1.0f, max_score = 0.0f;
    for (size_t b = 0; b < persona.example_banks_count; b++) {
        const hu_persona_example_bank_t *bank = &persona.example_banks[b];
        for (size_t i = 0; i < bank->examples_count; i++) {
            const hu_persona_example_t *ex = &bank->examples[i];
            if (!ex->response || ex->response[0] == '\0') continue;
            float s = hu_communication_style_fidelity_score(&target, ex->response,
                                                            strlen(ex->response));
            if (s < 0.f) continue;
            sum += s;
            if (s < min_score) min_score = s;
            if (s > max_score) max_score = s;
            scored++;
        }
    }

    if (scored == 0) {
        printf("[lora-baseline] persona '%s' has no scoreable example responses.\n",
               persona_name);
        hu_persona_deinit(alloc, &persona);
        return HU_OK;
    }

    float mean = sum / (float)scored;
    printf("[lora-baseline] persona '%s' baseline fidelity:\n"
           "[lora-baseline]   examples scored: %zu\n"
           "[lora-baseline]   mean:            %.3f\n"
           "[lora-baseline]   min:             %.3f\n"
           "[lora-baseline]   max:             %.3f\n",
           persona_name, scored, mean, min_score, max_score);
    printf("[lora-baseline] interpretation: scores in [0,1]; the mean is the\n"
           "[lora-baseline]   upper bound a frontier model can plausibly hit\n"
           "[lora-baseline]   without LoRA, since the bank's responses were\n"
           "[lora-baseline]   authored to match the persona. A post-LoRA mean\n"
           "[lora-baseline]   above this number indicates the adapter is\n"
           "[lora-baseline]   actively personalizing.\n");
    printf("[lora-baseline] doc: %s\n", hu_ml_lora_persona_caveat_doc_path());

    hu_persona_deinit(alloc, &persona);
    return HU_OK;
}

/* Track D D2.2 — A/B comparator helpers + CLI. */

/* Read an entire file into a freshly-allocated buffer. Caller frees
 * with `alloc->free`. Caps file size at 16 MiB to avoid runaway
 * loads from a typo'd path pointing at /dev/zero or similar. */
static hu_error_t hu_ml_lora_ab_read_file(hu_allocator_t *alloc, const char *path, char **out_buf,
                                          size_t *out_len) {
    *out_buf = NULL;
    *out_len = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return HU_ERR_NOT_FOUND;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return HU_ERR_IO;
    }
    long sz = ftell(fp);
    if (sz < 0 || sz > (long)(16L * 1024L * 1024L)) {
        fclose(fp);
        return sz < 0 ? HU_ERR_IO : HU_ERR_INVALID_ARGUMENT;
    }
    rewind(fp);
    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t read = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (read != (size_t)sz) {
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);
        return HU_ERR_IO;
    }
    buf[sz] = '\0';
    *out_buf = buf;
    *out_len = (size_t)sz;
    return HU_OK;
}

/* Parse a JSON file containing a top-level array of strings into
 * three parallel allocations: `*out_strings` (pointer array),
 * `*out_lens` (length array), `*out_count`. The strings alias
 * into the parsed JSON tree, which the caller must keep alive
 * via `*out_json` until the comparator is done. Caller frees
 * `*out_strings` and `*out_lens` with `alloc->free`, and
 * `*out_json` with `hu_json_free`. */
static hu_error_t hu_ml_lora_ab_load_response_set(hu_allocator_t *alloc, const char *path,
                                                  hu_json_value_t **out_json,
                                                  const char ***out_strings, size_t **out_lens,
                                                  size_t *out_count) {
    *out_json = NULL;
    *out_strings = NULL;
    *out_lens = NULL;
    *out_count = 0;

    char *raw = NULL;
    size_t raw_len = 0;
    hu_error_t err = hu_ml_lora_ab_read_file(alloc, path, &raw, &raw_len);
    if (err != HU_OK)
        return err;

    hu_json_value_t *root = NULL;
    err = hu_json_parse(alloc, raw, raw_len, &root);
    /* Free the raw buffer regardless — the JSON parser copies
     * string contents into its own allocations. */
    alloc->free(alloc->ctx, raw, raw_len + 1);
    if (err != HU_OK)
        return err;
    if (!root || root->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }

    size_t n = root->data.array.len;
    if (n == 0) {
        /* Empty array is technically valid; report count=0 and
         * keep the JSON tree so the caller can still call deinit. */
        *out_json = root;
        return HU_OK;
    }

    const char **strings = (const char **)alloc->alloc(alloc->ctx, n * sizeof(const char *));
    size_t *lens = (size_t *)alloc->alloc(alloc->ctx, n * sizeof(size_t));
    if (!strings || !lens) {
        if (strings) alloc->free(alloc->ctx, strings, n * sizeof(const char *));
        if (lens) alloc->free(alloc->ctx, lens, n * sizeof(size_t));
        hu_json_free(alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < n; i++) {
        const hu_json_value_t *item = root->data.array.items[i];
        if (!item || item->type != HU_JSON_STRING) {
            /* Non-string entries are kept as NULL/0; the
             * comparator will count them as `skipped`. */
            strings[i] = NULL;
            lens[i] = 0;
            continue;
        }
        strings[i] = item->data.string.ptr;
        lens[i] = item->data.string.len;
    }
    *out_json = root;
    *out_strings = strings;
    *out_lens = lens;
    *out_count = n;
    return HU_OK;
}

hu_error_t hu_ml_cli_lora_ab(hu_allocator_t *alloc, int argc, const char **argv) {
    const char *persona_name = NULL;
    const char *before_path = NULL;
    const char *after_path = NULL;
    float floor_delta = 0.f;
    bool require_positive = false;

    for (int i = 1; i < argc; i++) {
        const char *v = get_opt(argv, argc, i, "--persona");
        if (v) { persona_name = v; i++; continue; }
        v = get_opt(argv, argc, i, "--before");
        if (v) { before_path = v; i++; continue; }
        v = get_opt(argv, argc, i, "--after");
        if (v) { after_path = v; i++; continue; }
        v = get_opt(argv, argc, i, "--floor-delta");
        if (v) { floor_delta = (float)strtod(v, NULL); i++; continue; }
        if (strcmp(argv[i], "--require-positive") == 0) {
            require_positive = true;
            continue;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: human ml lora-ab --persona <name> --before <pre.json> --after <post.json>\n"
                   "                       [--floor-delta F] [--require-positive]\n\n"
                   "Score two response sets against a persona's communication-\n"
                   "style fingerprint and report the mean fidelity delta\n"
                   "(after - before). A positive delta indicates the LoRA\n"
                   "adapter is pulling the model toward persona fidelity.\n\n"
                   "Inputs are JSON files containing a top-level array of\n"
                   "response strings, e.g.\n"
                   "  [\"hey, sounds good\", \"yeah totally lmk\", ...]\n\n"
                   "Options:\n"
                   "  --persona <name>      Persona name in ~/.human/personas/\n"
                   "  --before <path>       JSON array of pre-LoRA responses\n"
                   "  --after <path>        JSON array of post-LoRA responses\n"
                   "  --floor-delta F       Exit non-zero when delta < F\n"
                   "  --require-positive    Exit non-zero when delta <= 0\n");
            printf("\nDoc: %s\n", hu_ml_lora_persona_caveat_doc_path());
            return HU_OK;
        }
    }
    if (!persona_name || !before_path || !after_path) {
        fprintf(stderr,
                "lora-ab requires --persona <name> --before <path> --after <path>\n");
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Load the persona to confirm it exists (and so future tooling
     * can use the example bank as a default fingerprint source).
     * The fingerprint itself comes from personal_model.bin or the
     * synthetic fallback, same as `lora-baseline`. */
    hu_persona_t persona = {0};
    hu_error_t err = hu_persona_load(alloc, persona_name, strlen(persona_name), &persona);
    if (err != HU_OK) {
        fprintf(stderr, "[lora-ab] failed to load persona '%s': %d\n", persona_name, err);
        return err;
    }

    hu_communication_style_t target;
    memset(&target, 0, sizeof(target));
    bool synthetic = true;
    {
        char pm_path[1024];
        if (hu_personal_model_resolve_default_path(pm_path, sizeof(pm_path))) {
            hu_personal_model_t loaded;
            if (hu_personal_model_load(&loaded, pm_path) == HU_OK &&
                loaded.style.sample_count > 0U) {
                target = loaded.style;
                synthetic = false;
            }
        }
    }
    if (synthetic) {
        /* Same defaults as `lora-baseline` so the two tools are
         * comparable when the user has no personal model. */
        target.formality = 0.3f;
        target.verbosity = 0.5f;
        target.emoji_frequency = 0.2f;
        target.humor_receptivity = 0.6f;
        target.lowercase_ratio = 0.85f;
        target.abbreviation_ratio = 0.2f;
        target.avg_message_length = 60;
        target.sample_count = 1;
        printf("[lora-ab] no personal_model.bin found — using synthetic fingerprint.\n");
    } else {
        printf("[lora-ab] fingerprint loaded from personal model "
               "(samples=%u, avg_len=%u).\n",
               (unsigned)target.sample_count, (unsigned)target.avg_message_length);
    }

    hu_json_value_t *json_before = NULL, *json_after = NULL;
    const char **strs_before = NULL, **strs_after = NULL;
    size_t *lens_before = NULL, *lens_after = NULL;
    size_t n_before = 0, n_after = 0;

    err = hu_ml_lora_ab_load_response_set(alloc, before_path, &json_before, &strs_before,
                                          &lens_before, &n_before);
    if (err != HU_OK) {
        fprintf(stderr, "[lora-ab] failed to load --before %s: %d\n", before_path, err);
        hu_persona_deinit(alloc, &persona);
        return err;
    }
    err = hu_ml_lora_ab_load_response_set(alloc, after_path, &json_after, &strs_after,
                                          &lens_after, &n_after);
    if (err != HU_OK) {
        fprintf(stderr, "[lora-ab] failed to load --after %s: %d\n", after_path, err);
        if (strs_before) alloc->free(alloc->ctx, strs_before, n_before * sizeof(const char *));
        if (lens_before) alloc->free(alloc->ctx, lens_before, n_before * sizeof(size_t));
        hu_json_free(alloc, json_before);
        hu_persona_deinit(alloc, &persona);
        return err;
    }

    hu_communication_style_set_summary_t sum_before, sum_after;
    float delta = 0.f;
    err = hu_communication_style_compare_response_sets(&target, strs_before, lens_before, n_before,
                                                       strs_after, lens_after, n_after,
                                                       &sum_before, &sum_after, &delta);
    /* Free loaders before reporting so a fail-fast exit doesn't
     * leak. The summaries are stack values and don't alias into
     * the JSON tree. */
    if (strs_before) alloc->free(alloc->ctx, strs_before, n_before * sizeof(const char *));
    if (lens_before) alloc->free(alloc->ctx, lens_before, n_before * sizeof(size_t));
    if (strs_after) alloc->free(alloc->ctx, strs_after, n_after * sizeof(const char *));
    if (lens_after) alloc->free(alloc->ctx, lens_after, n_after * sizeof(size_t));
    hu_json_free(alloc, json_before);
    hu_json_free(alloc, json_after);
    hu_persona_deinit(alloc, &persona);

    if (err != HU_OK) {
        fprintf(stderr, "[lora-ab] compare failed: %d\n", err);
        return err;
    }

    printf("[lora-ab] persona '%s' A/B fidelity:\n"
           "[lora-ab]   before: scored=%zu skipped=%zu mean=%.3f min=%.3f max=%.3f\n"
           "[lora-ab]   after:  scored=%zu skipped=%zu mean=%.3f min=%.3f max=%.3f\n"
           "[lora-ab]   delta:  %+.3f (after - before)\n",
           persona_name, sum_before.scored, sum_before.skipped, sum_before.mean,
           sum_before.min_score, sum_before.max_score, sum_after.scored, sum_after.skipped,
           sum_after.mean, sum_after.min_score, sum_after.max_score, delta);
    if (require_positive && delta <= 0.f) {
        fprintf(stderr, "[lora-ab] FAIL: delta=%+.3f <= 0 (require-positive set)\n", delta);
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (delta < floor_delta) {
        fprintf(stderr, "[lora-ab] FAIL: delta=%+.3f < floor=%+.3f\n", delta, floor_delta);
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (sum_after.scored == 0 || sum_before.scored == 0) {
        printf("[lora-ab] note: at least one set had 0 scored responses — "
               "delta may not be meaningful.\n");
    }
    printf("[lora-ab] doc: %s\n", hu_ml_lora_persona_caveat_doc_path());
    return HU_OK;
}

/* ── lora-runner ──────────────────────────────────────────────────────
 *
 * Walk a persona's example bank, send every `incoming` through a
 * provider's chat() call, capture each response, and write them as
 * a JSON array to disk for a downstream `lora-ab` invocation. The
 * critical design decision is what gets sent as the "system" message:
 * we use the persona's identity + traits so the model has the same
 * grounding it would in production. The user message is the
 * example's `incoming` — verbatim, no preprocessing.
 *
 * Errors during a single chat call don't abort the run — we
 * substitute an empty string for that index and keep going. The
 * comparator counts empty responses as `skipped`, so a few provider
 * hiccups don't poison the mean. */

/* Build a minimal "system" prompt from the persona's identity and
 * top traits, suitable for passing to a chat call. Returns 0 on
 * success (string written into `out`), non-zero when the persona
 * has no identity. */
static size_t hu_ml_lora_runner_build_system_prompt(const hu_persona_t *persona, char *out,
                                                    size_t cap) {
    if (!persona || cap == 0)
        return 0;
    out[0] = '\0';
    size_t n = 0;
    if (persona->identity && persona->identity[0]) {
        int w = snprintf(out + n, cap - n, "%s\n", persona->identity);
        if (w > 0 && (size_t)w < cap - n) n += (size_t)w;
    }
    if (persona->traits_count > 0 && persona->traits) {
        int w = snprintf(out + n, cap - n, "Traits: ");
        if (w > 0 && (size_t)w < cap - n) n += (size_t)w;
        for (size_t i = 0; i < persona->traits_count && n + 2 < cap; i++) {
            const char *t = persona->traits[i];
            if (!t || !t[0]) continue;
            w = snprintf(out + n, cap - n, "%s%s", i == 0 ? "" : ", ", t);
            if (w > 0 && (size_t)w < cap - n) n += (size_t)w;
        }
        if (n + 1 < cap) {
            out[n++] = '\n';
            out[n] = '\0';
        }
    }
    return n;
}

/* Write a `["resp1", "resp2", ...]` JSON array to `path`. JSON
 * escaping uses `hu_json_string_new` + `hu_json_stringify` so the
 * output is parser-round-trip compatible with `lora-ab`'s loader.
 * Each NULL or empty response becomes a JSON empty string `""` so
 * the array length matches the example count — easier to align
 * with the bank when debugging. */
static hu_error_t hu_ml_lora_runner_write_json_array(hu_allocator_t *alloc, const char *path,
                                                     const char *const *responses,
                                                     const size_t *lens, size_t count) {
    hu_json_value_t *arr = hu_json_array_new(alloc);
    if (!arr)
        return HU_ERR_OUT_OF_MEMORY;
    hu_error_t err = HU_OK;
    for (size_t i = 0; i < count && err == HU_OK; i++) {
        const char *s = responses[i] ? responses[i] : "";
        size_t l = responses[i] ? lens[i] : 0;
        hu_json_value_t *item = hu_json_string_new(alloc, s, l);
        if (!item) {
            err = HU_ERR_OUT_OF_MEMORY;
            break;
        }
        err = hu_json_array_push(alloc, arr, item);
        /* On failure `item` is leaked into nowhere — push owns
         * the value on success but doesn't free on failure.
         * `hu_json_free(arr)` will clean up everything we
         * successfully pushed. */
        if (err != HU_OK)
            hu_json_free(alloc, item);
    }
    if (err != HU_OK) {
        hu_json_free(alloc, arr);
        return err;
    }
    char *out = NULL;
    size_t out_len = 0;
    err = hu_json_stringify(alloc, arr, &out, &out_len);
    hu_json_free(alloc, arr);
    if (err != HU_OK)
        return err;
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        alloc->free(alloc->ctx, out, out_len + 1);
        return HU_ERR_IO;
    }
    size_t wrote = fwrite(out, 1, out_len, fp);
    /* Newline at EOF — POSIX text-file convention; downstream
     * parsers (jq, hu_json_parse) tolerate either way. */
    fputc('\n', fp);
    fclose(fp);
    alloc->free(alloc->ctx, out, out_len + 1);
    if (wrote != out_len)
        return HU_ERR_IO;
    return HU_OK;
}

hu_error_t hu_ml_cli_lora_runner(hu_allocator_t *alloc, int argc, const char **argv) {
    const char *persona_name = NULL;
    const char *output_path = NULL;
    const char *provider_name = NULL;
    const char *model = NULL;
    const char *adapter_path = NULL;
    const char *adapter_id = NULL;
    int max_examples = 0; /* 0 = all */

    for (int i = 1; i < argc; i++) {
        const char *v = get_opt(argv, argc, i, "--persona");
        if (v) { persona_name = v; i++; continue; }
        v = get_opt(argv, argc, i, "--output");
        if (v) { output_path = v; i++; continue; }
        v = get_opt(argv, argc, i, "--provider");
        if (v) { provider_name = v; i++; continue; }
        v = get_opt(argv, argc, i, "--model");
        if (v) { model = v; i++; continue; }
        v = get_opt(argv, argc, i, "--max-examples");
        if (v) { max_examples = atoi(v); i++; continue; }
        v = get_opt(argv, argc, i, "--adapter");
        if (v) { adapter_path = v; i++; continue; }
        v = get_opt(argv, argc, i, "--adapter-id");
        if (v) { adapter_id = v; i++; continue; }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: human ml lora-runner --persona <name> --output <path> "
                   "[--provider <name>] [--model <id>] [--max-examples N] "
                   "[--adapter <path>] [--adapter-id <id>]\n\n"
                   "Run a persona's example bank prompts through a provider's\n"
                   "chat() call and write the responses as a JSON array. The\n"
                   "output is directly consumable by `human ml lora-ab` as\n"
                   "either --before or --after. Run twice (with and without\n"
                   "an adapter loaded) to produce both halves of an A/B run.\n\n"
                   "  --persona <name>      Persona name in ~/.human/personas/\n"
                   "  --output <path>       Where to write the JSON array\n"
                   "  --provider <name>     Provider to invoke (default: from config)\n"
                   "  --model <id>          Model id (default: provider default)\n"
                   "  --max-examples N      Cap responses (default: all banks)\n"
                   "  --adapter <path>      Optional LoRA adapter to load before chat;\n"
                   "                        provider must support load_adapter (huml does)\n"
                   "  --adapter-id <id>     Identifier for the loaded adapter\n"
                   "                        (default: basename of --adapter)\n");
            printf("\nDoc: %s\n", hu_ml_lora_persona_caveat_doc_path());
            return HU_OK;
        }
    }
    if (!persona_name || !output_path) {
        fprintf(stderr, "lora-runner requires --persona <name> --output <path>\n");
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_persona_t persona = {0};
    hu_error_t err = hu_persona_load(alloc, persona_name, strlen(persona_name), &persona);
    if (err != HU_OK) {
        fprintf(stderr, "[lora-runner] failed to load persona '%s': %d\n", persona_name, err);
        return err;
    }

    /* Count total examples across all banks. */
    size_t total_examples = 0;
    for (size_t b = 0; b < persona.example_banks_count; b++) {
        const hu_persona_example_bank_t *bank = &persona.example_banks[b];
        for (size_t i = 0; i < bank->examples_count; i++) {
            if (bank->examples[i].incoming && bank->examples[i].incoming[0])
                total_examples++;
        }
    }
    if (max_examples > 0 && (size_t)max_examples < total_examples)
        total_examples = (size_t)max_examples;
    if (total_examples == 0) {
        fprintf(stderr, "[lora-runner] persona '%s' has no example incoming messages\n",
                persona_name);
        hu_persona_deinit(alloc, &persona);
        return HU_ERR_INVALID_ARGUMENT;
    }

    char **responses = (char **)alloc->alloc(alloc->ctx, total_examples * sizeof(char *));
    size_t *lens = (size_t *)alloc->alloc(alloc->ctx, total_examples * sizeof(size_t));
    if (!responses || !lens) {
        if (responses) alloc->free(alloc->ctx, responses, total_examples * sizeof(char *));
        if (lens) alloc->free(alloc->ctx, lens, total_examples * sizeof(size_t));
        hu_persona_deinit(alloc, &persona);
        return HU_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < total_examples; i++) {
        responses[i] = NULL;
        lens[i] = 0;
    }

#ifndef HU_IS_TEST
    /* Production path: create a provider and walk the example bank.
     * Errors per-example don't abort the run; the comparator counts
     * empty entries as `skipped`. */
    hu_provider_t provider = {0};
    if (provider_name && provider_name[0]) {
        err = hu_provider_create(alloc, provider_name, strlen(provider_name), NULL, 0, NULL, 0,
                                 &provider);
    } else {
        hu_config_t cfg = {0};
        if (hu_config_load(alloc, &cfg) == HU_OK) {
            err = hu_provider_create_default(alloc, &cfg, &provider);
            hu_config_deinit(&cfg);
        } else {
            err = HU_ERR_NOT_SUPPORTED;
        }
    }
    if (err != HU_OK || !provider.vtable || !provider.vtable->chat_with_system) {
        fprintf(stderr, "[lora-runner] no usable provider (err=%d, vtable=%s)\n", err,
                (provider.vtable && provider.vtable->chat_with_system) ? "ok" : "missing");
        if (provider.vtable && provider.vtable->deinit)
            provider.vtable->deinit(provider.ctx, alloc);
        alloc->free(alloc->ctx, responses, total_examples * sizeof(char *));
        alloc->free(alloc->ctx, lens, total_examples * sizeof(size_t));
        hu_persona_deinit(alloc, &persona);
        return err == HU_OK ? HU_ERR_NOT_SUPPORTED : err;
    }

    /* Optional adapter pre-load. The runner's "after.json" half of
     * the canonical A/B workflow happens in the same process as
     * provider creation — the adapter loaded here applies to every
     * subsequent chat call in this run. We deliberately fail-fast
     * (early return) when the adapter can't load: producing
     * "after.json" against the BASE model would silently corrupt
     * the comparator's delta, and a delta-zero false positive is
     * worse than no run at all. */
    if (adapter_path && adapter_path[0]) {
        const char *id = adapter_id;
        char id_buf[64];
        if (!id) {
            const char *slash = strrchr(adapter_path, '/');
            const char *base = slash ? slash + 1 : adapter_path;
            size_t copy = strlen(base);
            if (copy >= sizeof(id_buf))
                copy = sizeof(id_buf) - 1;
            memcpy(id_buf, base, copy);
            id_buf[copy] = '\0';
            id = id_buf;
        }
        hu_error_t lerr = hu_provider_load_adapter(&provider, alloc, adapter_path,
                                                   strlen(adapter_path), id, strlen(id));
        if (lerr != HU_OK) {
            fprintf(stderr,
                    "[lora-runner] adapter load failed (path=%s, id=%s, err=%d). "
                    "Aborting to avoid producing a base-model 'after.json'.\n",
                    adapter_path, id, lerr);
            if (provider.vtable && provider.vtable->deinit)
                provider.vtable->deinit(provider.ctx, alloc);
            alloc->free(alloc->ctx, responses, total_examples * sizeof(char *));
            alloc->free(alloc->ctx, lens, total_examples * sizeof(size_t));
            hu_persona_deinit(alloc, &persona);
            return lerr;
        }
        printf("[lora-runner] adapter loaded: %s (id=%s)\n", adapter_path, id);
    }

    char system_buf[1024];
    size_t system_len = hu_ml_lora_runner_build_system_prompt(&persona, system_buf,
                                                              sizeof(system_buf));
    size_t model_len = model ? strlen(model) : 0;

    size_t produced = 0;
    size_t errors = 0;
    for (size_t b = 0; b < persona.example_banks_count && produced < total_examples; b++) {
        const hu_persona_example_bank_t *bank = &persona.example_banks[b];
        for (size_t i = 0; i < bank->examples_count && produced < total_examples; i++) {
            const hu_persona_example_t *ex = &bank->examples[i];
            if (!ex->incoming || !ex->incoming[0])
                continue;
            char *content = NULL;
            size_t content_len = 0;
            hu_error_t cerr = provider.vtable->chat_with_system(
                provider.ctx, alloc, system_buf, system_len, ex->incoming, strlen(ex->incoming),
                model, model_len, 0.7, &content, &content_len);
            if (cerr == HU_OK && content && content_len > 0) {
                responses[produced] = (char *)alloc->alloc(alloc->ctx, content_len + 1);
                if (responses[produced]) {
                    memcpy(responses[produced], content, content_len);
                    responses[produced][content_len] = '\0';
                    lens[produced] = content_len;
                }
            } else {
                errors++;
            }
            if (content)
                alloc->free(alloc->ctx, content, content_len + 1);
            produced++;
        }
    }

    if (errors > 0)
        fprintf(stderr, "[lora-runner] %zu of %zu examples returned an error or empty content\n",
                errors, total_examples);

    if (provider.vtable->deinit)
        provider.vtable->deinit(provider.ctx, alloc);
#else
    /* Test path: deterministic echo of the persona's existing
     * `response` field. Lets unit tests exercise the full
     * load → write → JSON round-trip without spinning up a
     * provider, which is unavailable in tests anyway. */
    size_t produced = 0;
    for (size_t b = 0; b < persona.example_banks_count && produced < total_examples; b++) {
        const hu_persona_example_bank_t *bank = &persona.example_banks[b];
        for (size_t i = 0; i < bank->examples_count && produced < total_examples; i++) {
            const hu_persona_example_t *ex = &bank->examples[i];
            if (!ex->incoming || !ex->incoming[0])
                continue;
            const char *src = (ex->response && ex->response[0]) ? ex->response : "";
            size_t srclen = strlen(src);
            responses[produced] = (char *)alloc->alloc(alloc->ctx, srclen + 1);
            if (responses[produced]) {
                memcpy(responses[produced], src, srclen);
                responses[produced][srclen] = '\0';
                lens[produced] = srclen;
            }
            produced++;
        }
    }
#endif

    err = hu_ml_lora_runner_write_json_array(alloc, output_path,
                                             (const char *const *)responses, lens, total_examples);
    if (err == HU_OK) {
        printf("[lora-runner] persona '%s' → %s (%zu responses)\n", persona_name, output_path,
               total_examples);
    } else {
        fprintf(stderr, "[lora-runner] write failed: %d\n", err);
    }

    for (size_t i = 0; i < total_examples; i++) {
        if (responses[i])
            alloc->free(alloc->ctx, responses[i], lens[i] + 1);
    }
    alloc->free(alloc->ctx, responses, total_examples * sizeof(char *));
    alloc->free(alloc->ctx, lens, total_examples * sizeof(size_t));
    hu_persona_deinit(alloc, &persona);
    return err;
}

/* ── fidelity-status ──────────────────────────────────────────────────
 *
 * Emits a JSON status object that aggregates everything any
 * dashboard or CLI status surface would want to display about a
 * persona's LoRA-fidelity health: the baseline mean from the
 * persona's example bank, and (when a `--before`/`--after` pair is
 * provided) the A/B delta. The intent is "one command, one JSON
 * object" so future UI/observability work doesn't have to scrape
 * `lora-baseline` and `lora-ab` separately. */

/* Track D D2.2 — the resolve-target + baseline-score primitives
 * used to live as static helpers in this file; they moved to
 * `src/ml/fidelity.c` so the gateway's `metrics.fidelity` method
 * computes identical numbers. The CLI just orchestrates I/O. */

hu_error_t hu_ml_cli_fidelity_status(hu_allocator_t *alloc, int argc, const char **argv) {
    const char *persona_name = NULL;
    const char *before_path = NULL;
    const char *after_path = NULL;
    const char *output_path = NULL;
    for (int i = 1; i < argc; i++) {
        const char *v = get_opt(argv, argc, i, "--persona");
        if (v) { persona_name = v; i++; continue; }
        v = get_opt(argv, argc, i, "--before");
        if (v) { before_path = v; i++; continue; }
        v = get_opt(argv, argc, i, "--after");
        if (v) { after_path = v; i++; continue; }
        v = get_opt(argv, argc, i, "--output");
        if (v) { output_path = v; i++; continue; }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: human ml fidelity-status --persona <name> "
                   "[--before <path>] [--after <path>] [--output <path>]\n\n"
                   "Emit a single JSON object aggregating LoRA-fidelity\n"
                   "health metrics for downstream dashboards & status\n"
                   "surfaces. When --before and --after are both provided,\n"
                   "the A/B delta is included.\n");
            return HU_OK;
        }
    }
    if (!persona_name) {
        fprintf(stderr, "fidelity-status requires --persona <name>\n");
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_persona_t persona = {0};
    hu_error_t err = hu_persona_load(alloc, persona_name, strlen(persona_name), &persona);
    if (err != HU_OK) {
        fprintf(stderr, "[fidelity-status] failed to load persona '%s': %d\n", persona_name, err);
        return err;
    }

    hu_communication_style_t target;
    bool synthetic;
    (void)hu_ml_fidelity_resolve_target(alloc, &target, &synthetic);

    /* Baseline: walk the persona's example bank scoring every
     * non-empty response — same numbers `metrics.fidelity` returns
     * because both surfaces call into the shared helper. */
    hu_communication_style_set_summary_t baseline_summary = {0};
    (void)hu_ml_fidelity_score_baseline(&persona, &target, &baseline_summary);
    size_t b_scored = baseline_summary.scored;
    float b_mean = baseline_summary.mean;
    float b_min = baseline_summary.min_score;
    float b_max = baseline_summary.max_score;

    /* A/B (optional): only included when both files are provided. */
    bool ab_available = false;
    hu_communication_style_set_summary_t sum_a = {0}, sum_b = {0};
    float delta = 0.f;
    if (before_path && after_path) {
        hu_json_value_t *json_b = NULL, *json_a = NULL;
        const char **strs_b = NULL, **strs_a = NULL;
        size_t *lens_b = NULL, *lens_a = NULL;
        size_t n_b = 0, n_a = 0;
        if (hu_ml_lora_ab_load_response_set(alloc, before_path, &json_b, &strs_b, &lens_b,
                                            &n_b) == HU_OK &&
            hu_ml_lora_ab_load_response_set(alloc, after_path, &json_a, &strs_a, &lens_a,
                                            &n_a) == HU_OK) {
            if (hu_communication_style_compare_response_sets(&target, strs_b, lens_b, n_b,
                                                             strs_a, lens_a, n_a, &sum_a, &sum_b,
                                                             &delta) == HU_OK) {
                ab_available = true;
            }
        }
        if (strs_b) alloc->free(alloc->ctx, strs_b, n_b * sizeof(const char *));
        if (lens_b) alloc->free(alloc->ctx, lens_b, n_b * sizeof(size_t));
        if (strs_a) alloc->free(alloc->ctx, strs_a, n_a * sizeof(const char *));
        if (lens_a) alloc->free(alloc->ctx, lens_a, n_a * sizeof(size_t));
        hu_json_free(alloc, json_b);
        hu_json_free(alloc, json_a);
    }

    /* Build the JSON document via the canonical builder. */
    hu_json_value_t *root = hu_json_object_new(alloc);
    if (!root) {
        hu_persona_deinit(alloc, &persona);
        return HU_ERR_OUT_OF_MEMORY;
    }
    hu_json_object_set(alloc, root, "persona",
                       hu_json_string_new(alloc, persona_name, strlen(persona_name)));
    hu_json_object_set(alloc, root, "fingerprint_source",
                       hu_json_string_new(alloc, synthetic ? "synthetic" : "personal_model",
                                          synthetic ? 9 : 14));

    hu_json_value_t *baseline_obj = hu_json_object_new(alloc);
    hu_json_object_set(alloc, baseline_obj, "scored",
                       hu_json_number_new(alloc, (double)b_scored));
    hu_json_object_set(alloc, baseline_obj, "mean", hu_json_number_new(alloc, b_mean));
    hu_json_object_set(alloc, baseline_obj, "min", hu_json_number_new(alloc, b_min));
    hu_json_object_set(alloc, baseline_obj, "max", hu_json_number_new(alloc, b_max));
    hu_json_object_set(alloc, root, "baseline", baseline_obj);

    hu_json_value_t *ab = hu_json_object_new(alloc);
    hu_json_object_set(alloc, ab, "available", hu_json_bool_new(alloc, ab_available));
    if (ab_available) {
        hu_json_object_set(alloc, ab, "before_mean", hu_json_number_new(alloc, sum_a.mean));
        hu_json_object_set(alloc, ab, "after_mean", hu_json_number_new(alloc, sum_b.mean));
        hu_json_object_set(alloc, ab, "delta", hu_json_number_new(alloc, delta));
        hu_json_object_set(alloc, ab, "scored_before",
                           hu_json_number_new(alloc, (double)sum_a.scored));
        hu_json_object_set(alloc, ab, "scored_after",
                           hu_json_number_new(alloc, (double)sum_b.scored));
    }
    hu_json_object_set(alloc, root, "ab", ab);

    char *out = NULL;
    size_t out_len = 0;
    err = hu_json_stringify(alloc, root, &out, &out_len);
    hu_json_free(alloc, root);
    hu_persona_deinit(alloc, &persona);
    if (err != HU_OK)
        return err;

    if (output_path && output_path[0]) {
        FILE *fp = fopen(output_path, "wb");
        if (!fp) {
            alloc->free(alloc->ctx, out, out_len + 1);
            fprintf(stderr, "[fidelity-status] failed to open %s for writing\n", output_path);
            return HU_ERR_IO;
        }
        fwrite(out, 1, out_len, fp);
        fputc('\n', fp);
        fclose(fp);
    } else {
        /* stdout — single line of JSON terminated by newline. */
        fwrite(out, 1, out_len, stdout);
        fputc('\n', stdout);
    }
    alloc->free(alloc->ctx, out, out_len + 1);
    return HU_OK;
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

    /* Phase 0 fix — derive token_bytes from the in-scope tokenizer and align
     * cfg.gpt.vocab_size to the tokenizer's actual vocab so the model matches
     * the data and BPB is well-defined. See spec §1.5.2 issue #2. */
    int32_t *token_bytes = NULL;
    size_t token_bytes_count = 0;
    err = hu_ml_prepare_token_bytes(alloc, tok, &token_bytes, &token_bytes_count);
    if (err != HU_OK) {
        hu_bpe_tokenizer_deinit(tok);
        return err;
    }
    cfg.gpt.vocab_size = token_bytes_count;

    hu_model_t model = {0};
    err = hu_gpt_create(alloc, &cfg.gpt, &model);
    if (err != HU_OK) {
        alloc->free(alloc->ctx, token_bytes, token_bytes_count * sizeof(int32_t));
        hu_bpe_tokenizer_deinit(tok);
        return err;
    }

    hu_ml_optimizer_t optimizer = {0};
    err = hu_muon_adamw_create(alloc, &cfg.optimizer, &optimizer);
    if (err != HU_OK) {
        model.vtable->deinit(model.ctx, alloc);
        alloc->free(alloc->ctx, token_bytes, token_bytes_count * sizeof(int32_t));
        hu_bpe_tokenizer_deinit(tok);
        return err;
    }

    hu_ml_dataloader_t *train_loader = NULL;
    err = hu_ml_dataloader_create(alloc, tmp_dir, cfg.training.device_batch_size,
                                  cfg.gpt.sequence_len, "train", &train_loader);
    if (err != HU_OK) {
        optimizer.vtable->deinit(optimizer.ctx, alloc);
        model.vtable->deinit(model.ctx, alloc);
        alloc->free(alloc->ctx, token_bytes, token_bytes_count * sizeof(int32_t));
        hu_bpe_tokenizer_deinit(tok);
        return err;
    }

    printf("[train-feed-predictor] Training topic predictor (steps=%d, vocab=%zu)\n",
           max_steps, cfg.gpt.vocab_size);

    hu_ml_train_result_t result = {0};
    err = hu_ml_train(alloc, &model, &optimizer, train_loader, NULL, &cfg.training,
                      token_bytes, token_bytes_count, &result);

    printf("[train-feed-predictor] %s: %zu steps, %.2f bpb, %.1fs\n",
           err == HU_OK ? "Done" : "Failed", result.num_steps, result.val_bpb,
           result.training_seconds);

    hu_ml_dataloader_deinit(train_loader);
    optimizer.vtable->deinit(optimizer.ctx, alloc);
    model.vtable->deinit(model.ctx, alloc);
    alloc->free(alloc->ctx, token_bytes, token_bytes_count * sizeof(int32_t));
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
