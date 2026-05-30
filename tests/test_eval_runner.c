#include "human/core/string.h"
#include "human/eval.h"
#include "test_framework.h"
#include <string.h>

/* 2026-05-19 (M2): pin that hu_eval_suite_t::system_prompt round-trips
 * correctly through suite lifecycle. Without these tests, a future
 * contributor reverting line 33f8eaa5 (which threaded the persona prompt
 * into the runner) would only fail the noisy LLM-judge evals — no
 * automated guard catches the regression. */
static void test_eval_suite_system_prompt_set_and_free(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_suite_t suite;
    memset(&suite, 0, sizeof(suite));
    suite.name = hu_strdup(&alloc, "test-suite");
    HU_ASSERT_NOT_NULL(suite.name);
    const char *prompt = "You are Seth. ONE message. No markdown.";
    suite.system_prompt = hu_strdup(&alloc, prompt);
    suite.system_prompt_len = strlen(prompt);
    HU_ASSERT_NOT_NULL(suite.system_prompt);
    HU_ASSERT_EQ(suite.system_prompt_len, strlen(prompt));
    /* Free must zero + reclaim. ASan catches double-free + leak. */
    hu_eval_suite_free(&alloc, &suite);
    HU_ASSERT_TRUE(suite.system_prompt == NULL);
    HU_ASSERT_EQ(suite.system_prompt_len, (size_t)0);
}

static void test_eval_suite_null_system_prompt_is_legal(void) {
    /* Backward-compat: suites that don't set system_prompt must still work. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_suite_t suite;
    memset(&suite, 0, sizeof(suite));
    suite.name = hu_strdup(&alloc, "no-sysprompt");
    HU_ASSERT_TRUE(suite.system_prompt == NULL);
    HU_ASSERT_EQ(suite.system_prompt_len, (size_t)0);
    hu_eval_suite_free(&alloc, &suite);
}

static void eval_run_suite_null_args_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_suite_t suite = {0};
    hu_eval_run_t run = {0};
    const char *model = "test-model";
    size_t model_len = 10;

    HU_ASSERT_EQ(hu_eval_run_suite(NULL, NULL, model, model_len, &suite, HU_EVAL_EXACT, &run),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_eval_run_suite(&alloc, NULL, model, model_len, NULL, HU_EVAL_EXACT, &run),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_eval_run_suite(&alloc, NULL, model, model_len, &suite, HU_EVAL_EXACT, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

static void eval_run_suite_empty_suite_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"empty-suite\",\"tasks\":[]}";
    hu_eval_suite_t suite;
    hu_eval_run_t run = {0};

    HU_ASSERT_EQ(hu_eval_suite_load_json(&alloc, json, strlen(json), &suite), HU_OK);
    HU_ASSERT_EQ(hu_eval_run_suite(&alloc, NULL, NULL, 0, &suite, HU_EVAL_EXACT, &run), HU_OK);

    HU_ASSERT_EQ(run.results_count, 0u);
    HU_ASSERT_EQ(run.passed, 0u);
    HU_ASSERT_EQ(run.failed, 0u);
    HU_ASSERT_FLOAT_EQ(run.pass_rate, 1.0, 0.001);

    hu_eval_run_free(&alloc, &run);
    hu_eval_suite_free(&alloc, &suite);
}

static void eval_run_suite_mock_executes_all_tasks(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"name\":\"three-task\",\"tasks\":["
        "{\"id\":\"t1\",\"prompt\":\"Q1\",\"expected\":\"A1\",\"category\":\"c\",\"difficulty\":1},"
        "{\"id\":\"t2\",\"prompt\":\"Q2\",\"expected\":\"A2\",\"category\":\"c\",\"difficulty\":1},"
        "{\"id\":\"t3\",\"prompt\":\"Q3\",\"expected\":\"A3\",\"category\":\"c\",\"difficulty\":1}"
        "]}";
    hu_eval_suite_t suite;
    hu_eval_run_t run = {0};

    HU_ASSERT_EQ(hu_eval_suite_load_json(&alloc, json, strlen(json), &suite), HU_OK);
    HU_ASSERT_EQ(hu_eval_run_suite(&alloc, NULL, "mock", 4, &suite, HU_EVAL_EXACT, &run), HU_OK);

    HU_ASSERT_EQ(run.results_count, 3u);

    hu_eval_run_free(&alloc, &run);
    hu_eval_suite_free(&alloc, &suite);
}

static void eval_run_suite_tracks_pass_rate(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* Mock returns "Mock response for: {prompt}". Use expected that matches part of that. */
    const char *json = "{\"name\":\"pass-rate\",\"tasks\":["
                       "{\"id\":\"p1\",\"prompt\":\"What is "
                       "2+2?\",\"expected\":\"Mock\",\"category\":\"c\",\"difficulty\":1},"
                       "{\"id\":\"p2\",\"prompt\":\"Capital of "
                       "France?\",\"expected\":\"response\",\"category\":\"c\",\"difficulty\":1},"
                       "{\"id\":\"p3\",\"prompt\":\"Hello\",\"expected\":\"wrong\",\"category\":"
                       "\"c\",\"difficulty\":1}"
                       "]}";
    hu_eval_suite_t suite;
    hu_eval_run_t run = {0};

    HU_ASSERT_EQ(hu_eval_suite_load_json(&alloc, json, strlen(json), &suite), HU_OK);
    HU_ASSERT_EQ(hu_eval_run_suite(&alloc, NULL, "mock", 4, &suite, HU_EVAL_CONTAINS, &run), HU_OK);

    HU_ASSERT_EQ(run.results_count, 3u);
    HU_ASSERT_EQ(run.passed, 2u);
    HU_ASSERT_EQ(run.failed, 1u);
    HU_ASSERT_FLOAT_EQ(run.pass_rate, 2.0 / 3.0, 0.01);

    hu_eval_run_free(&alloc, &run);
    hu_eval_suite_free(&alloc, &suite);
}

static void eval_judge_word_overlap_passes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    bool passed = false;
    /* LLM judge: word overlap >= 50% of expected words. "hello world" has 2 words. */
    /* Actual "The answer is hello and world" contains both words. */
    HU_ASSERT_EQ(hu_eval_check(&alloc, "The answer is hello and world", 28, "hello world", 11,
                               HU_EVAL_LLM_JUDGE, &passed),
                 HU_OK);
    HU_ASSERT_TRUE(passed);
}

static void eval_judge_case_insensitive_passes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    bool passed = false;
    /* LLM judge: case-insensitive contains. */
    HU_ASSERT_EQ(hu_eval_check(&alloc, "hello world", 10, "HELLO", 5, HU_EVAL_LLM_JUDGE, &passed),
                 HU_OK);
    HU_ASSERT_TRUE(passed);
}

static void eval_compare_detects_regression(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_run_t baseline = {.pass_rate = 90.0};
    hu_eval_run_t current = {.pass_rate = 80.0};
    char *report = NULL;
    size_t rlen = 0;

    HU_ASSERT_EQ(hu_eval_compare(&alloc, &baseline, &current, &report, &rlen), HU_OK);
    HU_ASSERT_NOT_NULL(report);
    HU_ASSERT_TRUE(strstr(report, "\"delta\":-10.00") != NULL ||
                   strstr(report, "\"delta\":-10") != NULL);

    alloc.free(alloc.ctx, report, rlen + 1);
}

/* 2026-05-30: guard the empty-output footgun. fidelity.json / longitudinal silently
 * produced empty model output (a post-hoc timeout discarded valid slow generations)
 * and the runner scored them 0.00 — a generation failure masquerading as a humanness
 * regression, blinding `eval baseline` / `check-regression`. The pure predicate flags
 * a run INVALID when >50% of tasks are empty. The exactly-50% boundary is pinned so a
 * future `>`/`>=` slip is caught. */
static void eval_run_empty_invalid_flags_majority_empty(void) {
    /* strict majority empty -> INVALID */
    HU_ASSERT_TRUE(hu_eval_run_empty_invalid(18, 18)); /* all empty (the fidelity case) */
    HU_ASSERT_TRUE(hu_eval_run_empty_invalid(10, 18)); /* 56% */
    HU_ASSERT_TRUE(hu_eval_run_empty_invalid(5, 9));   /* 55% (the longitudinal case) */
    /* exactly 50% or fewer -> VALID (not auto-invalidated) */
    HU_ASSERT_FALSE(hu_eval_run_empty_invalid(9, 18)); /* exactly 50% boundary */
    HU_ASSERT_FALSE(hu_eval_run_empty_invalid(0, 18)); /* none empty */
    HU_ASSERT_FALSE(hu_eval_run_empty_invalid(2, 6));  /* 2 of 6 empty (multi_turn), <50% */
    /* defensive: zero tasks is never invalid (no division, no false alarm) */
    HU_ASSERT_FALSE(hu_eval_run_empty_invalid(0, 0));
    HU_ASSERT_FALSE(hu_eval_run_empty_invalid(1, 0));
}

void run_eval_runner_tests(void) {
    HU_TEST_SUITE("eval_runner");
    HU_RUN_TEST(eval_run_empty_invalid_flags_majority_empty);
    HU_RUN_TEST(test_eval_suite_system_prompt_set_and_free);
    HU_RUN_TEST(test_eval_suite_null_system_prompt_is_legal);
    HU_RUN_TEST(eval_run_suite_null_args_returns_error);
    HU_RUN_TEST(eval_run_suite_empty_suite_succeeds);
    HU_RUN_TEST(eval_run_suite_mock_executes_all_tasks);
    HU_RUN_TEST(eval_run_suite_tracks_pass_rate);
    HU_RUN_TEST(eval_judge_word_overlap_passes);
    HU_RUN_TEST(eval_judge_case_insensitive_passes);
    HU_RUN_TEST(eval_compare_detects_regression);
}
