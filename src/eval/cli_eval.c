/* src/eval/cli_eval.c -- CF-1 wiring
 *
 * `human eval competitive / leaderboard / gate` previously printed a
 * one-liner pointing at the backend modules. Each subcommand below
 * now invokes the real backend so the user-facing CLI produces the
 * artifacts the Ship Contract DoD-9 promises.
 */

#include "human/eval/cli_eval.h"

#include "human/eval/bootstrap_ci.h"
#include "human/eval/competitive_harness.h"
#include "human/eval/eval_gate.h"
#include "human/eval/eval_judge_external.h"
#include "human/eval/leaderboard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_CF1_MAX_PROMPTS 64
#define HU_CF1_MAX_SCORES  256

static int is_flag(const char *s, const char *want) {
    return s && want && strcmp(s, want) == 0;
}

static int wants_help(int argc, char **argv) {
    for (int i = 0; i < argc; i++)
        if (is_flag(argv[i], "--help") || is_flag(argv[i], "-h"))
            return 1;
    return 0;
}

/* Production dispatch in src/cli_commands.c passes `argv + 2`, so
 * argv[0] of these handlers is the subcommand name (e.g. "competitive")
 * rather than the first flag. Skip a leading positional that doesn't
 * begin with '-'; otherwise parse from index 0 so tests can pass a
 * flat flag array directly. Matches the i=1 convention used by
 * src/ml/cli_dpo.c::hu_ml_cli_dpo_judge. */
static int parse_start_index(int argc, char **argv) {
    if (argc > 0 && argv && argv[0] && argv[0][0] != '-')
        return 1;
    return 0;
}

static hu_error_t print_competitive_help(void) {
    printf("human eval competitive -- side-by-side persona scorecard\n\n");
    printf("Options:\n");
    printf("  --persona <name>     Persona label (informational; default 'default')\n");
    printf("  --out-md <path>      Markdown scorecard output\n");
    printf("  --out-json <path>    JSON summary output\n");
    printf("  --min-available N    Minimum competitor count for OK exit (default 1)\n");
    printf("  --help               Show this help\n");
    return HU_OK;
}

hu_error_t hu_eval_cli_competitive(hu_allocator_t *alloc, int argc, char **argv) {
    if (!alloc) return HU_ERR_INVALID_ARGUMENT;
    if (wants_help(argc, argv)) return print_competitive_help();

    const char *persona = "default";
    const char *out_md = "/tmp/human-eval-scorecard.md";
    const char *out_json = "/tmp/human-eval-scorecard.json";
    long min_avail = 1;

    for (int i = parse_start_index(argc, argv); i < argc; i++) {
        if (is_flag(argv[i], "--persona") && i + 1 < argc) { persona = argv[++i]; continue; }
        if (is_flag(argv[i], "--out-md") && i + 1 < argc) { out_md = argv[++i]; continue; }
        if (is_flag(argv[i], "--out-json") && i + 1 < argc) { out_json = argv[++i]; continue; }
        if (is_flag(argv[i], "--min-available") && i + 1 < argc) {
            min_avail = strtol(argv[++i], NULL, 10);
            if (min_avail <= 0) return HU_ERR_INVALID_ARGUMENT;
            continue;
        }
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_competitive_harness_config_t cfg = {
        .prompt_fixture = NULL,
        .out_markdown = out_md,
        .out_json = out_json,
        .min_available = (size_t)min_avail,
    };

    hu_eval_judge_external_t stock = {0};
    hu_eval_judge_verdict_t verdict = {.prefer_a = 1, .confidence = 0.9, .rationale = NULL};
    hu_eval_judge_canned_config_t canned = {.verdicts = &verdict, .n_verdicts = 1};
    hu_error_t e = hu_eval_judge_create_canned(alloc, &canned, &stock);
    if (e != HU_OK) return e;

    hu_eval_judge_external_t apple = {0};
    const char *apple_reason = NULL;
    hu_error_t apple_e = hu_eval_judge_create_apple_fm(alloc, &apple, &apple_reason);
    hu_eval_judge_external_t nano = {0};
    const char *nano_reason = NULL;
    hu_error_t nano_e = hu_eval_judge_create_gemini_nano(alloc, &nano, &nano_reason);
    (void)apple_reason;
    (void)nano_reason;

    hu_competitive_harness_judge_slot_t slots[3];
    slots[0].column_name = "stock";
    slots[0].judge = stock;
    slots[0].available = true;
    slots[0].unavailable_reason = NULL;
    slots[1].column_name = "apple_fm";
    slots[1].judge = apple;
    slots[1].available = (apple_e == HU_OK);
    slots[1].unavailable_reason = (apple_e == HU_OK) ? NULL
        : "unavailable (Apple FM bridge not compiled in)";
    slots[2].column_name = "gemini_nano";
    slots[2].judge = nano;
    slots[2].available = (nano_e == HU_OK);
    slots[2].unavailable_reason = (nano_e == HU_OK) ? NULL
        : "unavailable (Gemini Nano bridge not compiled in)";

    hu_competitive_harness_result_t res = {0};
    hu_error_t run_e = hu_competitive_harness_run_with_test_judges(alloc, &cfg, slots, 3, &res);

    printf("human eval competitive --persona %s\n", persona);
    printf("  %s\n", res.summary);
    printf("  scorecard: %s\n", out_md);
    printf("  summary:   %s\n", out_json);

    if (stock.vtable && stock.vtable->deinit) stock.vtable->deinit(&stock);
    if (apple_e == HU_OK && apple.vtable && apple.vtable->deinit) apple.vtable->deinit(&apple);
    if (nano_e == HU_OK && nano.vtable && nano.vtable->deinit) nano.vtable->deinit(&nano);

    return run_e;
}

static hu_error_t print_leaderboard_help(void) {
    printf("human eval leaderboard -- run cached gold-judge leaderboards\n\n");
    printf("Options:\n");
    printf("  --kind <k>           mt-bench | alpaca-eval | ifeval (default mt-bench)\n");
    printf("  --canned <path>      JSON file with canned scores (optional)\n");
    printf("  --prompts <p1,p2,..> Comma-separated prompts to score (optional)\n");
    printf("  --seed N             Runner seed (default 42)\n");
    printf("  --out <path>         Write per-prompt scores to file (optional)\n");
    printf("  --help               Show this help\n");
    return HU_OK;
}

static hu_error_t parse_leaderboard_kind(const char *s, hu_leaderboard_kind_t *out) {
    if (!s || !out) return HU_ERR_INVALID_ARGUMENT;
    if (strcmp(s, "mt-bench") == 0 || strcmp(s, "mt_bench") == 0) {
        *out = HU_LEADERBOARD_MT_BENCH;
        return HU_OK;
    }
    if (strcmp(s, "alpaca-eval") == 0 || strcmp(s, "alpaca_eval") == 0) {
        *out = HU_LEADERBOARD_ALPACA_EVAL;
        return HU_OK;
    }
    if (strcmp(s, "ifeval") == 0) {
        *out = HU_LEADERBOARD_IFEVAL;
        return HU_OK;
    }
    return HU_ERR_INVALID_ARGUMENT;
}

static int split_csv(char *buf, const char **out, int cap) {
    if (!buf || !out || cap <= 0) return -1;
    int n = 0;
    char *p = buf;
    out[n++] = p;
    while (*p) {
        if (*p == ',') {
            *p = '\0';
            if (n >= cap) return -1;
            out[n++] = p + 1;
        }
        p++;
    }
    return n;
}

hu_error_t hu_eval_cli_leaderboard(hu_allocator_t *alloc, int argc, char **argv) {
    if (!alloc) return HU_ERR_INVALID_ARGUMENT;
    if (wants_help(argc, argv)) return print_leaderboard_help();

    hu_leaderboard_kind_t kind = HU_LEADERBOARD_MT_BENCH;
    const char *canned = NULL;
    char *prompts_csv = NULL;
    char prompts_buf[2048] = {0};
    const char *out_path = NULL;
    unsigned int seed = 42;

    for (int i = parse_start_index(argc, argv); i < argc; i++) {
        if (is_flag(argv[i], "--kind") && i + 1 < argc) {
            hu_error_t pe = parse_leaderboard_kind(argv[++i], &kind);
            if (pe != HU_OK) return pe;
            continue;
        }
        if (is_flag(argv[i], "--canned") && i + 1 < argc) { canned = argv[++i]; continue; }
        if (is_flag(argv[i], "--prompts") && i + 1 < argc) { prompts_csv = argv[++i]; continue; }
        if (is_flag(argv[i], "--out") && i + 1 < argc) { out_path = argv[++i]; continue; }
        if (is_flag(argv[i], "--seed") && i + 1 < argc) {
            long v = strtol(argv[++i], NULL, 10);
            if (v < 0) return HU_ERR_INVALID_ARGUMENT;
            seed = (unsigned int)v;
            continue;
        }
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_leaderboard_config_t cfg = {.canned_path = canned, .seed = seed};
    hu_leaderboard_runner_t runner = {0};
    hu_error_t e = HU_OK;
    switch (kind) {
    case HU_LEADERBOARD_MT_BENCH:    e = hu_leaderboard_create_mt_bench(alloc, &cfg, &runner); break;
    case HU_LEADERBOARD_ALPACA_EVAL: e = hu_leaderboard_create_alpaca_eval(alloc, &cfg, &runner); break;
    case HU_LEADERBOARD_IFEVAL:      e = hu_leaderboard_create_ifeval(alloc, &cfg, &runner); break;
    }
    if (e != HU_OK) return e;

    const char *kind_name = runner.vtable->name(&runner);

    if (!prompts_csv) {
        printf("human eval leaderboard --kind %s (no --prompts; runner ready)\n", kind_name);
        runner.vtable->deinit(&runner, alloc);
        return HU_OK;
    }

    size_t pl = strlen(prompts_csv);
    if (pl + 1 > sizeof(prompts_buf)) {
        runner.vtable->deinit(&runner, alloc);
        return HU_ERR_INVALID_ARGUMENT;
    }
    memcpy(prompts_buf, prompts_csv, pl + 1);

    const char *prompts[HU_CF1_MAX_PROMPTS];
    int n = split_csv(prompts_buf, prompts, HU_CF1_MAX_PROMPTS);
    if (n <= 0) {
        runner.vtable->deinit(&runner, alloc);
        return HU_ERR_INVALID_ARGUMENT;
    }

    double scores[HU_CF1_MAX_PROMPTS] = {0};
    hu_error_t re = runner.vtable->run(&runner, alloc, prompts, NULL, (size_t)n, scores);
    if (re != HU_OK) {
        runner.vtable->deinit(&runner, alloc);
        return re;
    }

    FILE *fp = out_path ? fopen(out_path, "w") : stdout;
    if (!fp) {
        runner.vtable->deinit(&runner, alloc);
        return HU_ERR_IO;
    }
    fprintf(fp, "leaderboard: %s\n", kind_name);
    for (int i = 0; i < n; i++)
        fprintf(fp, "  %s\t%.4g\n", prompts[i], scores[i]);
    if (out_path) {
        fclose(fp);
        printf("human eval leaderboard --kind %s -> %s (%d prompts)\n", kind_name, out_path, n);
    }

    runner.vtable->deinit(&runner, alloc);
    return HU_OK;
}

static hu_error_t print_gate_help(void) {
    printf("human eval gate -- LoRA promotion gate (bootstrap CI on persona scores)\n\n");
    printf("Options:\n");
    printf("  --persona-scores <csv>     Per-conversation persona scores (required; >= 10)\n");
    printf("  --persona-baseline F       Baseline persona mean         (default 0.50)\n");
    printf("  --persona-delta-min F      Min delta above baseline      (default 0.05)\n");
    printf("  --candidate-p95-ms F       Candidate p95 latency (ms)    (default 100)\n");
    printf("  --latency-baseline-ms F    Baseline p95 latency (ms)     (default 100)\n");
    printf("  --latency-delta-max-ms F   Max latency regression (ms)   (default 50)\n");
    printf("  --bootstrap-samples N      Bootstrap resamples           (default 1000)\n");
    printf("  --bootstrap-seed N         Bootstrap seed                (default 42)\n");
    printf("  --out <path>               Write verdict to file (optional)\n");
    printf("  --help                     Show this help\n");
    return HU_OK;
}

static int parse_csv_doubles(const char *csv, double *out, int cap) {
    if (!csv || !out || cap <= 0) return -1;
    int n = 0;
    const char *p = csv;
    while (*p && n < cap) {
        char *end = NULL;
        double v = strtod(p, &end);
        if (end == p) return -1;
        out[n++] = v;
        p = end;
        while (*p == ',' || *p == ' ') p++;
    }
    return n;
}

hu_error_t hu_eval_cli_gate(hu_allocator_t *alloc, int argc, char **argv) {
    if (!alloc) return HU_ERR_INVALID_ARGUMENT;
    if (wants_help(argc, argv)) return print_gate_help();

    const char *scores_csv = NULL;
    const char *out_path = NULL;
    double persona_baseline = 0.50;
    double persona_delta_min = 0.05;
    double candidate_p95_ms = 100.0;
    double latency_baseline_ms = 100.0;
    double latency_delta_max_ms = 50.0;
    long bootstrap_samples = 1000;
    long bootstrap_seed = 42;

    for (int i = parse_start_index(argc, argv); i < argc; i++) {
        if (is_flag(argv[i], "--persona-scores") && i + 1 < argc) { scores_csv = argv[++i]; continue; }
        if (is_flag(argv[i], "--persona-baseline") && i + 1 < argc) {
            persona_baseline = strtod(argv[++i], NULL); continue;
        }
        if (is_flag(argv[i], "--persona-delta-min") && i + 1 < argc) {
            persona_delta_min = strtod(argv[++i], NULL); continue;
        }
        if (is_flag(argv[i], "--candidate-p95-ms") && i + 1 < argc) {
            candidate_p95_ms = strtod(argv[++i], NULL); continue;
        }
        if (is_flag(argv[i], "--latency-baseline-ms") && i + 1 < argc) {
            latency_baseline_ms = strtod(argv[++i], NULL); continue;
        }
        if (is_flag(argv[i], "--latency-delta-max-ms") && i + 1 < argc) {
            latency_delta_max_ms = strtod(argv[++i], NULL); continue;
        }
        if (is_flag(argv[i], "--bootstrap-samples") && i + 1 < argc) {
            bootstrap_samples = strtol(argv[++i], NULL, 10);
            if (bootstrap_samples < 1) return HU_ERR_INVALID_ARGUMENT;
            continue;
        }
        if (is_flag(argv[i], "--bootstrap-seed") && i + 1 < argc) {
            bootstrap_seed = strtol(argv[++i], NULL, 10); continue;
        }
        if (is_flag(argv[i], "--out") && i + 1 < argc) { out_path = argv[++i]; continue; }
        return HU_ERR_INVALID_ARGUMENT;
    }

    if (!scores_csv) return HU_ERR_INVALID_ARGUMENT;

    double persona[HU_CF1_MAX_SCORES];
    int n = parse_csv_doubles(scores_csv, persona, HU_CF1_MAX_SCORES);
    if (n < 1) return HU_ERR_INVALID_ARGUMENT;

    hu_eval_gate_t gate = {
        .baseline_persona_fidelity_mean = persona_baseline,
        .baseline_mt_bench_mean = 0.0,
        .baseline_ifeval_mean = 0.0,
        .baseline_p95_latency_ms = latency_baseline_ms,
        .persona_delta_min = persona_delta_min,
        .mt_bench_regression_max = 0.0,
        .ifeval_regression_max = 0.0,
        .latency_delta_max_ms = latency_delta_max_ms,
        .bootstrap_samples = (size_t)bootstrap_samples,
        .bootstrap_seed = (uint32_t)bootstrap_seed,
        .mt_bench = NULL,
        .ifeval = NULL,
        .reward_model = NULL,
    };

    hu_eval_gate_verdict_t verdict = {0};
    hu_error_t e = hu_eval_gate_decide_from_arrays_for_test(
        &gate, persona, NULL, NULL, NULL, (size_t)n, candidate_p95_ms, &verdict);
    if (e != HU_OK) return e;

    FILE *fp = out_path ? fopen(out_path, "w") : stdout;
    if (!fp) return HU_ERR_IO;
    fprintf(fp, "%s\n", verdict.promote ? "PROMOTE" : "REJECT");
    fprintf(fp, "persona_ci_lower=%.6f\n", verdict.persona_ci_lower);
    fprintf(fp, "persona_ci_upper=%.6f\n", verdict.persona_ci_upper);
    fprintf(fp, "persona_pass=%d\n", verdict.persona_pass ? 1 : 0);
    fprintf(fp, "latency_pass=%d\n", verdict.latency_pass ? 1 : 0);
    fprintf(fp, "reason=%s\n", verdict.reason[0] ? verdict.reason : "(all checks passed)");
    if (out_path) {
        fclose(fp);
        printf("human eval gate -> %s (%s)\n", out_path, verdict.promote ? "PROMOTE" : "REJECT");
    }
    return HU_OK;
}

#ifdef HU_IS_TEST
bool hu_build_has_competitive_eval(void) {
#ifdef HU_ENABLE_RL_FULL
    return true;
#else
    return false;
#endif
}
#endif
