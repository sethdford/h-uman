#include "test_framework.h"
#include "human/eval/stock_baseline.h"
#include "human/provider_test_seam.h"
#include "human/core/allocator.h"
#include <string.h>

static void test_stock_baseline_unloads_adapter_before_generate(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t *provider = NULL;
    HU_ASSERT_EQ(hu_provider_create_for_test_with_canned_response(
                     &alloc, "canned: four", &provider),
                 HU_OK);
    hu_eval_judge_external_t judge = {0};
    HU_ASSERT_EQ(hu_stock_baseline_create(&alloc, provider, &judge), HU_OK);

    hu_eval_judge_pair_t pair = {
        .prompt = "what is two plus two?",
        .response_a = "four",
        .response_b = "five",
    };
    hu_eval_judge_verdict_t verdict = {0};
    HU_ASSERT_EQ(judge.vtable->judge(&judge, &pair, &verdict), HU_OK);
    HU_ASSERT_TRUE(hu_provider_unload_called_count_for_test(provider) >= 1);

    judge.vtable->deinit(&judge);
    hu_provider_destroy_for_test(provider, &alloc);
}

void run_stock_baseline_tests(void) {
    HU_TEST_SUITE("stock-baseline");
    HU_RUN_TEST(test_stock_baseline_unloads_adapter_before_generate);
}
