#include "human/core/allocator.h"
#include "human/eval/eval_gate.h"
#include "test_framework.h"

#include <math.h>
#include <string.h>

static void test_eval_gate_accepts_when_all_criteria_pass(void) {
    hu_eval_gate_t gate = {
        .baseline_persona_fidelity_mean = 0.60,
        .baseline_mt_bench_mean = 5.0,
        .baseline_ifeval_mean = 0.50,
        .baseline_p95_latency_ms = 200.0,
        .persona_delta_min = 0.05,
        .mt_bench_regression_max = -0.01,
        .ifeval_regression_max = -0.02,
        .latency_delta_max_ms = 50.0,
        .bootstrap_samples = 1000,
        .bootstrap_seed = 42,
        .mt_bench = (void *)1,
        .ifeval = (void *)1,
        .reward_model = (void *)1,
    };
    double persona[20], mt[20], ifeval[20], reward[20];
    for (int i = 0; i < 20; i++) {
        persona[i] = 0.75 + 0.01 * (double)(i % 3 - 1);
        mt[i] = 5.0 + 0.02 * (double)(i % 3 - 1);
        ifeval[i] = 0.55 + 0.01 * (double)(i % 3 - 1);
        reward[i] = 0.20 + 0.01 * (double)(i % 3 - 1);
    }
    hu_eval_gate_verdict_t verdict = {0};
    HU_ASSERT_EQ(hu_eval_gate_decide_from_arrays_for_test(&gate, persona, mt, ifeval, reward, 20,
                                                         180.0, &verdict),
                 HU_OK);
    HU_ASSERT_TRUE(verdict.promote);
    HU_ASSERT_TRUE(verdict.persona_pass);
    HU_ASSERT_TRUE(verdict.mt_bench_pass);
    HU_ASSERT_TRUE(verdict.ifeval_pass);
    HU_ASSERT_TRUE(verdict.latency_pass);
    HU_ASSERT_TRUE(verdict.reward_pass);
}

static void test_eval_gate_rejects_when_persona_delta_below_threshold(void) {
    hu_eval_gate_t gate = {
        .baseline_persona_fidelity_mean = 0.70,
        .baseline_mt_bench_mean = 5.0,
        .baseline_ifeval_mean = 0.50,
        .baseline_p95_latency_ms = 200.0,
        .persona_delta_min = 0.05,
        .mt_bench_regression_max = -0.01,
        .ifeval_regression_max = -0.02,
        .latency_delta_max_ms = 50.0,
        .bootstrap_samples = 1000,
        .bootstrap_seed = 42,
    };
    double persona[20], mt[20], ifeval[20], reward[20];
    for (int i = 0; i < 20; i++) {
        persona[i] = 0.72;
        mt[i] = 5.0;
        ifeval[i] = 0.55;
        reward[i] = 0.20;
    }
    hu_eval_gate_verdict_t verdict = {0};
    HU_ASSERT_EQ(hu_eval_gate_decide_from_arrays_for_test(&gate, persona, mt, ifeval, reward, 20,
                                                         180.0, &verdict),
                 HU_OK);
    HU_ASSERT_FALSE(verdict.promote);
    HU_ASSERT_FALSE(verdict.persona_pass);
    HU_ASSERT_TRUE(strstr(verdict.reason, "persona") != NULL);
}

static void test_eval_gate_rejects_on_latency_regression(void) {
    hu_eval_gate_t gate = {
        .baseline_persona_fidelity_mean = 0.60,
        .baseline_mt_bench_mean = 5.0,
        .baseline_ifeval_mean = 0.50,
        .baseline_p95_latency_ms = 200.0,
        .persona_delta_min = 0.05,
        .mt_bench_regression_max = -0.01,
        .ifeval_regression_max = -0.02,
        .latency_delta_max_ms = 50.0,
        .bootstrap_samples = 1000,
        .bootstrap_seed = 42,
    };
    double persona[20], mt[20], ifeval[20], reward[20];
    for (int i = 0; i < 20; i++) {
        persona[i] = 0.75;
        mt[i] = 5.0;
        ifeval[i] = 0.55;
        reward[i] = 0.20;
    }
    hu_eval_gate_verdict_t verdict = {0};
    HU_ASSERT_EQ(hu_eval_gate_decide_from_arrays_for_test(&gate, persona, mt, ifeval, reward, 20,
                                                         300.0, &verdict),
                 HU_OK);
    HU_ASSERT_FALSE(verdict.promote);
    HU_ASSERT_FALSE(verdict.latency_pass);
    HU_ASSERT_TRUE(strstr(verdict.reason, "latency") != NULL);
}

static void test_eval_gate_rejects_when_reward_ci_lower_below_zero(void) {
    hu_eval_gate_t gate = {
        .baseline_persona_fidelity_mean = 0.60,
        .baseline_mt_bench_mean = 5.0,
        .baseline_ifeval_mean = 0.50,
        .baseline_p95_latency_ms = 200.0,
        .persona_delta_min = 0.05,
        .mt_bench_regression_max = -0.01,
        .ifeval_regression_max = -0.02,
        .latency_delta_max_ms = 50.0,
        .bootstrap_samples = 1000,
        .bootstrap_seed = 42,
        .reward_model = (void *)1,
    };
    double persona[20], mt[20], ifeval[20], reward[20];
    for (int i = 0; i < 20; i++) {
        persona[i] = 0.75;
        mt[i] = 5.0;
        ifeval[i] = 0.55;
        reward[i] = -0.30 + 0.01 * (double)(i % 3 - 1);
    }
    hu_eval_gate_verdict_t verdict = {0};
    HU_ASSERT_EQ(hu_eval_gate_decide_from_arrays_for_test(&gate, persona, mt, ifeval, reward, 20,
                                                         180.0, &verdict),
                 HU_OK);
    HU_ASSERT_FALSE(verdict.promote);
    HU_ASSERT_FALSE(verdict.reward_pass);
    HU_ASSERT_TRUE(strstr(verdict.reason, "reward") != NULL);
}

static void test_eval_gate_rejects_when_n_below_floor_for_test(void) {
    hu_eval_gate_t gate = {
        .baseline_persona_fidelity_mean = 0.60,
        .baseline_mt_bench_mean = 5.0,
        .baseline_ifeval_mean = 0.50,
        .baseline_p95_latency_ms = 200.0,
        .persona_delta_min = 0.05,
        .mt_bench_regression_max = -0.01,
        .ifeval_regression_max = -0.02,
        .latency_delta_max_ms = 50.0,
        .bootstrap_samples = 1000,
        .bootstrap_seed = 42,
    };
    double one[1] = {0.75};
    hu_eval_gate_verdict_t verdict = {0};
    HU_ASSERT_EQ(hu_eval_gate_decide_from_arrays_for_test(&gate, one, one, one, one, 1, 180.0,
                                                         &verdict),
                 HU_ERR_INVALID_ARGUMENT);
    double ten[10];
    for (int i = 0; i < 10; i++) ten[i] = 0.75;
    HU_ASSERT_EQ(hu_eval_gate_decide_from_arrays_for_test(&gate, ten, ten, ten, ten, 10, 180.0,
                                                         &verdict),
                 HU_OK);
}

void run_eval_gate_tests(void) {
    HU_TEST_SUITE("eval-gate");
    HU_RUN_TEST(test_eval_gate_accepts_when_all_criteria_pass);
    HU_RUN_TEST(test_eval_gate_rejects_when_persona_delta_below_threshold);
    HU_RUN_TEST(test_eval_gate_rejects_on_latency_regression);
    HU_RUN_TEST(test_eval_gate_rejects_when_reward_ci_lower_below_zero);
    HU_RUN_TEST(test_eval_gate_rejects_when_n_below_floor_for_test);
}
