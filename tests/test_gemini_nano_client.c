#include "test_framework.h"
#include "human/eval/eval_judge_external.h"
#include "human/core/allocator.h"

static void test_gemini_nano_create_returns_canned_judge_in_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_judge_external_t judge = {0};
    HU_ASSERT_EQ(hu_eval_judge_create_gemini_nano(&alloc, &judge), HU_OK);
    HU_ASSERT_STR_EQ(judge.vtable->name(&judge), "canned");
    judge.vtable->deinit(&judge);
}

void run_gemini_nano_client_tests(void) {
    HU_TEST_SUITE("gemini-nano-client");
    HU_RUN_TEST(test_gemini_nano_create_returns_canned_judge_in_test_mode);
}
