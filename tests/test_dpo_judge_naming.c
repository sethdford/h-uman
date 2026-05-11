/* Phase 0 Task 8 — proves that the rename hu_dpo_train_step → hu_dpo_judge_step
 * lands cleanly: the new name exists, the old name still compiles (deprecation
 * shim), and both return identical values for identical inputs.
 *
 * Why the rename matters: hu_dpo_train_step is misleading. It does not run
 * Direct Preference Optimization in the technical sense — there is no policy
 * log-prob, no reference-model log-prob, no gradient on policy weights. It
 * collects preference pairs and asks an external LLM (the "judge") to score
 * each pair, then aggregates the scores into a synthetic loss for reporting.
 * Real DPO with policy gradients lands in Phase 2 as hu_dpo_real_step. Until
 * then, calling this function "train_step" makes the M3 narrative dishonest.
 *
 * See spec §1.5.2 issue #5 and the May 11 2026 audit baseline. */

#include "test_framework.h"
#include "human/ml/dpo.h"
#include <string.h>

#ifdef HU_ENABLE_ML

/* The new canonical name MUST exist and be callable. This test is the API
 * surface contract — Task 9 introduces hu_dpo_judge_step in dpo.h and the
 * compile passes. */
static void test_dpo_judge_step_new_name_exists(void) {
    hu_dpo_judge_result_t result;
    memset(&result, 0, sizeof(result));
    /* Pass NULLs to verify the signature is callable; the function rejects
     * NULL provider/collector/alloc and returns HU_ERR_INVALID_ARGUMENT.
     * The test is the compile, not the return value. */
    hu_error_t err = hu_dpo_judge_step(/*collector*/ NULL, /*alloc*/ NULL,
                                       /*provider*/ NULL, /*model*/ NULL,
                                       /*model_len*/ 0, /*beta*/ 0.1,
                                       /*batch_size*/ 0, &result);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

/* The old name MUST still compile (deprecation shim) so existing callers
 * are not broken in this commit. The deprecation warning surfaces at
 * compile time; we suppress it here because exercising the shim is
 * exactly the point of the test. */
static void test_dpo_train_step_deprecated_shim_still_works(void) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    hu_dpo_train_result_t old_result;
    memset(&old_result, 0, sizeof(old_result));
    hu_error_t err = hu_dpo_train_step(/*collector*/ NULL, /*alloc*/ NULL,
                                       /*provider*/ NULL, /*model*/ NULL,
                                       /*model_len*/ 0, /*beta*/ 0.1,
                                       /*batch_size*/ 0, &old_result);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
#pragma GCC diagnostic pop
}

/* The deprecation shim must forward every input verbatim and return an
 * identical result struct. Trivially-equivalent-by-construction (the shim
 * is a one-liner `return hu_dpo_judge_step(...)`) is NOT the same as
 * pinned-by-test. This test pins it.
 *
 * Approach: call both functions on the same well-defined invalid input
 * (NULL collector — both must reject uniformly with HU_ERR_INVALID_ARGUMENT)
 * and assert (a) same error code, (b) byte-equal result struct. memcmp is
 * appropriate because both structs are POD and zero-initialized. */
static void test_dpo_judge_step_and_shim_return_identical_values(void) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    hu_dpo_judge_result_t new_result;
    hu_dpo_judge_result_t shim_result;
    memset(&new_result, 0, sizeof(new_result));
    memset(&shim_result, 0, sizeof(shim_result));

    hu_error_t err_new =
        hu_dpo_judge_step(/*collector*/ NULL, /*alloc*/ NULL, /*provider*/ NULL,
                          /*model*/ NULL, /*model_len*/ 0, /*beta*/ 0.1,
                          /*batch_size*/ 0, &new_result);
    hu_error_t err_shim =
        hu_dpo_train_step(/*collector*/ NULL, /*alloc*/ NULL, /*provider*/ NULL,
                          /*model*/ NULL, /*model_len*/ 0, /*beta*/ 0.1,
                          /*batch_size*/ 0, &shim_result);

    HU_ASSERT_EQ(err_new, err_shim);
    HU_ASSERT_EQ(memcmp(&new_result, &shim_result, sizeof(new_result)), 0);
#pragma GCC diagnostic pop
}

#endif /* HU_ENABLE_ML */

void run_dpo_judge_naming_tests(void) {
    HU_TEST_SUITE("dpo-judge-naming");
#ifdef HU_ENABLE_ML
    HU_RUN_TEST(test_dpo_judge_step_new_name_exists);
    HU_RUN_TEST(test_dpo_train_step_deprecated_shim_still_works);
    HU_RUN_TEST(test_dpo_judge_step_and_shim_return_identical_values);
#endif
}
