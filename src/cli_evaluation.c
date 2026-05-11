/* W16 evaluation CLI subcommand (FIX 16).
 *
 * Closes the W16 spec gap "All 6 suites runnable from CLI on a single
 * command". Wraps `hu_evaluation_run_suite` for each of the six factories
 * (locomo, longmemeval, dmr, minja, memoryagentbench, frontier_compare)
 * and emits the report as JSON.
 *
 *   human evaluation list
 *   human evaluation run <suite>
 *   human evaluation bench [--baseline FILE] [--write-baseline]
 *
 * The suite IDs match the factory short names. `bench` runs all six
 * back-to-back, optionally checking against a baseline JSON file and
 * exiting non-zero on regression. The frontier-compare backend is
 * included but currently emits a synthetic report (no live API calls);
 * when its API plumbing lands the same CLI surface keeps working.
 *
 * This is the surface the W16 GitHub Actions workflow drives.
 */

#include "human/cli_commands.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/evaluation/evaluation.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct evaluation_factory_entry {
    const char *id;
    const char *description;
    hu_error_t (*factory)(hu_allocator_t *alloc, hu_evaluation_t *out);
} evaluation_factory_entry_t;

static const evaluation_factory_entry_t kEvaluationFactories[] = {
    {"locomo", "Long-conversation recall (Snap LoCoMo, synthetic dataset)",
     hu_evaluation_locomo},
    {"longmemeval", "Five-task long-memory benchmark (synthetic dataset)",
     hu_evaluation_longmemeval},
    {"dmr", "Deep memory retrieval (vector recall on synthetic corpus)",
     hu_evaluation_dmr},
    {"minja", "Memory poisoning red-team (W1 write-trust gate)",
     hu_evaluation_minja},
    {"memoryagentbench", "Multi-agent shared-memory coordination (stub)",
     hu_evaluation_memoryagentbench},
    {"frontier", "h-uman vs frontier no-memory baseline (stub)",
     hu_evaluation_frontier_compare},
    {"legacy-bridge", "Legacy task-list framework adapted to W16 vtable",
     hu_evaluation_legacy_bridge},
    {"facade-recall", "Production v2 stack (facade + planner + executor) "
                     "scored against LoCoMo-style fact recall",
     hu_evaluation_facade_recall},
    {"locomo-facade", "Production v2 stack scored against the real 1542-prompt "
                     "LoCoMo corpus (requires real dataset on disk)",
     hu_evaluation_locomo_facade},
};

static const size_t kEvaluationFactoriesCount =
    sizeof(kEvaluationFactories) / sizeof(kEvaluationFactories[0]);

static const evaluation_factory_entry_t *find_factory(const char *id) {
    for (size_t i = 0; i < kEvaluationFactoriesCount; i++) {
        if (strcmp(kEvaluationFactories[i].id, id) == 0)
            return &kEvaluationFactories[i];
    }
    return NULL;
}

static void print_usage(void) {
    printf("Usage: human evaluation <list|run|bench> [args]\n");
    printf("  list                              Print available suites\n");
    printf("  run <suite>                       Run one suite, print JSON report on stdout\n");
    printf("  bench [--baseline FILE]           Run all six suites; aggregate JSON to stdout\n");
    printf("        [--write-baseline FILE]     After run, save current scores as new baseline\n");
    printf("        [--fail-on-regression]      Exit non-zero if regression gate fails\n");
    printf("\n");
    printf("Suites:\n");
    for (size_t i = 0; i < kEvaluationFactoriesCount; i++)
        printf("  %-20s %s\n", kEvaluationFactories[i].id,
               kEvaluationFactories[i].description);
    printf("\n");
    printf("Notes:\n");
    printf("  - All synthetic-dataset suites run offline. No API key needed.\n");
    printf("  - The frontier-compare suite is a stub today; live API plumbing\n");
    printf("    is documented in docs/plans/2026-05-10-w16-evaluation-suite.md.\n");
}

static hu_error_t cmd_list(void) {
    printf("[\n");
    for (size_t i = 0; i < kEvaluationFactoriesCount; i++) {
        printf("  {\"id\": \"%s\", \"description\": \"%s\"}%s\n",
               kEvaluationFactories[i].id,
               kEvaluationFactories[i].description,
               (i + 1 < kEvaluationFactoriesCount) ? "," : "");
    }
    printf("]\n");
    return HU_OK;
}

static hu_error_t run_one(hu_allocator_t *alloc, const evaluation_factory_entry_t *entry,
                         char **out_json, size_t *out_len) {
    hu_evaluation_t e;
    memset(&e, 0, sizeof(e));
    hu_error_t err = entry->factory(alloc, &e);
    if (err != HU_OK)
        return err;
    hu_evaluation_run_report_t report;
    memset(&report, 0, sizeof(report));
    err = hu_evaluation_run_suite(&e, &report);
    hu_evaluation_close(&e);
    if (err != HU_OK) {
        hu_evaluation_report_free(alloc, &report);
        return err;
    }
    err = hu_evaluation_report_to_json(alloc, &report, out_json, out_len);
    hu_evaluation_report_free(alloc, &report);
    return err;
}

static hu_error_t cmd_run(hu_allocator_t *alloc, int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: human evaluation run <suite>\n");
        return HU_ERR_INVALID_ARGUMENT;
    }
    const evaluation_factory_entry_t *entry = find_factory(argv[3]);
    if (!entry) {
        fprintf(stderr, "Unknown suite '%s' (try `human evaluation list`)\n", argv[3]);
        return HU_ERR_INVALID_ARGUMENT;
    }
    char *json = NULL;
    size_t json_len = 0;
    hu_error_t err = run_one(alloc, entry, &json, &json_len);
    if (err != HU_OK) {
        fprintf(stderr, "Suite '%s' failed: %s\n", entry->id, hu_error_string(err));
        if (json)
            alloc->free(alloc->ctx, json, json_len + 1);
        return err;
    }
    fwrite(json, 1, json_len, stdout);
    fputc('\n', stdout);
    alloc->free(alloc->ctx, json, json_len + 1);
    return HU_OK;
}

/* Read entire file into a heap buffer. Caller frees via alloc->free(). */
static hu_error_t slurp_file(hu_allocator_t *alloc, const char *path, char **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return HU_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    rewind(f);
    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);
        return HU_ERR_IO;
    }
    buf[sz] = '\0';
    *out = buf;
    *out_len = (size_t)sz;
    return HU_OK;
}

static hu_error_t write_file(const char *path, const char *buf, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return HU_ERR_IO;
    size_t wrote = fwrite(buf, 1, len, f);
    fclose(f);
    return (wrote == len) ? HU_OK : HU_ERR_IO;
}

static hu_error_t cmd_bench(hu_allocator_t *alloc, int argc, char **argv) {
    const char *baseline_path = NULL;
    const char *write_baseline_path = NULL;
    bool fail_on_regression = false;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--baseline") == 0 && i + 1 < argc) {
            baseline_path = argv[++i];
        } else if (strcmp(argv[i], "--write-baseline") == 0 && i + 1 < argc) {
            write_baseline_path = argv[++i];
        } else if (strcmp(argv[i], "--fail-on-regression") == 0) {
            fail_on_regression = true;
        } else {
            fprintf(stderr, "Unknown bench arg: %s\n", argv[i]);
            return HU_ERR_INVALID_ARGUMENT;
        }
    }

    /* Load baseline if requested. Missing baseline file is not fatal --
     * tools should be able to bootstrap a first run. */
    hu_evaluation_baseline_t baseline;
    memset(&baseline, 0, sizeof(baseline));
    bool have_baseline = false;
    if (baseline_path) {
        char *bjson = NULL;
        size_t blen = 0;
        hu_error_t bl = slurp_file(alloc, baseline_path, &bjson, &blen);
        if (bl == HU_OK) {
            bl = hu_evaluation_baseline_load(alloc, bjson, blen, &baseline);
            alloc->free(alloc->ctx, bjson, blen + 1);
            have_baseline = (bl == HU_OK);
            if (!have_baseline) {
                fprintf(stderr, "warning: baseline parse failed: %s\n",
                        hu_error_string(bl));
            }
        } else {
            fprintf(stderr, "warning: baseline file not readable (%s); proceeding without\n",
                    baseline_path);
        }
    }

    /* Aggregate report: emit JSON array of per-suite reports, plus a
     * trailing regression summary if a baseline was provided. */
    hu_evaluation_baseline_t fresh_baseline;
    memset(&fresh_baseline, 0, sizeof(fresh_baseline));
    fresh_baseline.entries = (hu_evaluation_baseline_entry_t *)alloc->alloc(
        alloc->ctx, sizeof(hu_evaluation_baseline_entry_t) *
                        (kEvaluationFactoriesCount * HU_EVALUATION_MAX_METRICS));
    if (!fresh_baseline.entries) {
        if (have_baseline)
            hu_evaluation_baseline_free(alloc, &baseline);
        return HU_ERR_OUT_OF_MEMORY;
    }

    bool any_regression = false;
    int exit_code = 0;
    printf("{\n  \"suites\": [\n");
    for (size_t i = 0; i < kEvaluationFactoriesCount; i++) {
        hu_evaluation_t e;
        memset(&e, 0, sizeof(e));
        hu_error_t err = kEvaluationFactories[i].factory(alloc, &e);
        if (err != HU_OK) {
            fprintf(stderr, "factory failed for '%s': %s\n",
                    kEvaluationFactories[i].id, hu_error_string(err));
            exit_code = 1;
            continue;
        }
        hu_evaluation_run_report_t report;
        memset(&report, 0, sizeof(report));
        err = hu_evaluation_run_suite(&e, &report);
        hu_evaluation_close(&e);
        if (err != HU_OK) {
            fprintf(stderr, "run failed for '%s': %s\n", kEvaluationFactories[i].id,
                    hu_error_string(err));
            hu_evaluation_report_free(alloc, &report);
            exit_code = 1;
            continue;
        }

        char *rjson = NULL;
        size_t rlen = 0;
        if (hu_evaluation_report_to_json(alloc, &report, &rjson, &rlen) == HU_OK) {
            fwrite(rjson, 1, rlen, stdout);
            if (i + 1 < kEvaluationFactoriesCount)
                fputs(",", stdout);
            fputs("\n", stdout);
            alloc->free(alloc->ctx, rjson, rlen + 1);
        }

        /* Capture metrics into the fresh baseline. */
        for (size_t m = 0; m < report.metrics_count; m++) {
            size_t idx = fresh_baseline.entries_count++;
            fresh_baseline.entries[idx].suite_name =
                report.suite_name ? strdup(report.suite_name) : NULL;
            fresh_baseline.entries[idx].metric_name =
                report.metrics[m].name ? strdup(report.metrics[m].name) : NULL;
            fresh_baseline.entries[idx].score = report.metrics[m].score;
            fresh_baseline.entries[idx].sample_count = report.metrics[m].sample_count;
        }

        /* Regression check against the LOADED baseline (not fresh). */
        if (have_baseline) {
            hu_evaluation_regression_result_t res;
            memset(&res, 0, sizeof(res));
            if (hu_evaluation_regression_check(alloc, &report, &baseline, &res) == HU_OK) {
                if (res.any_failed)
                    any_regression = true;
                hu_evaluation_regression_free(alloc, &res);
            }
        }
        hu_evaluation_report_free(alloc, &report);
    }
    printf("  ],\n  \"regression\": %s\n}\n",
           any_regression ? "\"FAILED\"" : "\"PASSED\"");

    /* Persist fresh baseline if requested. */
    if (write_baseline_path) {
        char *out_json = NULL;
        size_t out_len = 0;
        hu_error_t err = hu_evaluation_baseline_save(alloc, &fresh_baseline, &out_json, &out_len);
        if (err == HU_OK) {
            if (write_file(write_baseline_path, out_json, out_len) != HU_OK) {
                fprintf(stderr, "warning: failed to write baseline to %s\n",
                        write_baseline_path);
                exit_code = 1;
            }
            alloc->free(alloc->ctx, out_json, out_len + 1);
        }
    }

    /* Free fresh baseline strings (each strdup). */
    for (size_t k = 0; k < fresh_baseline.entries_count; k++) {
        free(fresh_baseline.entries[k].suite_name);
        free(fresh_baseline.entries[k].metric_name);
    }
    alloc->free(alloc->ctx, fresh_baseline.entries,
                sizeof(hu_evaluation_baseline_entry_t) *
                    (kEvaluationFactoriesCount * HU_EVALUATION_MAX_METRICS));

    if (have_baseline)
        hu_evaluation_baseline_free(alloc, &baseline);

    if (fail_on_regression && any_regression)
        return HU_ERR_TOOL_VALIDATION;
    return (exit_code == 0) ? HU_OK : HU_ERR_TOOL_VALIDATION;
}

hu_error_t cmd_evaluation(hu_allocator_t *alloc, int argc, char **argv) {
    if (argc < 3) {
        print_usage();
        return HU_OK;
    }
    const char *sub = argv[2];
    if (strcmp(sub, "list") == 0)
        return cmd_list();
    if (strcmp(sub, "run") == 0)
        return cmd_run(alloc, argc, argv);
    if (strcmp(sub, "bench") == 0)
        return cmd_bench(alloc, argc, argv);
    if (strcmp(sub, "help") == 0 || strcmp(sub, "--help") == 0 || strcmp(sub, "-h") == 0) {
        print_usage();
        return HU_OK;
    }
    fprintf(stderr, "Unknown evaluation subcommand '%s'\n", sub);
    print_usage();
    return HU_ERR_INVALID_ARGUMENT;
}
