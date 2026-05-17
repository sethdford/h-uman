#include "test_framework.h"
#include "human/eval/competitive_harness.h"
#include "human/eval/eval_judge_external.h"
#include "human/core/allocator.h"
#include <stdio.h>
#include <string.h>

static void test_harness_renders_scorecard_with_unavailable_columns_honestly(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_competitive_harness_config_t cfg = {
        .out_markdown = "/tmp/scorecard.md",
        .out_json = "/tmp/scorecard.json",
        .min_available = 1,
    };
    hu_eval_judge_external_t stock = {0}, apple = {0};
    hu_eval_judge_verdict_t v = {.prefer_a = 1, .confidence = 0.9, .rationale = NULL};
    hu_eval_judge_canned_config_t canned = {.verdicts = &v, .n_verdicts = 1};
    HU_ASSERT_EQ(hu_eval_judge_create_canned(&alloc, &canned, &stock), HU_OK);
    HU_ASSERT_EQ(hu_eval_judge_create_apple_fm(&alloc, &apple, NULL), HU_OK);

    hu_competitive_harness_judge_slot_t judges[] = {
        {.column_name = "stock", .judge = stock, .available = true},
        {.column_name = "apple_fm", .judge = apple, .available = true},
        {.column_name = "gemini_nano", .available = false,
         .unavailable_reason = "unavailable (no chrome)"},
    };
    hu_competitive_harness_result_t res = {0};
    HU_ASSERT_EQ(hu_competitive_harness_run_with_test_judges(&alloc, &cfg, judges, 3, &res),
                 HU_OK);

    char buf[16384];
    FILE *f = fopen("/tmp/scorecard.md", "r");
    HU_ASSERT_NOT_NULL(f);
    size_t r = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[r] = '\0';
    HU_ASSERT_TRUE(strstr(buf, "stock") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "apple_fm") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "gemini_nano") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "unavailable") != NULL);

    stock.vtable->deinit(&stock);
    apple.vtable->deinit(&apple);
}

void run_competitive_harness_tests(void) {
    HU_TEST_SUITE("competitive-harness");
    HU_RUN_TEST(test_harness_renders_scorecard_with_unavailable_columns_honestly);
}
