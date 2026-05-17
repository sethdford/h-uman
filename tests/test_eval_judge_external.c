#include "test_framework.h"
#include "human/eval/eval_judge_external.h"
#include "human/core/allocator.h"
#include "human/core/error.h"

#include <math.h>
#include <string.h>

/*
 * Phase 5 Task 3 — `hu_eval_judge_external_t` vtable + canned-response
 * factory tests. Pins:
 *
 *   1. canned cycle is deterministic — judge() returns
 *      verdicts[i % n_verdicts] and advances i.
 *   2. verdict struct round-trips unchanged through the deep copy.
 *   3. zero-length verdict array is rejected at create time.
 *   4. apple_fm factory returns HU_ERR_NOT_SUPPORTED until Task 7.
 *   5. gemini_nano factory returns HU_ERR_NOT_SUPPORTED until Task 8.
 *   6. ASan-clean deinit after a few judge() calls (no leaked rationale).
 */

static void test_eval_judge_canned_cycles_through_verdicts(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_judge_verdict_t cfg_verdicts[3] = {
        {.prefer_a = 1, .confidence = 0.90, .rationale = "A is better"},
        {.prefer_a = 0, .confidence = 0.75, .rationale = "B is better"},
        {.prefer_a = -1, .confidence = 0.50, .rationale = "tie"},
    };
    hu_eval_judge_canned_config_t cfg = {
        .verdicts = cfg_verdicts,
        .n_verdicts = 3,
    };
    hu_eval_judge_external_t judge = {0};
    HU_ASSERT_EQ(hu_eval_judge_create_canned(&alloc, &cfg, &judge), HU_OK);
    HU_ASSERT_NOT_NULL(judge.vtable);
    HU_ASSERT_NOT_NULL(judge.ctx);
    HU_ASSERT_STR_EQ(judge.vtable->name(&judge), "canned");

    hu_eval_judge_pair_t pair = {
        .prompt = "what is 2+2?",
        .response_a = "four",
        .response_b = "five",
    };

    /* Five calls should yield indices 0,1,2,0,1 — deterministic cycle. */
    int expected_prefer_a[5] = {1, 0, -1, 1, 0};
    double expected_conf[5] = {0.90, 0.75, 0.50, 0.90, 0.75};
    for (int i = 0; i < 5; i++) {
        hu_eval_judge_verdict_t v = {0};
        HU_ASSERT_EQ(judge.vtable->judge(&judge, &pair, &v), HU_OK);
        HU_ASSERT_EQ(v.prefer_a, expected_prefer_a[i]);
        HU_ASSERT_TRUE(fabs(v.confidence - expected_conf[i]) < 1e-9);
    }

    judge.vtable->deinit(&judge);
    HU_ASSERT_NULL(judge.vtable);
    HU_ASSERT_NULL(judge.ctx);
}

static void test_eval_judge_canned_returns_verdict_struct_unchanged(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_judge_verdict_t cfg_verdicts[1] = {
        {.prefer_a = 1, .confidence = 0.8, .rationale = "candidate A is clearly stronger"},
    };
    hu_eval_judge_canned_config_t cfg = {
        .verdicts = cfg_verdicts,
        .n_verdicts = 1,
    };
    hu_eval_judge_external_t judge = {0};
    HU_ASSERT_EQ(hu_eval_judge_create_canned(&alloc, &cfg, &judge), HU_OK);

    hu_eval_judge_pair_t pair = {
        .prompt = "explain recursion",
        .response_a = "a function that calls itself",
        .response_b = "loops do it better",
    };
    hu_eval_judge_verdict_t v = {0};
    HU_ASSERT_EQ(judge.vtable->judge(&judge, &pair, &v), HU_OK);
    HU_ASSERT_EQ(v.prefer_a, 1);
    HU_ASSERT_TRUE(fabs(v.confidence - 0.8) < 1e-9);
    HU_ASSERT_NOT_NULL(v.rationale);
    HU_ASSERT_STR_EQ(v.rationale, "candidate A is clearly stronger");

    judge.vtable->deinit(&judge);
}

static void test_eval_judge_canned_handles_zero_verdicts_gracefully(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_judge_verdict_t cfg_verdicts[1] = {
        {.prefer_a = 0, .confidence = 0.5, .rationale = NULL},
    };
    hu_eval_judge_canned_config_t cfg = {
        .verdicts = cfg_verdicts, /* non-NULL */
        .n_verdicts = 0,          /* but empty */
    };
    hu_eval_judge_external_t judge = {0};
    HU_ASSERT_EQ(hu_eval_judge_create_canned(&alloc, &cfg, &judge),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(judge.vtable);
    HU_ASSERT_NULL(judge.ctx);
}

#if !defined(HU_EVAL_JUDGE_HAVE_APPLE_FM_IMPL)
static void test_eval_judge_apple_fm_returns_not_supported_until_task_7(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_judge_external_t judge = {0};
    const char *reason = NULL;
    HU_ASSERT_EQ(hu_eval_judge_create_apple_fm(&alloc, &judge, &reason), HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NOT_NULL(reason);
    HU_ASSERT_STR_CONTAINS(reason, "unavailable");
    HU_ASSERT_NULL(judge.vtable);
    HU_ASSERT_NULL(judge.ctx);
}

static void test_eval_judge_apple_fm_accepts_null_out_reason(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_judge_external_t judge = {0};
    HU_ASSERT_EQ(hu_eval_judge_create_apple_fm(&alloc, &judge, NULL), HU_ERR_NOT_SUPPORTED);
}
#endif

#if !defined(HU_EVAL_JUDGE_HAVE_GEMINI_NANO_IMPL)
static void test_eval_judge_gemini_nano_returns_not_supported_until_task_8(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_judge_external_t judge = {0};
    const char *reason = NULL;
    HU_ASSERT_EQ(hu_eval_judge_create_gemini_nano(&alloc, &judge, &reason), HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NOT_NULL(reason);
    HU_ASSERT_STR_CONTAINS(reason, "unavailable");
    HU_ASSERT_NULL(judge.vtable);
    HU_ASSERT_NULL(judge.ctx);
}
#endif

static void test_eval_judge_canned_deinit_does_not_leak(void) {
    /* Mix NULL and non-NULL rationales to exercise both branches in
     * the deep-copy + cleanup paths under ASan. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_judge_verdict_t cfg_verdicts[3] = {
        {.prefer_a = 1, .confidence = 0.7, .rationale = "first"},
        {.prefer_a = 0, .confidence = 0.6, .rationale = NULL},
        {.prefer_a = -1, .confidence = 0.5, .rationale = "third with a longer rationale string"},
    };
    hu_eval_judge_canned_config_t cfg = {
        .verdicts = cfg_verdicts,
        .n_verdicts = 3,
    };
    hu_eval_judge_external_t judge = {0};
    HU_ASSERT_EQ(hu_eval_judge_create_canned(&alloc, &cfg, &judge), HU_OK);

    hu_eval_judge_pair_t pair = {
        .prompt = "p",
        .response_a = "a",
        .response_b = "b",
    };
    for (int i = 0; i < 4; i++) {
        hu_eval_judge_verdict_t v = {0};
        HU_ASSERT_EQ(judge.vtable->judge(&judge, &pair, &v), HU_OK);
    }

    judge.vtable->deinit(&judge);
    /* No leaks ⇒ ASan clean (run_eval_judge_external_tests is invoked from
     * test_main.c which links with -fsanitize=address). */
}

void run_eval_judge_external_tests(void) {
    HU_TEST_SUITE("eval_judge_external");
    HU_RUN_TEST(test_eval_judge_canned_cycles_through_verdicts);
    HU_RUN_TEST(test_eval_judge_canned_returns_verdict_struct_unchanged);
    HU_RUN_TEST(test_eval_judge_canned_handles_zero_verdicts_gracefully);
#if !defined(HU_EVAL_JUDGE_HAVE_APPLE_FM_IMPL)
    HU_RUN_TEST(test_eval_judge_apple_fm_returns_not_supported_until_task_7);
    HU_RUN_TEST(test_eval_judge_apple_fm_accepts_null_out_reason);
#endif
#if !defined(HU_EVAL_JUDGE_HAVE_GEMINI_NANO_IMPL)
    HU_RUN_TEST(test_eval_judge_gemini_nano_returns_not_supported_until_task_8);
#endif
    HU_RUN_TEST(test_eval_judge_canned_deinit_does_not_leak);
}
