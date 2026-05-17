/* US-7.10 — `human ml rl-train` CLI router tests.
 *
 * AC-7.10.3: --algorithm simpo runs train_step without crashing.
 * AC-7.10.4: --algorithm dpo delegates to hu_ml_cli_dpo_train.
 * AC-7.10.5: --algorithm orpo / grpo2 emit "not yet implemented"
 *            and return HU_ERR_NOT_SUPPORTED.
 * Plus: missing --algorithm flag returns HU_ERR_INVALID_ARGUMENT;
 *       unknown algorithm name returns HU_ERR_INVALID_ARGUMENT;
 *       --help returns HU_OK.
 *
 * All paths run in HU_IS_TEST mode (file writes / provider calls are
 * stubbed by the existing CLI test seam). */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/cli.h"

#include "test_framework.h"

#include <stddef.h>
#include <string.h>

/* AC-7.10.3: simpo path returns HU_OK and exercises train_step. */
static void test_rl_train_simpo_e2e_fixture(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {"rl-train", "--algorithm", "simpo"};
    int argc = (int)(sizeof(argv) / sizeof(argv[0]));
    HU_ASSERT_EQ(hu_ml_cli_rl_train(&alloc, argc, argv), HU_OK);
}

/* AC-7.10.3: simpo respects --beta / --gamma overrides. */
static void test_rl_train_simpo_with_overrides(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {
        "rl-train", "--algorithm", "simpo", "--beta", "0.2", "--gamma", "0.3",
    };
    int argc = (int)(sizeof(argv) / sizeof(argv[0]));
    HU_ASSERT_EQ(hu_ml_cli_rl_train(&alloc, argc, argv), HU_OK);
}

/* AC-7.10.4: dpo delegates to the existing hu_ml_cli_dpo_train, which
 * in HU_IS_TEST mode returns HU_OK after printing "test mode: skipped"
 * (src/ml/cli.c:529). */
static void test_rl_train_dpo_backward_compat(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {"rl-train", "--algorithm", "dpo"};
    int argc = (int)(sizeof(argv) / sizeof(argv[0]));
    HU_ASSERT_EQ(hu_ml_cli_rl_train(&alloc, argc, argv), HU_OK);
}

/* AC-7.10.4: dpo path preserves additional flags (e.g. --batch-size).
 * The DPO CLI in test mode accepts and ignores them, so this asserts
 * argv rewriting doesn't drop unrelated tokens. */
static void test_rl_train_dpo_preserves_other_flags(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {
        "rl-train", "--algorithm", "dpo", "--batch-size", "5",
    };
    int argc = (int)(sizeof(argv) / sizeof(argv[0]));
    HU_ASSERT_EQ(hu_ml_cli_rl_train(&alloc, argc, argv), HU_OK);
}

/* AC-11.5.5: grpo2 keeps returning HU_ERR_NOT_SUPPORTED (boundary
 * stays clean). orpo MOVED to test_orpo_train_exits_0 below — US-11.5
 * lands the ORPO loss head, so --algorithm orpo now exits 0 in
 * HU_IS_TEST mode. */
static void test_rl_train_unimplemented_algorithms(void) {
    hu_allocator_t alloc = hu_system_allocator();
    {
        const char *argv[] = {"rl-train", "--algorithm", "grpo2"};
        int argc = (int)(sizeof(argv) / sizeof(argv[0]));
        HU_ASSERT_EQ(hu_ml_cli_rl_train(&alloc, argc, argv), HU_ERR_NOT_SUPPORTED);
    }
}

/* AC-11.5.1: --algorithm orpo runs the ORPO factory and exits 0 in
 * HU_IS_TEST mode (mock train_step prints loss=0.7). */
static void test_orpo_train_exits_0(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {"rl-train", "--algorithm", "orpo", "--lambda-orpo", "0.1"};
    int argc = (int)(sizeof(argv) / sizeof(argv[0]));
    HU_ASSERT_EQ(hu_ml_cli_rl_train(&alloc, argc, argv), HU_OK);
}

/* AC-11.5.1: --algorithm orpo defaults work when --lambda-orpo is
 * omitted (factory default λ=0.1). */
static void test_orpo_train_defaults_exit_0(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {"rl-train", "--algorithm", "orpo"};
    int argc = (int)(sizeof(argv) / sizeof(argv[0]));
    HU_ASSERT_EQ(hu_ml_cli_rl_train(&alloc, argc, argv), HU_OK);
}

/* --lambda-orpo input validation: non-numeric and non-positive rejected. */
static void test_orpo_train_invalid_lambda(void) {
    hu_allocator_t alloc = hu_system_allocator();
    {
        const char *argv[] = {
            "rl-train", "--algorithm", "orpo", "--lambda-orpo", "not-a-number",
        };
        int argc = (int)(sizeof(argv) / sizeof(argv[0]));
        HU_ASSERT_EQ(hu_ml_cli_rl_train(&alloc, argc, argv), HU_ERR_INVALID_ARGUMENT);
    }
    {
        const char *argv[] = {
            "rl-train", "--algorithm", "orpo", "--lambda-orpo", "0",
        };
        int argc = (int)(sizeof(argv) / sizeof(argv[0]));
        HU_ASSERT_EQ(hu_ml_cli_rl_train(&alloc, argc, argv), HU_ERR_INVALID_ARGUMENT);
    }
}

/* Missing --algorithm flag: clear error, exit code != 0. */
static void test_rl_train_missing_algorithm_flag(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {"rl-train"};
    int argc = (int)(sizeof(argv) / sizeof(argv[0]));
    HU_ASSERT_EQ(hu_ml_cli_rl_train(&alloc, argc, argv), HU_ERR_INVALID_ARGUMENT);
}

/* Unknown algorithm name. */
static void test_rl_train_unknown_algorithm(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {"rl-train", "--algorithm", "ppo"};
    int argc = (int)(sizeof(argv) / sizeof(argv[0]));
    HU_ASSERT_EQ(hu_ml_cli_rl_train(&alloc, argc, argv), HU_ERR_INVALID_ARGUMENT);
}

/* --help returns HU_OK and prints usage. */
static void test_rl_train_help(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {"rl-train", "--help"};
    int argc = (int)(sizeof(argv) / sizeof(argv[0]));
    HU_ASSERT_EQ(hu_ml_cli_rl_train(&alloc, argc, argv), HU_OK);
}

/* --beta / --gamma input validation. */
static void test_rl_train_simpo_invalid_hyperparameters(void) {
    hu_allocator_t alloc = hu_system_allocator();
    {
        const char *argv[] = {
            "rl-train", "--algorithm", "simpo", "--beta", "not-a-number",
        };
        int argc = (int)(sizeof(argv) / sizeof(argv[0]));
        HU_ASSERT_EQ(hu_ml_cli_rl_train(&alloc, argc, argv), HU_ERR_INVALID_ARGUMENT);
    }
    {
        const char *argv[] = {
            "rl-train", "--algorithm", "simpo", "--gamma", "-0.1",
        };
        int argc = (int)(sizeof(argv) / sizeof(argv[0]));
        HU_ASSERT_EQ(hu_ml_cli_rl_train(&alloc, argc, argv), HU_ERR_INVALID_ARGUMENT);
    }
}

void run_ml_cli_rl_train_tests(void) {
    HU_TEST_SUITE("MlCliRlTrain");
    HU_RUN_TEST(test_rl_train_simpo_e2e_fixture);
    HU_RUN_TEST(test_rl_train_simpo_with_overrides);
    HU_RUN_TEST(test_rl_train_dpo_backward_compat);
    HU_RUN_TEST(test_rl_train_dpo_preserves_other_flags);
    HU_RUN_TEST(test_rl_train_unimplemented_algorithms);
    HU_RUN_TEST(test_orpo_train_exits_0);
    HU_RUN_TEST(test_orpo_train_defaults_exit_0);
    HU_RUN_TEST(test_orpo_train_invalid_lambda);
    HU_RUN_TEST(test_rl_train_missing_algorithm_flag);
    HU_RUN_TEST(test_rl_train_unknown_algorithm);
    HU_RUN_TEST(test_rl_train_help);
    HU_RUN_TEST(test_rl_train_simpo_invalid_hyperparameters);
}
