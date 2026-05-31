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
    printf("[dpo] Running DPO judge step (provider=%s, batch=%d)...\n", provider_name, batch_size);

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
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;
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
                   "  --legacy-judge             dispatch to old dpo-judge (deprecated, removed "
                   "Phase 3)\n");
            return HU_OK;
        }
        const char *v;
        if ((v = cli_dpo_get_opt_p(argv, argc, &i, "--backend")))
            backend_str = v;
        else if ((v = cli_dpo_get_opt_p(argv, argc, &i, "--pairs")))
            pairs_path = v;
        else if ((v = cli_dpo_get_opt_p(argv, argc, &i, "--iters")))
            max_iters = atoi(v);
        else if ((v = cli_dpo_get_opt_p(argv, argc, &i, "--beta")))
            beta = atof(v);
        else if ((v = cli_dpo_get_opt_p(argv, argc, &i, "--model")))
            model_id = v;
        else if ((v = cli_dpo_get_opt_p(argv, argc, &i, "--adapter-out")))
            adapter_out = v;
        else if (strcmp(argv[i], "--legacy-judge") == 0) {
            return hu_ml_cli_dpo_judge(alloc, argc - i - 1, argv + i + 1);
        }
    }

    hu_dpo_backend_t backend = HU_DPO_BACKEND_AUTO;
    if (strcmp(backend_str, "huml") == 0)
        backend = HU_DPO_BACKEND_HUML;
    else if (strcmp(backend_str, "mlx") == 0)
        backend = HU_DPO_BACKEND_MLX;

    hu_rl_trainer_config_t cfg = {
        .backend = backend,
        .beta = beta,
        .max_iters = (size_t)max_iters,
        .model_id = model_id,
        .adapter_out_dir = adapter_out,
    };
    hu_rl_trainer_t trainer = {0};
    hu_error_t err = hu_rl_trainer_create_dpo(alloc, &cfg, &trainer);
    if (err != HU_OK) {
        fprintf(stderr, "[dpo-train] failed to create trainer: error %d\n", (int)err);
        return err;
    }
    fprintf(stderr, "[dpo-train] backend=%s, iters=%d, beta=%.2f\n",
            trainer.vtable->name(trainer.ctx), max_iters, beta);

    /* Task 9: JSONL pair loader + iteration loop. Replaces the Task 8
     * fprintf-and-exit placeholder.
     *
     * Plan deviation #4 (vs lines 2113-2123): the canonical snippet's
     * `if (!f) return HU_ERR_IO;` early-return leaks the trainer (created
     * above). The plan's `else` branch correctly deinits before
     * returning HU_ERR_NOT_SUPPORTED — we apply the same discipline to
     * the fopen-failure path to keep ASan clean.
     *
     * `pairs[256]` ≈ 256 * sizeof(hu_preference_pair_t) (~10 KB each) =
     * ~2.65 MB on the function stack. macOS default main-thread stack is
     * 8 MB so this fits, but with ASan red zones it gets tight; if a
     * future test runs on a smaller pthread stack this should switch to
     * `alloc->alloc(...)`. Tracked but not changed in this task to
     * minimize diff vs the plan snippet (plan lines 2116 + 2170-2171
     * explicitly call out the stack/caller-allocated discipline). */
    hu_preference_pair_t pairs[256];
    size_t n_pairs = 0;

    if (pairs_path) {
        FILE *f = fopen(pairs_path, "r");
        if (!f) {
            fprintf(stderr, "[dpo-train] failed to open --pairs %s\n", pairs_path);
            trainer.vtable->deinit(trainer.ctx, alloc);
            return HU_ERR_IO;
        }
        char line[2048];
        while (fgets(line, sizeof(line), f) && n_pairs < 256) {
            char *p = strstr(line, "\"prompt\": \"");
            char *c = strstr(line, "\"chosen\": \"");
            char *r = strstr(line, "\"rejected\": \"");
            if (!p || !c || !r)
                continue;
            char prompt[512] = {0}, chosen[512] = {0}, rejected[512] = {0};
            sscanf(p + 11, "%511[^\"]", prompt);
            sscanf(c + 11, "%511[^\"]", chosen);
            sscanf(r + 13, "%511[^\"]", rejected);
            /* hu_preference_pair_t fields are fixed-size char arrays per
             * include/human/ml/dpo.h:15-26, NOT pointers. strncpy + _len
             * updates match the Phase 2 discipline (same pattern as
             * test_dpo_real_e2e.c:55-61). */
            memset(&pairs[n_pairs], 0, sizeof(pairs[n_pairs]));
            strncpy(pairs[n_pairs].prompt, prompt, sizeof(pairs[n_pairs].prompt) - 1);
            pairs[n_pairs].prompt_len = strlen(pairs[n_pairs].prompt);
            strncpy(pairs[n_pairs].chosen, chosen, sizeof(pairs[n_pairs].chosen) - 1);
            pairs[n_pairs].chosen_len = strlen(pairs[n_pairs].chosen);
            strncpy(pairs[n_pairs].rejected, rejected, sizeof(pairs[n_pairs].rejected) - 1);
            pairs[n_pairs].rejected_len = strlen(pairs[n_pairs].rejected);
            strncpy(pairs[n_pairs].source, "jsonl", sizeof(pairs[n_pairs].source) - 1);
            pairs[n_pairs].source_len = strlen(pairs[n_pairs].source);
            n_pairs++;
        }
        fclose(f);
    } else {
        /* SQLite path: open ~/.human/memory.db, run hu_dpo_export
         * (which embeds the iterator filter that skips short-side
         * legacy rows — see src/ml/dpo.c::hu_dpo_export and
         * docs/plans/2026-05-19-dpo-corpus-inverted.md). Copy up to
         * 256 rows into the stack array used by the training loop. */
        const char *home = getenv("HOME");
        if (!home || !*home) {
            fprintf(stderr, "[dpo-train] HOME not set; cannot find memory.db\n");
            trainer.vtable->deinit(trainer.ctx, alloc);
            return HU_ERR_IO;
        }
        char db_path[1024];
        snprintf(db_path, sizeof(db_path), "%s/.human/memory.db", home);
        sqlite3 *db = NULL;
        if (sqlite3_open(db_path, &db) != SQLITE_OK) {
            fprintf(stderr, "[dpo-train] failed to open %s: %s\n", db_path,
                    db ? sqlite3_errmsg(db) : "(null)");
            if (db)
                sqlite3_close(db);
            trainer.vtable->deinit(trainer.ctx, alloc);
            return HU_ERR_IO;
        }
        hu_dpo_collector_t col;
        hu_error_t cerr = hu_dpo_collector_create(alloc, db, 0, &col);
        if (cerr != HU_OK) {
            fprintf(stderr, "[dpo-train] collector_create failed: error %d\n", (int)cerr);
            sqlite3_close(db);
            trainer.vtable->deinit(trainer.ctx, alloc);
            return cerr;
        }
        hu_dpo_export_t exp = {0};
        /* Paired export: a user's on-disk corpus accumulates SINGLE-SIDED rows
         * from reaction collection (a positive tapback fills only `chosen`, a
         * negative only `rejected`). Plain hu_dpo_export drops those, so manual
         * `dpo-train` on a reaction-built DB would see zero pairs. The paired
         * variant zips same-prompt chosen-only + rejected-only rows into
         * trainable pairs (genuine two-sided rows still pass through). */
        cerr = hu_dpo_export_paired(&col, alloc, &exp);
        if (cerr != HU_OK) {
            fprintf(stderr, "[dpo-train] export from SQLite failed: error %d\n", (int)cerr);
            hu_dpo_export_free(alloc, &exp);
            hu_dpo_collector_deinit(&col);
            sqlite3_close(db);
            trainer.vtable->deinit(trainer.ctx, alloc);
            return cerr;
        }
        fprintf(stderr,
                "[dpo-train] loaded %zu pairs from %s "
                "(after iterator filter)\n",
                exp.count, db_path);
        size_t to_copy = exp.count < 256 ? exp.count : 256;
        for (size_t i = 0; i < to_copy; i++) {
            pairs[n_pairs++] = exp.pairs[i];
        }
        if (exp.count > 256)
            fprintf(stderr, "[dpo-train] truncated %zu pairs to 256 (stack cap)\n", exp.count);
        hu_dpo_export_free(alloc, &exp);
        hu_dpo_collector_deinit(&col);
        sqlite3_close(db);
        if (n_pairs == 0) {
            fprintf(stderr, "[dpo-train] no usable pairs in DB after filter; "
                            "nothing to train on\n");
            trainer.vtable->deinit(trainer.ctx, alloc);
            return HU_ERR_NOT_SUPPORTED;
        }
    }

    for (int iter = 0; iter < max_iters; iter++) {
        hu_rl_trainer_metrics_t m = {0};
        hu_error_t serr = trainer.vtable->step(trainer.ctx, alloc, pairs, n_pairs, &m);
        if (serr != HU_OK) {
            fprintf(stderr, "[dpo-train] step %d failed: error %d\n", iter, (int)serr);
            break;
        }
        if ((iter + 1) % 10 == 0 || iter == max_iters - 1)
            fprintf(stderr, "[dpo-train] iter %d/%d loss=%.4f Δlogp_w=%.4f Δlogp_l=%.4f\n",
                    iter + 1, max_iters, m.final_loss, m.chosen_logprob_delta,
                    m.rejected_logprob_delta);
        if (m.adapter_path[0])
            fprintf(stderr, "[dpo-train] adapter written to %s\n", m.adapter_path);
    }

    /* No per-field frees — strings are inline char[] arrays in
     * hu_preference_pair_t. The pairs[] array itself is stack-allocated
     * and falls out of scope at function return. */
    trainer.vtable->deinit(trainer.ctx, alloc);
    return HU_OK;
}
