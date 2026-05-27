#include "human/eval.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

#ifndef HU_EVAL_SUITES_DIR
#error "HU_EVAL_SUITES_DIR must be defined when building human_tests"
#endif

static const struct eval_suite_expect_row {
    const char *file;
    size_t expect_tasks;
} k_eval_suite_expect[] = {
    {"adversarial.json", 10},
    {"adversarial_turing.json", 30},
    {"anti_sycophancy.json", 8},
    {"capability_edges.json", 10},
    {"coding_basic.json", 5},
    {"companion_safety.json", 12},
    {"fidelity.json", 18},
    {"hula_orchestration.json", 4},
    {"human_likeness.json", 8},
    {"humor_engine.json", 8},
    {"imessage_humanness.json", 8},
    {"inner_thoughts.json", 6},
    {"intelligence.json", 10},
    {"longitudinal.json", 9},
    {"memory.json", 8},
    {"multi_turn.json", 6},
    {"reasoning.json", 10},
    {"reasoning_basic.json", 10},
    {"social.json", 8},
    {"temporal_reasoning.json", 6},
    {"tier1_naturalness.json", 24}, /* M6-B 2026-05-26: doubled 12→24 (6/channel) */
    {"tool_capability.json", 8},
    {"tool_use.json", 8},
    {"tool_use_basic.json", 5},
    {"trust_repair.json", 10},
};

static void test_eval_load(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"test-suite\",\"tasks\":[]}";
    hu_eval_suite_t suite;
    hu_error_t err = hu_eval_suite_load_json(&alloc, json, strlen(json), &suite);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(suite.name != NULL);
    HU_ASSERT_STR_EQ(suite.name, "test-suite");
    HU_ASSERT_EQ(suite.tasks_count, 0u);
    HU_ASSERT_EQ(suite.default_match_mode, HU_EVAL_CONTAINS);
    hu_eval_suite_free(&alloc, &suite);
}

static void test_eval_load_match_mode_defaults(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"m\",\"match_mode\":\"llm_judge\",\"tasks\":["
                       "{\"id\":\"t1\",\"prompt\":\"p\",\"expected\":\"e\"}]}";
    hu_eval_suite_t suite;
    hu_error_t err = hu_eval_suite_load_json(&alloc, json, strlen(json), &suite);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(suite.default_match_mode, HU_EVAL_LLM_JUDGE);
    HU_ASSERT_EQ(suite.tasks_count, 1u);
    HU_ASSERT_EQ(suite.tasks[0].match_mode, HU_EVAL_LLM_JUDGE);
    hu_eval_suite_free(&alloc, &suite);
}

static void test_eval_load_task_match_mode_override(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"name\":\"m\",\"match_mode\":\"exact\",\"tasks\":["
        "{\"id\":\"t1\",\"prompt\":\"p\",\"expected\":\"e\",\"match_mode\":\"contains\"}]}";
    hu_eval_suite_t suite;
    HU_ASSERT_EQ(hu_eval_suite_load_json(&alloc, json, strlen(json), &suite), HU_OK);
    HU_ASSERT_EQ(suite.default_match_mode, HU_EVAL_EXACT);
    HU_ASSERT_EQ(suite.tasks[0].match_mode, HU_EVAL_CONTAINS);
    hu_eval_suite_free(&alloc, &suite);
}

static void test_eval_load_tasks(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"name\":\"reasoning\",\"tasks\":["
        "{\"id\":\"t1\",\"prompt\":\"What is "
        "2+2?\",\"expected\":\"4\",\"category\":\"math\",\"difficulty\":1,\"timeout_ms\":5000},"
        "{\"id\":\"t2\",\"prompt\":\"Capital of "
        "France?\",\"expected\":\"Paris\",\"category\":\"knowledge\",\"difficulty\":1}"
        "]}";
    hu_eval_suite_t suite;
    hu_error_t err = hu_eval_suite_load_json(&alloc, json, strlen(json), &suite);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(suite.name, "reasoning");
    HU_ASSERT_EQ(suite.tasks_count, 2u);
    HU_ASSERT(suite.tasks != NULL);
    HU_ASSERT_STR_EQ(suite.tasks[0].id, "t1");
    HU_ASSERT_STR_EQ(suite.tasks[0].prompt, "What is 2+2?");
    HU_ASSERT_STR_EQ(suite.tasks[0].expected, "4");
    HU_ASSERT_STR_EQ(suite.tasks[0].category, "math");
    HU_ASSERT_EQ(suite.tasks[0].difficulty, 1);
    HU_ASSERT_EQ(suite.tasks[0].timeout_ms, 5000);
    HU_ASSERT_STR_EQ(suite.tasks[1].id, "t2");
    HU_ASSERT_STR_EQ(suite.tasks[1].prompt, "Capital of France?");
    HU_ASSERT_STR_EQ(suite.tasks[1].expected, "Paris");
    HU_ASSERT_STR_EQ(suite.tasks[1].category, "knowledge");
    HU_ASSERT_EQ(suite.tasks[1].difficulty, 1);
    HU_ASSERT_EQ(suite.tasks[1].timeout_ms, 5000);
    hu_eval_suite_free(&alloc, &suite);
}

static void test_eval_llm_judge_placeholder(void) {
    hu_allocator_t alloc = hu_system_allocator();
    bool passed = false;
    HU_ASSERT_EQ(hu_eval_check(&alloc, "The answer is 4", 15, "4", 1, HU_EVAL_LLM_JUDGE, &passed),
                 HU_OK);
    HU_ASSERT(passed);
    passed = false;
    HU_ASSERT_EQ(
        hu_eval_check(&alloc, "Paris is the capital", 19, "Paris", 5, HU_EVAL_LLM_JUDGE, &passed),
        HU_OK);
    HU_ASSERT(passed);
    passed = true;
    HU_ASSERT_EQ(hu_eval_check(&alloc, "wrong answer", 12, "Paris", 5, HU_EVAL_LLM_JUDGE, &passed),
                 HU_OK);
    HU_ASSERT(!passed);
}

static void test_eval_llm_judge_case_insensitive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    bool passed = false;
    HU_ASSERT_EQ(
        hu_eval_check(&alloc, "the CAPITAL is paris", 20, "Paris", 5, HU_EVAL_LLM_JUDGE, &passed),
        HU_OK);
    HU_ASSERT(passed);
    passed = false;
    HU_ASSERT_EQ(hu_eval_check(&alloc, "HELLO WORLD", 11, "hello", 5, HU_EVAL_LLM_JUDGE, &passed),
                 HU_OK);
    HU_ASSERT(passed);
}

static void test_eval_llm_judge_word_overlap(void) {
    hu_allocator_t alloc = hu_system_allocator();
    bool passed = false;
    HU_ASSERT_EQ(hu_eval_check(
                     &alloc,
                     "The transformer architecture uses attention mechanisms for sequence modeling",
                     75, "transformer attention sequence", 30, HU_EVAL_LLM_JUDGE, &passed),
                 HU_OK);
    HU_ASSERT(passed);

    passed = true;
    HU_ASSERT_EQ(hu_eval_check(&alloc, "The weather is sunny today", 26,
                               "transformer attention sequence modeling", 39, HU_EVAL_LLM_JUDGE,
                               &passed),
                 HU_OK);
    HU_ASSERT(!passed);
}

static void test_eval_llm_judge_word_overlap_threshold(void) {
    hu_allocator_t alloc = hu_system_allocator();
    bool passed = false;
    HU_ASSERT_EQ(hu_eval_check(&alloc, "machine learning optimization gradient", 38,
                               "machine learning optimization gradient descent backprop", 55,
                               HU_EVAL_LLM_JUDGE, &passed),
                 HU_OK);
    HU_ASSERT(passed);

    passed = true;
    HU_ASSERT_EQ(hu_eval_check(&alloc, "machine zzzzz yyyyy xxxxx", 25,
                               "machine learning optimization gradient descent backprop", 55,
                               HU_EVAL_LLM_JUDGE, &passed),
                 HU_OK);
    HU_ASSERT(!passed);
}

static void test_eval_exact(void) {
    hu_allocator_t alloc = hu_system_allocator();
    bool passed = false;
    HU_ASSERT_EQ(hu_eval_check(&alloc, "hello", 5, "hello", 5, HU_EVAL_EXACT, &passed), HU_OK);
    HU_ASSERT(passed);
    HU_ASSERT_EQ(hu_eval_check(&alloc, "hello", 5, "world", 5, HU_EVAL_EXACT, &passed), HU_OK);
    HU_ASSERT(!passed);
}

static void test_eval_contains(void) {
    hu_allocator_t alloc = hu_system_allocator();
    bool passed = false;
    HU_ASSERT_EQ(hu_eval_check(&alloc, "hello world", 11, "world", 5, HU_EVAL_CONTAINS, &passed),
                 HU_OK);
    HU_ASSERT(passed);
}

static void test_eval_numeric(void) {
    hu_allocator_t alloc = hu_system_allocator();
    bool passed = false;
    HU_ASSERT_EQ(hu_eval_check(&alloc, "3.14", 4, "3.14", 4, HU_EVAL_NUMERIC_CLOSE, &passed),
                 HU_OK);
    HU_ASSERT(passed);
}

static void test_eval_report(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_run_t run = {.suite_name = "s",
                         .provider = "p",
                         .model = "m",
                         .passed = 5,
                         .failed = 1,
                         .pass_rate = 83.33,
                         .total_elapsed_ms = 1000};
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_eval_report_json(&alloc, &run, &out, &out_len), HU_OK);
    HU_ASSERT(out != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_eval_compare(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_run_t b = {.pass_rate = 80.0};
    hu_eval_run_t c = {.pass_rate = 90.0};
    char *report = NULL;
    size_t rlen = 0;
    HU_ASSERT_EQ(hu_eval_compare(&alloc, &b, &c, &report, &rlen), HU_OK);
    HU_ASSERT(report != NULL);
    alloc.free(alloc.ctx, report, rlen + 1);
}

static void test_eval_run_suite_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_suite_t suite = {0};
    hu_eval_run_t run = {0};
    HU_ASSERT_EQ(hu_eval_run_suite(NULL, NULL, NULL, 0, &suite, HU_EVAL_EXACT, &run),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_eval_run_suite(&alloc, NULL, NULL, 0, NULL, HU_EVAL_EXACT, &run),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_eval_run_suite(&alloc, NULL, NULL, 0, &suite, HU_EVAL_EXACT, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

static void test_eval_run_suite_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_suite_t suite = {0};
    suite.name = "empty-suite";
    suite.tasks = NULL;
    suite.tasks_count = 0;
    hu_eval_run_t run = {0};
    HU_ASSERT_EQ(hu_eval_run_suite(&alloc, NULL, "model", 5, &suite, HU_EVAL_EXACT, &run), HU_OK);
    HU_ASSERT_NOT_NULL(run.suite_name);
    HU_ASSERT_STR_EQ(run.suite_name, "empty-suite");
    HU_ASSERT_EQ(run.results_count, 0u);
    HU_ASSERT_EQ(run.passed, 0u);
    HU_ASSERT_EQ(run.failed, 0u);
    HU_ASSERT_FLOAT_EQ(run.pass_rate, 1.0, 0.001);
    hu_eval_run_free(&alloc, &run);
}

static void test_eval_run_suite_with_tasks(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"basic\",\"tasks\":["
                       "{\"id\":\"t1\",\"prompt\":\"What is "
                       "2+2?\",\"expected\":\"4\",\"category\":\"math\",\"difficulty\":1}"
                       "]}";
    hu_eval_suite_t suite = {0};
    HU_ASSERT_EQ(hu_eval_suite_load_json(&alloc, json, strlen(json), &suite), HU_OK);
    HU_ASSERT_EQ(suite.tasks_count, 1u);

    hu_eval_run_t run = {0};
    hu_error_t err = hu_eval_run_suite(&alloc, NULL, "test", 4, &suite, HU_EVAL_EXACT, &run);
    /* NULL provider: in HU_IS_TEST either succeeds with mock or returns validation error */
    if (err == HU_OK) {
        HU_ASSERT_TRUE(run.results_count <= suite.tasks_count);
        hu_eval_run_free(&alloc, &run);
    } else {
        HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    }

    hu_eval_suite_free(&alloc, &suite);
}

static void test_eval_run_suite_report_roundtrip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"rt\",\"tasks\":["
                       "{\"id\":\"t1\",\"prompt\":\"hello\",\"expected\":\"hello\",\"category\":"
                       "\"test\",\"difficulty\":1}"
                       "]}";
    hu_eval_suite_t suite = {0};
    HU_ASSERT_EQ(hu_eval_suite_load_json(&alloc, json, strlen(json), &suite), HU_OK);

    hu_eval_run_t run = {0};
    HU_ASSERT_EQ(hu_eval_run_suite(&alloc, NULL, NULL, 0, &suite, HU_EVAL_CONTAINS, &run), HU_OK);

    char *report = NULL;
    size_t rlen = 0;
    HU_ASSERT_EQ(hu_eval_report_json(&alloc, &run, &report, &rlen), HU_OK);
    HU_ASSERT_NOT_NULL(report);
    HU_ASSERT_TRUE(rlen > 0);
    HU_ASSERT_TRUE(strstr(report, "\"suite\"") != NULL);
    HU_ASSERT_TRUE(strstr(report, "\"passed\"") != NULL);

    alloc.free(alloc.ctx, report, rlen + 1);
    hu_eval_run_free(&alloc, &run);
    hu_eval_suite_free(&alloc, &suite);
}

static void test_eval_run_load_json_valid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"suite\":\"my-suite\",\"passed\":8,\"failed\":2,\"pass_rate\":0.80,\"elapsed_ms\":1500}";
    hu_eval_run_t run = {0};
    HU_ASSERT_EQ(hu_eval_run_load_json(&alloc, json, strlen(json), &run), HU_OK);
    HU_ASSERT_NOT_NULL(run.suite_name);
    HU_ASSERT_STR_EQ(run.suite_name, "my-suite");
    HU_ASSERT_EQ(run.passed, 8u);
    HU_ASSERT_EQ(run.failed, 2u);
    HU_ASSERT_FLOAT_EQ(run.pass_rate, 0.80, 0.01);
    HU_ASSERT_EQ(run.total_elapsed_ms, 1500);
    hu_eval_run_free(&alloc, &run);
}

static void test_eval_run_load_json_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_run_t run = {0};
    HU_ASSERT_EQ(hu_eval_run_load_json(NULL, "{}", 2, &run), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_eval_run_load_json(&alloc, NULL, 2, &run), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_eval_run_load_json(&alloc, "{}", 0, &run), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_eval_run_load_json(&alloc, "{}", 2, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_eval_run_load_json_partial(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"suite\":\"partial\",\"passed\":3}";
    hu_eval_run_t run = {0};
    HU_ASSERT_EQ(hu_eval_run_load_json(&alloc, json, strlen(json), &run), HU_OK);
    HU_ASSERT_NOT_NULL(run.suite_name);
    HU_ASSERT_STR_EQ(run.suite_name, "partial");
    HU_ASSERT_EQ(run.passed, 3u);
    HU_ASSERT_EQ(run.failed, 0u);
    HU_ASSERT_FLOAT_EQ(run.pass_rate, 0.0, 0.001);
    hu_eval_run_free(&alloc, &run);
}

static void test_eval_check_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    bool passed = false;
    HU_ASSERT_EQ(hu_eval_check(NULL, "a", 1, "a", 1, HU_EVAL_EXACT, &passed),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_eval_check(&alloc, NULL, 1, "a", 1, HU_EVAL_EXACT, &passed),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_eval_check(&alloc, "a", 1, NULL, 1, HU_EVAL_EXACT, &passed),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_eval_check(&alloc, "a", 1, "a", 1, HU_EVAL_EXACT, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

static void test_eval_judge_with_provider_null_falls_back(void) {
    hu_allocator_t alloc = hu_system_allocator();
    bool passed = false;
    double score = -1.0;
    HU_ASSERT_EQ(hu_eval_check_with_provider(&alloc, "The answer is 4", 15, "4", 1,
                                             HU_EVAL_LLM_JUDGE, NULL, NULL, 0, &passed, &score),
                 HU_OK);
    HU_ASSERT(passed);
    HU_ASSERT_TRUE(score >= 0.0 && score <= 1.0);
}

static void test_eval_judge_heuristic_still_works(void) {
    hu_allocator_t alloc = hu_system_allocator();
    bool passed = false;
    HU_ASSERT_EQ(
        hu_eval_check(&alloc, "Paris is the capital", 19, "Paris", 5, HU_EVAL_LLM_JUDGE, &passed),
        HU_OK);
    HU_ASSERT(passed);
    passed = true;
    HU_ASSERT_EQ(hu_eval_check(&alloc, "wrong answer", 12, "Paris", 5, HU_EVAL_LLM_JUDGE, &passed),
                 HU_OK);
    HU_ASSERT(!passed);
}

static void test_eval_suite_load_json_path_missing_returns_io(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_suite_t suite = {0};
    HU_ASSERT_EQ(
        hu_eval_suite_load_json_path(&alloc, "/nonexistent/human_eval_suite_xyz.json", &suite),
        HU_ERR_IO);
}

static void test_eval_expanded_suite_json_files_parse_unique_ids_expected_counts(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_suite_t suites[sizeof(k_eval_suite_expect) / sizeof(k_eval_suite_expect[0])];
    memset(suites, 0, sizeof(suites));

    for (size_t si = 0; si < sizeof(k_eval_suite_expect) / sizeof(k_eval_suite_expect[0]); si++) {
        char path[768];
        int n =
            snprintf(path, sizeof(path), "%s/%s", HU_EVAL_SUITES_DIR, k_eval_suite_expect[si].file);
        HU_ASSERT(n > 0 && (size_t)n < sizeof(path));
        HU_ASSERT_EQ(hu_eval_suite_load_json_path(&alloc, path, &suites[si]), HU_OK);
        HU_ASSERT_EQ(suites[si].tasks_count, k_eval_suite_expect[si].expect_tasks);
    }

    for (size_t i = 0; i < sizeof(k_eval_suite_expect) / sizeof(k_eval_suite_expect[0]); i++) {
        for (size_t ti = 0; ti < suites[i].tasks_count; ti++) {
            HU_ASSERT_NOT_NULL(suites[i].tasks[ti].id);
            for (size_t j = i; j < sizeof(k_eval_suite_expect) / sizeof(k_eval_suite_expect[0]);
                 j++) {
                size_t tj_start = (j == i) ? (ti + 1) : 0;
                for (size_t tj = tj_start; tj < suites[j].tasks_count; tj++) {
                    if (strcmp(suites[i].tasks[ti].id, suites[j].tasks[tj].id) == 0)
                        HU_FAIL("duplicate eval task id across suites: %s", suites[i].tasks[ti].id);
                }
            }
        }
    }

    for (size_t si = 0; si < sizeof(k_eval_suite_expect) / sizeof(k_eval_suite_expect[0]); si++)
        hu_eval_suite_free(&alloc, &suites[si]);
}

#if defined(__unix__) || defined(__APPLE__)
static void test_eval_smoke_validate_eval_suites_directory(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_validate_stats_t st = {0};
    HU_ASSERT_EQ(hu_eval_suites_validate_dir(&alloc, HU_EVAL_SUITES_DIR, NULL, &st), HU_OK);
    HU_ASSERT_EQ(st.errors, 0u);
    HU_ASSERT_EQ(st.suites_ok, sizeof(k_eval_suite_expect) / sizeof(k_eval_suite_expect[0]));
    size_t expect_tasks = 0;
    for (size_t i = 0; i < sizeof(k_eval_suite_expect) / sizeof(k_eval_suite_expect[0]); i++)
        expect_tasks += k_eval_suite_expect[i].expect_tasks;
    HU_ASSERT_EQ(st.tasks, expect_tasks);
}
#endif

static void test_eval_baseline_status_for_score_thresholds(void) {
    HU_ASSERT_STR_EQ(hu_eval_baseline_status_for_score(0.90), "SOTA");
    HU_ASSERT_STR_EQ(hu_eval_baseline_status_for_score(0.85), "SOTA");
    HU_ASSERT_STR_EQ(hu_eval_baseline_status_for_score(0.75), "COMPETITIVE");
    HU_ASSERT_STR_EQ(hu_eval_baseline_status_for_score(0.70), "COMPETITIVE");
    HU_ASSERT_STR_EQ(hu_eval_baseline_status_for_score(0.65), "PARTIAL");
    HU_ASSERT_STR_EQ(hu_eval_baseline_status_for_score(0.50), "PARTIAL");
    HU_ASSERT_STR_EQ(hu_eval_baseline_status_for_score(0.49), "BASIC");
    HU_ASSERT_STR_EQ(hu_eval_baseline_status_for_score(0.0), "BASIC");
}

static void test_eval_baseline_try_mock_scores_for_known_stems(void) {
    double s = 0.0;
    HU_ASSERT_FALSE(hu_eval_baseline_try_mock_score_for_stem(NULL, &s));
    HU_ASSERT_FALSE(hu_eval_baseline_try_mock_score_for_stem("fidelity", NULL));
#if defined(HU_IS_TEST) && HU_IS_TEST
    HU_ASSERT_TRUE(hu_eval_baseline_try_mock_score_for_stem("fidelity", &s));
    HU_ASSERT_FLOAT_EQ(s, 0.72, 0.001);
    HU_ASSERT_TRUE(hu_eval_baseline_try_mock_score_for_stem("intelligence", &s));
    HU_ASSERT_FLOAT_EQ(s, 0.65, 0.001);
    HU_ASSERT_FALSE(hu_eval_baseline_try_mock_score_for_stem("coding_basic", &s));
#else
    HU_ASSERT_FALSE(hu_eval_baseline_try_mock_score_for_stem("fidelity", &s));
#endif
}

static void test_eval_run_free(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_run_t run = {0};
    run.suite_name = alloc.alloc(alloc.ctx, 8);
    if (run.suite_name)
        memcpy(run.suite_name, "suite", 6);
    run.provider = alloc.alloc(alloc.ctx, 6);
    if (run.provider)
        memcpy(run.provider, "p", 2);
    run.model = alloc.alloc(alloc.ctx, 6);
    if (run.model)
        memcpy(run.model, "m", 2);
    run.results = alloc.alloc(alloc.ctx, sizeof(hu_eval_result_t));
    run.results_count = 1;
    if (run.results) {
        memset(&run.results[0], 0, sizeof(run.results[0]));
        run.results[0].task_id = alloc.alloc(alloc.ctx, 4);
        if (run.results[0].task_id)
            memcpy(run.results[0].task_id, "t1", 3);
        run.results[0].actual_output = alloc.alloc(alloc.ctx, 6);
        if (run.results[0].actual_output) {
            memcpy(run.results[0].actual_output, "out", 4);
            run.results[0].actual_output_len = 3;
        }
    }
    hu_eval_run_free(&alloc, &run);
    HU_ASSERT(run.suite_name == NULL);
    HU_ASSERT(run.results == NULL);
}

static void test_eval_empathy_trajectory_increasing(void) {
    float scores[] = {0.3f, 0.5f, 0.6f, 0.8f, 0.9f};
    hu_eval_empathy_trajectory_t result;
    hu_error_t err = hu_eval_score_empathy_trajectory(scores, 5, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.directional_alignment > 0.9f);
    HU_ASSERT_TRUE(result.stability > 0.9f);
    HU_ASSERT_TRUE(result.composite > 0.5f);
}

static void test_eval_empathy_trajectory_regressing(void) {
    float scores[] = {0.8f, 0.6f, 0.3f, 0.2f};
    hu_eval_empathy_trajectory_t result;
    hu_error_t err = hu_eval_score_empathy_trajectory(scores, 4, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.directional_alignment < 0.4f);
    HU_ASSERT_TRUE(result.stability < 0.7f);
}

static void test_eval_empathy_trajectory_null(void) {
    hu_eval_empathy_trajectory_t result;
    hu_error_t err = hu_eval_score_empathy_trajectory(NULL, 0, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.composite == 0.0f);
}

static void test_eval_consistency_high(void) {
    float prompt_sims[] = {0.9f, 0.85f, 0.92f};
    float turn_sims[] = {0.88f, 0.91f};
    hu_eval_consistency_metrics_t result;
    hu_error_t err = hu_eval_score_consistency(prompt_sims, 3, turn_sims, 2, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.prompt_to_line > 0.8f);
    HU_ASSERT_TRUE(result.line_to_line > 0.85f);
    HU_ASSERT_TRUE(result.composite > 0.8f);
}

static void test_eval_consistency_null(void) {
    hu_eval_consistency_metrics_t result;
    hu_error_t err = hu_eval_score_consistency(NULL, 0, NULL, 0, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.composite == 0.0f);
}

static void test_eval_antisycophancy_all_held(void) {
    bool opinions[] = {true, true, true, true};
    float score = hu_eval_score_antisycophancy(opinions, 4);
    HU_ASSERT_TRUE(score > 0.99f);
}

static void test_eval_antisycophancy_none_held(void) {
    bool opinions[] = {false, false, false};
    float score = hu_eval_score_antisycophancy(opinions, 3);
    HU_ASSERT_TRUE(score < 0.01f);
}

static void test_eval_antisycophancy_mixed(void) {
    bool opinions[] = {true, false, true, false};
    float score = hu_eval_score_antisycophancy(opinions, 4);
    HU_ASSERT_TRUE(score > 0.4f && score < 0.6f);
}

static void test_eval_antisycophancy_null(void) {
    float score = hu_eval_score_antisycophancy(NULL, 0);
    HU_ASSERT_TRUE(score == 0.0f);
}

void run_eval_tests(void) {
    HU_TEST_SUITE("Evaluation Harness");
    HU_RUN_TEST(test_eval_load);
    HU_RUN_TEST(test_eval_load_match_mode_defaults);
    HU_RUN_TEST(test_eval_load_task_match_mode_override);
    HU_RUN_TEST(test_eval_load_tasks);
    HU_RUN_TEST(test_eval_exact);
    HU_RUN_TEST(test_eval_contains);
    HU_RUN_TEST(test_eval_numeric);
    HU_RUN_TEST(test_eval_llm_judge_placeholder);
    HU_RUN_TEST(test_eval_llm_judge_case_insensitive);
    HU_RUN_TEST(test_eval_llm_judge_word_overlap);
    HU_RUN_TEST(test_eval_llm_judge_word_overlap_threshold);
    HU_RUN_TEST(test_eval_judge_with_provider_null_falls_back);
    HU_RUN_TEST(test_eval_judge_heuristic_still_works);
    HU_RUN_TEST(test_eval_check_null_args);
    HU_RUN_TEST(test_eval_run_suite_null_args);
    HU_RUN_TEST(test_eval_run_suite_empty);
    HU_RUN_TEST(test_eval_run_suite_with_tasks);
    HU_RUN_TEST(test_eval_run_suite_report_roundtrip);
    HU_RUN_TEST(test_eval_run_load_json_valid);
    HU_RUN_TEST(test_eval_run_load_json_null_args);
    HU_RUN_TEST(test_eval_run_load_json_partial);
    HU_RUN_TEST(test_eval_report);
    HU_RUN_TEST(test_eval_compare);
    HU_RUN_TEST(test_eval_baseline_status_for_score_thresholds);
    HU_RUN_TEST(test_eval_baseline_try_mock_scores_for_known_stems);
    HU_RUN_TEST(test_eval_suite_load_json_path_missing_returns_io);
    HU_RUN_TEST(test_eval_expanded_suite_json_files_parse_unique_ids_expected_counts);
#if defined(__unix__) || defined(__APPLE__)
    HU_RUN_TEST(test_eval_smoke_validate_eval_suites_directory);
#endif
    HU_RUN_TEST(test_eval_run_free);
    HU_RUN_TEST(test_eval_empathy_trajectory_increasing);
    HU_RUN_TEST(test_eval_empathy_trajectory_regressing);
    HU_RUN_TEST(test_eval_empathy_trajectory_null);
    HU_RUN_TEST(test_eval_consistency_high);
    HU_RUN_TEST(test_eval_consistency_null);
    HU_RUN_TEST(test_eval_antisycophancy_all_held);
    HU_RUN_TEST(test_eval_antisycophancy_none_held);
    HU_RUN_TEST(test_eval_antisycophancy_mixed);
    HU_RUN_TEST(test_eval_antisycophancy_null);
}
