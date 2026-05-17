#include "test_framework.h"
#include "human/eval/eval_gate.h"
#include "human/ml/cli.h"
#include "human/core/allocator.h"

static int g_gate_decide_called = 0;

static void test_lora_ab_with_require_positive_calls_hu_eval_gate_decide(void) {
    g_gate_decide_called = 0;
    hu_eval_gate_set_decide_spy_for_test(&g_gate_decide_called);
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {
        "lora-ab", "--persona", "test-inline", "--require-positive",
        "--before", "tests/fixtures/lora_ab_before.json",
        "--after", "tests/fixtures/lora_ab_after.json",
    };
    (void)hu_ml_cli_lora_ab(&alloc, 8, argv);
    HU_ASSERT_TRUE(g_gate_decide_called >= 1);
    hu_eval_gate_set_decide_spy_for_test(NULL);
}

static void test_lora_ab_without_require_positive_does_not_call_gate(void) {
    g_gate_decide_called = 0;
    hu_eval_gate_set_decide_spy_for_test(&g_gate_decide_called);
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {
        "lora-ab", "--persona", "test-inline",
        "--before", "tests/fixtures/lora_ab_before.json",
        "--after", "tests/fixtures/lora_ab_after.json",
    };
    (void)hu_ml_cli_lora_ab(&alloc, 7, argv);
    HU_ASSERT_EQ(g_gate_decide_called, 0);
    hu_eval_gate_set_decide_spy_for_test(NULL);
}

void run_lora_ab_require_positive_tests(void) {
    HU_TEST_SUITE("lora-ab-require-positive");
    HU_RUN_TEST(test_lora_ab_with_require_positive_calls_hu_eval_gate_decide);
    HU_RUN_TEST(test_lora_ab_without_require_positive_does_not_call_gate);
}
