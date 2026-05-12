/* src/ml/cli_dpo.c — Phase 2 Task 8
 *
 * Splits `human ml dpo-train` into two handlers:
 *   - hu_ml_cli_dpo_judge: legacy provider-scored "judge step" path.
 *     Body is a verbatim mechanical extraction of the pre-Phase-2
 *     hu_ml_cli_dpo_train body (src/ml/cli.c:484-595 in the
 *     predecessor commit). Behavior unchanged; only the function
 *     name moved.
 *   - hu_ml_cli_dpo_real:  the Phase 2 real-DPO path, dispatching
 *     to the hu_rl_trainer_t vtable from Tasks 1/4/6.
 *
 * Plan deviation notes:
 *   1. Headers: the canonical plan snippet (lines 1962–2039)
 *      `#include`s `"human/error.h"`. That path does not exist in
 *      this repo — the real path is `"human/core/error.h"`
 *      (cli_dpo.h's includes already cover allocator + error).
 *   2. The plan's `hu_ml_cli_dpo_judge` body is the placeholder
 *      `"For brevity in this plan; the actual extraction is
 *      mechanical"`. The real extraction lives below — see the
 *      function comment.
 *   3. Local `get_opt` here mirrors the index-by-value variant in
 *      src/ml/cli.c (i + 1 < argc && strcmp == 0 → argv[i+1])
 *      because the legacy body relies on that exact signature.
 *      The plan's `get_opt` pointer-to-i variant is kept for the
 *      `hu_ml_cli_dpo_real` branch.
 */

#include "human/ml/cli_dpo.h"
#include "human/core/log.h"
#include "human/ml/dpo.h"
#include "human/ml/rl_trainer.h"
#include "human/provider.h"
#include "human/providers/factory.h"

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Mirrors the index-by-value get_opt in src/ml/cli.c. The legacy body
 * extracted into hu_ml_cli_dpo_judge depends on this exact signature. */
static const char *cli_dpo_get_opt(const char **argv, int argc, int i, const char *opt) {
    if (i + 1 < argc && strcmp(argv[i], opt) == 0)
        return argv[i + 1];
    return NULL;
}

static int cli_dpo_parse_int_arg(const char *val, int *out) {
    if (!val || !out)
        return -1;
    char *end = NULL;
    long n = strtol(val, &end, 10);
    if (end == val || *end != '\0' || n < 0)
        return -1;
    *out = (int)n;
    return 0;
}

/* Pointer-to-index helper from the plan snippet for hu_ml_cli_dpo_real. */
static const char *cli_dpo_get_opt_p(const char **argv, int argc, int *i, const char *flag) {
    if (strcmp(argv[*i], flag) == 0 && *i + 1 < argc) {
        return argv[++(*i)];
    }
    return NULL;
}

/* Verbatim mechanical extraction of hu_ml_cli_dpo_train's pre-Phase-2
 * body (src/ml/cli.c:484-595 in the predecessor commit). Only the
 * function name has changed; every line of behavior is preserved. */
hu_error_t hu_ml_cli_dpo_judge(hu_allocator_t *alloc, int argc, const char **argv) {
    const char *db_path = NULL;
    const char *provider_name = NULL;
    const char *model = NULL;
    int batch_size = 20;
    for (int i = 1; i < argc; i++) {
        const char *v = cli_dpo_get_opt(argv, argc, i, "--db");
        if (v) {
            db_path = v;
            i++;
            continue;
        }
        v = cli_dpo_get_opt(argv, argc, i, "--provider");
        if (v) {
            provider_name = v;
            i++;
            continue;
        }
        v = cli_dpo_get_opt(argv, argc, i, "--model");
        if (v) {
            model = v;
            i++;
            continue;
        }
        v = cli_dpo_get_opt(argv, argc, i, "--batch-size");
        if (v) {
            if (cli_dpo_parse_int_arg(v, &batch_size) != 0) {
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
    hu_dpo_judge_result_t result = {0};
    printf("[dpo] Running DPO judge step (provider=%s, batch=%d)...\n", provider_name,
           batch_size);

    err = hu_dpo_judge_step(&collector, alloc, &provider, model, model_len, 0.1, (size_t)batch_size,
                            &result);

    if (err == HU_OK) {
        printf("[dpo] Judge step complete:\n");
        printf("  Pairs evaluated: %zu\n", result.pairs_evaluated);
        printf("  Pairs aligned:   %zu\n", result.pairs_aligned);
        printf("  Alignment score: %.2f%%\n", result.alignment_score * 100.0);
        printf("  Loss:            %.4f\n", result.loss);
    } else {
        fprintf(stderr, "[dpo] Judge step failed: %d\n", err);
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

hu_error_t hu_ml_cli_dpo_real(hu_allocator_t *alloc, int argc, const char **argv) {
    if (!alloc) return HU_ERR_INVALID_ARGUMENT;
    const char *backend_str = "auto";
    const char *pairs_path = NULL;
    int max_iters = 100;
    double beta = 0.1;
    const char *model_id = "mlx-community/gemma-3-4b-it-bf16";
    const char *adapter_out = "./adapters";

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: human ml dpo-train [options]\n"
                   "  --backend {auto,huml,mlx}  default: auto\n"
                   "  --pairs <path>             JSONL preference pairs (default: SQLite)\n"
                   "  --iters <N>                training iterations (default: 100)\n"
                   "  --beta <float>             DPO temperature (default: 0.1)\n"
                   "  --model <hf-id>            MLX backend model id\n"
                   "  --adapter-out <dir>        MLX backend output directory\n"
                   "  --legacy-judge             dispatch to old dpo-judge (deprecated, removed Phase 3)\n");
            return HU_OK;
        }
        const char *v;
        if ((v = cli_dpo_get_opt_p(argv, argc, &i, "--backend")))     backend_str = v;
        else if ((v = cli_dpo_get_opt_p(argv, argc, &i, "--pairs")))  pairs_path = v;
        else if ((v = cli_dpo_get_opt_p(argv, argc, &i, "--iters")))  max_iters = atoi(v);
        else if ((v = cli_dpo_get_opt_p(argv, argc, &i, "--beta")))   beta = atof(v);
        else if ((v = cli_dpo_get_opt_p(argv, argc, &i, "--model")))  model_id = v;
        else if ((v = cli_dpo_get_opt_p(argv, argc, &i, "--adapter-out"))) adapter_out = v;
        else if (strcmp(argv[i], "--legacy-judge") == 0) {
            return hu_ml_cli_dpo_judge(alloc, argc - i - 1, argv + i + 1);
        }
    }

    /* TODO(Task 9): load pairs from `--pairs` JSONL or from the SQLite
     * dpo_pairs table and feed them to trainer.vtable->step(). For now
     * we just accept and ignore the path so `--pairs <p>` parses. */
    (void)pairs_path;

    hu_dpo_backend_t backend = HU_DPO_BACKEND_AUTO;
    if (strcmp(backend_str, "huml") == 0) backend = HU_DPO_BACKEND_HUML;
    else if (strcmp(backend_str, "mlx") == 0) backend = HU_DPO_BACKEND_MLX;

    hu_rl_trainer_config_t cfg = {
        .backend = backend, .beta = beta, .max_iters = (size_t)max_iters,
        .model_id = model_id, .adapter_out_dir = adapter_out,
    };
    hu_rl_trainer_t trainer = {0};
    hu_error_t err = hu_rl_trainer_create_dpo(alloc, &cfg, &trainer);
    if (err != HU_OK) {
        fprintf(stderr, "[dpo-train] failed to create trainer: error %d\n", (int)err);
        return err;
    }
    /* TODO(Task 9): once pair loading lands, drive trainer.vtable->step()
     * here. Task 8 just prints the resolved backend and exits. */
    fprintf(stderr, "[dpo-train] backend=%s, iters=%d, beta=%.2f\n",
            trainer.vtable->name(trainer.ctx), max_iters, beta);
    trainer.vtable->deinit(trainer.ctx, alloc);
    return HU_OK;
}
