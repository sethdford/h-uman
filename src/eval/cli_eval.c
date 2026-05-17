/* src/eval/cli_eval.c — CF-1 wiring for competitive / gate / leaderboard. */

#include "human/eval/cli_eval.h"

#include "human/eval/bootstrap_ci.h"
#include "human/eval/competitive_harness.h"
#include "human/eval/eval_gate.h"
#include "human/eval/eval_judge_external.h"
#include "human/eval/leaderboard.h"
#include "human/eval/persona_rollout.h"
#include "human/memory/personal_model.h"
#include "human/persona.h"
#include "human/provider.h"
#include "human/providers/factory.h"

#ifdef HU_IS_TEST
#include "human/provider_test_seam.h"
#endif

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

static int parse_start_index(int argc, char **argv) {
    int i = 0;
    if (argc > i && argv[i] && strcmp(argv[i], "human") == 0)
        i++;
    if (argc > i && argv[i] && argv[i][0] != '-')
        i++;
    return i;
}

static hu_error_t resolve_style_target(hu_allocator_t *alloc, const char *persona_name,
                                       hu_communication_style_t *out) {
    (void)alloc;
    memset(out, 0, sizeof(*out));
    if (persona_name && persona_name[0] && strcmp(persona_name, "default") != 0) {
        hu_persona_t persona = {0};
        if (hu_persona_load(alloc, persona_name, strlen(persona_name), &persona) == HU_OK) {
            hu_persona_deinit(alloc, &persona);
        }
    }
    char pm_path[1024];
    if (hu_personal_model_resolve_default_path(pm_path, sizeof(pm_path))) {
        hu_personal_model_t loaded;
        if (hu_personal_model_load(&loaded, pm_path) == HU_OK && loaded.style.sample_count > 0U) {
            *out = loaded.style;
            return HU_OK;
        }
    }
    out->sample_count = 1;
    out->lowercase_ratio = 0.7f;
    out->avg_message_length = 80.f;
    return HU_OK;
}

static void fill_slot_metrics(hu_competitive_harness_judge_slot_t *slot,
                              const hu_persona_rollout_result_t *rr, bool is_baseline,
                              double baseline_mean) {
    if (!slot || !rr || rr->n_scored < 10)
        return;
    double mean = 0.0;
    for (size_t i = 0; i < rr->n_scored; i++)
        mean += rr->persona_scores[i];
    mean /= (double)rr->n_scored;

    double lo = 0.0, hi = 0.0, ignored = 0.0;
    (void)hu_bootstrap_ci_for_test(rr->persona_scores, rr->n_scored, 0.95, 200, 42, &lo, &hi,
                                   &ignored);

    slot->has_persona_metrics = true;
    slot->persona_fidelity = mean;
    slot->ci_lower = lo;
    slot->ci_upper = hi;
    slot->n_samples = rr->n_scored;
    slot->p95_ms = rr->p95_ms;
    slot->is_baseline = is_baseline;
    if (!is_baseline && baseline_mean > 0.0) {
        slot->delta_vs_baseline = mean - baseline_mean;
        slot->delta_ci_lower = lo - baseline_mean;
        slot->delta_ci_upper = hi - baseline_mean;
    }
}

static hu_error_t print_competitive_help(void) {
    printf("human eval competitive -- side-by-side persona scorecard\n\n");
    printf("Options:\n");
    printf("  --persona <name>       Persona label for style fingerprint\n");
    printf("  --adapter <path>       Candidate LoRA adapter path\n");
    printf("  --prompts <fixture>    Newline-delimited prompt fixture (>=10 lines)\n");
    printf("  --out-md <path>        Markdown scorecard output\n");
    printf("  --out-json <path>      JSON summary output\n");
    printf("  --min-available N      Minimum competitor count for OK exit (default 1)\n");
    printf("  --help                 Show this help\n");
    return HU_OK;
}

hu_error_t hu_eval_cli_competitive(hu_allocator_t *alloc, int argc, char **argv) {
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (wants_help(argc, argv))
        return print_competitive_help();

    const char *persona = "default";
    const char *adapter = NULL;
    const char *prompts_path = NULL;
    const char *out_md = "/tmp/human-eval-scorecard.md";
    const char *out_json = "/tmp/human-eval-scorecard.json";
    long min_avail = 1;

    for (int i = parse_start_index(argc, argv); i < argc; i++) {
        if (is_flag(argv[i], "--persona") && i + 1 < argc) {
            persona = argv[++i];
            continue;
        }
        if (is_flag(argv[i], "--adapter") && i + 1 < argc) {
            adapter = argv[++i];
            continue;
        }
        if (is_flag(argv[i], "--prompts") && i + 1 < argc) {
            prompts_path = argv[++i];
            continue;
        }
        if (is_flag(argv[i], "--out-md") && i + 1 < argc) {
            out_md = argv[++i];
            continue;
        }
        if (is_flag(argv[i], "--out-json") && i + 1 < argc) {
            out_json = argv[++i];
            continue;
        }
        if (is_flag(argv[i], "--min-available") && i + 1 < argc) {
            min_avail = strtol(argv[++i], NULL, 10);
            if (min_avail <= 0)
                return HU_ERR_INVALID_ARGUMENT;
            continue;
        }
        return HU_ERR_INVALID_ARGUMENT;
    }

    if (!prompts_path)
        return HU_ERR_INVALID_ARGUMENT;

    char **prompts = NULL;
    size_t prompt_n = 0;
    hu_error_t le = hu_persona_rollout_load_prompt_fixture(alloc, prompts_path, &prompts, &prompt_n);
    if (le != HU_OK)
        return le;
    if (prompt_n < 10) {
        for (size_t i = 0; i < prompt_n; i++)
            alloc->free(alloc->ctx, prompts[i], strlen(prompts[i]) + 1);
        alloc->free(alloc->ctx, prompts, prompt_n * sizeof(char *));
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (prompt_n > HU_CF1_MAX_PROMPTS)
        prompt_n = HU_CF1_MAX_PROMPTS;

    hu_communication_style_t target;
    hu_error_t te = resolve_style_target(alloc, persona, &target);
    if (te != HU_OK) {
        for (size_t i = 0; i < prompt_n; i++)
            alloc->free(alloc->ctx, prompts[i], strlen(prompts[i]) + 1);
        alloc->free(alloc->ctx, prompts, prompt_n * sizeof(char *));
        return te;
    }

    hu_provider_t provider = {0};
    hu_error_t pe = HU_OK;
#ifdef HU_IS_TEST
    hu_provider_t *provider_heap = NULL;
    pe = hu_provider_create_for_test_with_canned_response(alloc, "canned: hey sounds good",
                                                          &provider_heap);
    if (pe == HU_OK)
        provider = *provider_heap;
#else
    pe = hu_provider_create(alloc, "huml", 4, NULL, 0, NULL, 0, &provider);
#endif
    if (pe != HU_OK) {
        for (size_t i = 0; i < prompt_n; i++)
            alloc->free(alloc->ctx, prompts[i], strlen(prompts[i]) + 1);
        alloc->free(alloc->ctx, prompts, prompt_n * sizeof(char *));
        return pe;
    }

    hu_persona_rollout_config_t base_cfg = {
        .provider = &provider,
        .adapter_path = NULL,
        .target = &target,
        .prompts = (const char **)prompts,
        .n_prompts = prompt_n,
        .timeout_ms_per_prompt = 5000,
    };
    hu_persona_rollout_result_t base_rr = {0};
    pe = hu_persona_rollout_run(alloc, &base_cfg, &base_rr);

    hu_persona_rollout_result_t cand_rr = {0};
    if (pe == HU_OK && adapter && adapter[0]) {
        hu_persona_rollout_config_t cand_cfg = base_cfg;
        cand_cfg.adapter_path = adapter;
        pe = hu_persona_rollout_run(alloc, &cand_cfg, &cand_rr);
    }

    for (size_t i = 0; i < prompt_n; i++)
        alloc->free(alloc->ctx, prompts[i], strlen(prompts[i]) + 1);
    alloc->free(alloc->ctx, prompts, prompt_n * sizeof(char *));

    if (pe != HU_OK) {
        hu_persona_rollout_result_free(alloc, &base_rr);
        hu_persona_rollout_result_free(alloc, &cand_rr);
        if (provider.vtable && provider.vtable->deinit)
            provider.vtable->deinit(provider.ctx, alloc);
        return pe;
    }

    hu_competitive_harness_config_t cfg = {
        .out_markdown = out_md,
        .out_json = out_json,
        .min_available = (size_t)min_avail,
    };

    hu_eval_judge_external_t stock = {0}, apple = {0}, nano = {0};
    hu_eval_judge_verdict_t verdict = {.prefer_a = 1, .confidence = 0.9, .rationale = NULL};
    hu_eval_judge_canned_config_t canned = {.verdicts = &verdict, .n_verdicts = 1};
    (void)hu_eval_judge_create_canned(alloc, &canned, &stock);

    const char *apple_reason = NULL;
    const char *nano_reason = NULL;
    hu_error_t apple_e = hu_eval_judge_create_apple_fm(alloc, &apple, &apple_reason);
    hu_error_t nano_e = hu_eval_judge_create_gemini_nano(alloc, &nano, &nano_reason);

    hu_competitive_harness_judge_slot_t slots[4];
    memset(slots, 0, sizeof(slots));
    slots[0].column_name = "stock";
    slots[0].judge = stock;
    slots[0].available = true;
    fill_slot_metrics(&slots[0], &base_rr, true, 0.0);

    slots[1].column_name = "candidate";
    slots[1].available = (adapter && adapter[0] && cand_rr.n_scored >= 10);
    if (slots[1].available) {
        double baseline_mean = slots[0].has_persona_metrics ? slots[0].persona_fidelity : 0.0;
        fill_slot_metrics(&slots[1], &cand_rr, false, baseline_mean);
    }

    slots[2].column_name = "apple_fm";
    slots[2].judge = apple;
    slots[2].available = (apple_e == HU_OK);
    slots[2].unavailable_reason = apple_reason;

    slots[3].column_name = "gemini_nano";
    slots[3].judge = nano;
    slots[3].available = (nano_e == HU_OK);
    slots[3].unavailable_reason = nano_reason;

    hu_competitive_harness_result_t res = {0};
    hu_error_t run_e =
        hu_competitive_harness_run_with_test_judges(alloc, &cfg, slots, 4, &res);

    printf("human eval competitive --persona %s\n", persona);
    printf("  %s\n", res.summary);
    printf("  scorecard: %s\n", out_md);
    printf("  summary:   %s\n", out_json);

    hu_persona_rollout_result_free(alloc, &base_rr);
    hu_persona_rollout_result_free(alloc, &cand_rr);
    if (stock.vtable && stock.vtable->deinit)
        stock.vtable->deinit(&stock);
    if (apple_e == HU_OK && apple.vtable && apple.vtable->deinit)
        apple.vtable->deinit(&apple);
    if (nano_e == HU_OK && nano.vtable && nano.vtable->deinit)
        nano.vtable->deinit(&nano);
#ifdef HU_IS_TEST
    if (provider_heap)
        hu_provider_destroy_for_test(provider_heap, alloc);
#else
    if (provider.vtable && provider.vtable->deinit)
        provider.vtable->deinit(provider.ctx, alloc);
#endif

    return run_e;
}

static hu_error_t print_leaderboard_help(void) {
    printf("human eval leaderboard -- run cached gold-judge leaderboards\n\n");
    printf("  --kind mt-bench|alpaca-eval|ifeval  --canned <path>  --prompts a,b,c\n");
    printf("  --out <path>  --seed N\n");
    return HU_OK;
}

static hu_error_t parse_leaderboard_kind(const char *s, hu_leaderboard_kind_t *out) {
    if (!s || !out)
        return HU_ERR_INVALID_ARGUMENT;
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
    if (!buf || !out || cap <= 0)
        return -1;
    int n = 0;
    char *p = buf;
    out[n++] = p;
    while (*p) {
        if (*p == ',') {
            *p = '\0';
            if (n >= cap)
                return -1;
            out[n++] = p + 1;
        }
        p++;
    }
    return n;
}

hu_error_t hu_eval_cli_leaderboard(hu_allocator_t *alloc, int argc, char **argv) {
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (wants_help(argc, argv))
        return print_leaderboard_help();

    hu_leaderboard_kind_t kind = HU_LEADERBOARD_MT_BENCH;
    const char *canned = NULL;
    char *prompts_csv = NULL;
    char prompts_buf[2048] = {0};
    const char *out_path = NULL;
    unsigned int seed = 42;

    for (int i = parse_start_index(argc, argv); i < argc; i++) {
        if (is_flag(argv[i], "--kind") && i + 1 < argc) {
            hu_error_t pe = parse_leaderboard_kind(argv[++i], &kind);
            if (pe != HU_OK)
                return pe;
            continue;
        }
        if (is_flag(argv[i], "--canned") && i + 1 < argc) {
            canned = argv[++i];
            continue;
        }
        if (is_flag(argv[i], "--prompts") && i + 1 < argc) {
            prompts_csv = argv[++i];
            continue;
        }
        if (is_flag(argv[i], "--out") && i + 1 < argc) {
            out_path = argv[++i];
            continue;
        }
        if (is_flag(argv[i], "--seed") && i + 1 < argc) {
            long v = strtol(argv[++i], NULL, 10);
            if (v < 0)
                return HU_ERR_INVALID_ARGUMENT;
            seed = (unsigned int)v;
            continue;
        }
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_leaderboard_config_t cfg = {.canned_path = canned, .seed = seed};
    hu_leaderboard_runner_t runner = {0};
    hu_error_t e = HU_OK;
    switch (kind) {
    case HU_LEADERBOARD_MT_BENCH:
        e = hu_leaderboard_create_mt_bench(alloc, &cfg, &runner);
        break;
    case HU_LEADERBOARD_ALPACA_EVAL:
        e = hu_leaderboard_create_alpaca_eval(alloc, &cfg, &runner);
        break;
    case HU_LEADERBOARD_IFEVAL:
        e = hu_leaderboard_create_ifeval(alloc, &cfg, &runner);
        break;
    }
    if (e != HU_OK)
        return e;

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
    double mean = 0.0;
    hu_error_t re = runner.vtable->run(&runner, alloc, prompts, NULL, (size_t)n, scores);
    if (re != HU_OK) {
        runner.vtable->deinit(&runner, alloc);
        return re;
    }
    for (int i = 0; i < n; i++)
        mean += scores[i];
    mean /= (double)n;

    FILE *fp = out_path ? fopen(out_path, "w") : stdout;
    if (!fp) {
        runner.vtable->deinit(&runner, alloc);
        return HU_ERR_IO;
    }
    fprintf(fp, "{\"kind\":\"%s\",\"mean\":%.6f,\"prompts\":[", kind_name, mean);
    for (int i = 0; i < n; i++)
        fprintf(fp, "%s\"%s\"", i ? "," : "", prompts[i]);
    fprintf(fp, "],\"scores\":[");
    for (int i = 0; i < n; i++)
        fprintf(fp, "%s%.6f", i ? "," : "", scores[i]);
    fprintf(fp, "]}\n");
    if (out_path) {
        fclose(fp);
        printf("human eval leaderboard --kind %s -> %s (%d prompts)\n", kind_name, out_path, n);
    }
    runner.vtable->deinit(&runner, alloc);
    return HU_OK;
}

static hu_error_t print_gate_help(void) {
    printf("human eval gate -- LoRA promotion gate (bootstrap CI)\n\n");
    printf("  --persona-scores <csv>  --out <path>  (>=10 scores required)\n");
    return HU_OK;
}

static int parse_csv_doubles(const char *csv, double *out, int cap) {
    if (!csv || !out || cap <= 0)
        return -1;
    int n = 0;
    const char *p = csv;
    while (*p && n < cap) {
        char *end = NULL;
        double v = strtod(p, &end);
        if (end == p)
            return -1;
        out[n++] = v;
        p = end;
        while (*p == ',' || *p == ' ')
            p++;
    }
    return n;
}

hu_error_t hu_eval_cli_gate(hu_allocator_t *alloc, int argc, char **argv) {
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (wants_help(argc, argv))
        return print_gate_help();

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
        if (is_flag(argv[i], "--persona-scores") && i + 1 < argc) {
            scores_csv = argv[++i];
            continue;
        }
        if (is_flag(argv[i], "--persona-baseline") && i + 1 < argc) {
            persona_baseline = strtod(argv[++i], NULL);
            continue;
        }
        if (is_flag(argv[i], "--persona-delta-min") && i + 1 < argc) {
            persona_delta_min = strtod(argv[++i], NULL);
            continue;
        }
        if (is_flag(argv[i], "--candidate-p95-ms") && i + 1 < argc) {
            candidate_p95_ms = strtod(argv[++i], NULL);
            continue;
        }
        if (is_flag(argv[i], "--latency-baseline-ms") && i + 1 < argc) {
            latency_baseline_ms = strtod(argv[++i], NULL);
            continue;
        }
        if (is_flag(argv[i], "--latency-delta-max-ms") && i + 1 < argc) {
            latency_delta_max_ms = strtod(argv[++i], NULL);
            continue;
        }
        if (is_flag(argv[i], "--bootstrap-samples") && i + 1 < argc) {
            bootstrap_samples = strtol(argv[++i], NULL, 10);
            if (bootstrap_samples < 1)
                return HU_ERR_INVALID_ARGUMENT;
            continue;
        }
        if (is_flag(argv[i], "--bootstrap-seed") && i + 1 < argc) {
            bootstrap_seed = strtol(argv[++i], NULL, 10);
            continue;
        }
        if (is_flag(argv[i], "--out") && i + 1 < argc) {
            out_path = argv[++i];
            continue;
        }
        return HU_ERR_INVALID_ARGUMENT;
    }

    if (!scores_csv)
        return HU_ERR_INVALID_ARGUMENT;

    double persona[HU_CF1_MAX_SCORES];
    int n = parse_csv_doubles(scores_csv, persona, HU_CF1_MAX_SCORES);
    if (n < 10)
        return HU_ERR_INVALID_ARGUMENT;

    hu_eval_gate_t gate = {
        .baseline_persona_fidelity_mean = persona_baseline,
        .baseline_p95_latency_ms = latency_baseline_ms,
        .persona_delta_min = persona_delta_min,
        .latency_delta_max_ms = latency_delta_max_ms,
        .bootstrap_samples = (size_t)bootstrap_samples,
        .bootstrap_seed = (uint32_t)bootstrap_seed,
    };

    hu_eval_gate_verdict_t verdict = {0};
    hu_error_t err = hu_eval_gate_decide_from_arrays_for_test(
        &gate, persona, NULL, NULL, NULL, (size_t)n, candidate_p95_ms, &verdict);
    if (err != HU_OK)
        return err;

    FILE *fp = out_path ? fopen(out_path, "w") : stdout;
    if (!fp)
        return HU_ERR_IO;
    fprintf(fp,
            "{\"promote\":%s,\"persona_pass\":%s,\"persona_ci_lower\":%.6f,"
            "\"persona_ci_upper\":%.6f,\"latency_pass\":%s,\"reason\":\"%s\"}\n",
            verdict.promote ? "true" : "false", verdict.persona_pass ? "true" : "false",
            verdict.persona_ci_lower, verdict.persona_ci_upper,
            verdict.latency_pass ? "true" : "false",
            verdict.reason[0] ? verdict.reason : "(all checks passed)");
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
