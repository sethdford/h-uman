#include "test_framework.h"
#include "human/eval/eval_judge_external.h"
#include "human/core/allocator.h"
#include <string.h>

static void test_apple_fm_create_returns_canned_judge_in_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_judge_external_t judge = {0};
    HU_ASSERT_EQ(hu_eval_judge_create_apple_fm(&alloc, &judge, NULL), HU_OK);
    HU_ASSERT_STR_EQ(judge.vtable->name(&judge), "canned");

    hu_eval_judge_pair_t pair = {
        .prompt = "what is two plus two?",
        .response_a = "four",
        .response_b = "five",
    };
    hu_eval_judge_verdict_t v = {0};
    HU_ASSERT_EQ(judge.vtable->judge(&judge, &pair, &v), HU_OK);
    HU_ASSERT_EQ(v.prefer_a, 1);
    judge.vtable->deinit(&judge);
}

void run_apple_fm_client_tests(void) {
    HU_TEST_SUITE("apple-fm-client");
    HU_RUN_TEST(test_apple_fm_create_returns_canned_judge_in_test_mode);
}
