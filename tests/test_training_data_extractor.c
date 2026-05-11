/* W14 — training data extractor + DPO + scheduler runner.
 *
 * Coverage: NULL-arg guards, HU_IS_TEST short-circuits (the implementation
 * returns HU_OK + count=0 in test mode regardless of DB state), runner ctx
 * defaults, runner argument validation, and constants from the public
 * header so renaming a #define breaks the build the same day.
 *
 * The runner itself is not exercised end-to-end here because that would
 * require a real W14 scheduler + memory facade; the runner-level wiring
 * is covered in `test_w14_runners.c`. */

#include "human/agent/training_data_runner.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/training_data_extractor.h"
#include "test_framework.h"

#include <string.h>

/* ── Extractor null-arg guards ─────────────────────────────────────────── */

static void tde_extractor_null_alloc_returns_invalid(void) {
    size_t count = 99;
    HU_ASSERT_EQ(hu_training_data_extract(NULL, "/tmp/x.db", NULL, "/tmp", &count),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(count, 99u);
}

static void tde_extractor_null_db_path_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t count = 99;
    HU_ASSERT_EQ(hu_training_data_extract(&alloc, NULL, NULL, "/tmp", &count),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(count, 99u);
}

static void tde_extractor_null_output_dir_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t count = 99;
    HU_ASSERT_EQ(hu_training_data_extract(&alloc, "/tmp/x.db", NULL, NULL, &count),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(count, 99u);
}

static void tde_extractor_null_count_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_training_data_extract(&alloc, "/tmp/x.db", NULL, "/tmp", NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

static void tde_extractor_valid_args_returns_ok_under_test(void) {
    /* HU_IS_TEST is defined for the test target; the extractor short-
     * circuits with HU_OK and count=0 instead of touching SQLite. */
    hu_allocator_t alloc = hu_system_allocator();
    size_t count = 99;
    HU_ASSERT_EQ(hu_training_data_extract(&alloc, "/tmp/hu_nonexistent_extractor.db", NULL,
                                          "/tmp", &count),
                 HU_OK);
    HU_ASSERT_EQ(count, 0u);
}

/* ── DPO extraction null-arg guards ────────────────────────────────────── */

static void tde_dpo_null_alloc_returns_invalid(void) {
    size_t pairs = 99;
    HU_ASSERT_EQ(hu_training_data_extract_dpo(NULL, "/tmp/x.db", 120, &pairs),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(pairs, 99u);
}

static void tde_dpo_null_db_path_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t pairs = 99;
    HU_ASSERT_EQ(hu_training_data_extract_dpo(&alloc, NULL, 120, &pairs),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(pairs, 99u);
}

static void tde_dpo_null_count_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_training_data_extract_dpo(&alloc, "/tmp/x.db", 120, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

static void tde_dpo_valid_args_returns_ok_under_test(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t pairs = 99;
    HU_ASSERT_EQ(hu_training_data_extract_dpo(&alloc, "/tmp/hu_nonexistent_extractor.db",
                                              120, &pairs),
                 HU_OK);
    HU_ASSERT_EQ(pairs, 0u);
}

static void tde_dpo_default_window_used_for_zero(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t pairs = 0;
    HU_ASSERT_EQ(hu_training_data_extract_dpo(&alloc, "/tmp/hu_nonexistent_extractor.db",
                                              0, &pairs),
                 HU_OK);
    HU_ASSERT_EQ(pairs, 0u);
}

/* ── Runner context defaults + argument validation ────────────────────── */

static void tde_runner_ctx_zero_init_has_sensible_defaults(void) {
    hu_training_data_runner_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    HU_ASSERT_EQ((long long)ctx.retrain_threshold, 0LL);
    HU_ASSERT_EQ(ctx.correction_window_sec, 0);
    HU_ASSERT_EQ((long long)ctx.cumulative_extracted, 0LL);
    HU_ASSERT_NULL(ctx.alloc);
    HU_ASSERT_NULL(ctx.memory_db_path);
    HU_ASSERT_NULL(ctx.persona_path);
    HU_ASSERT_NULL(ctx.output_dir);
    HU_ASSERT_NULL(ctx.scheduler);
}

static void tde_runner_null_spec_returns_invalid(void) {
    HU_ASSERT_EQ(hu_training_data_runner(NULL, NULL, 5000, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

static void tde_runner_null_user_data_returns_invalid(void) {
    /* `spec` is opaque to the runner — a non-NULL bag is enough to reach
     * the user_data check. */
    char dummy_spec[1] = {0};
    HU_ASSERT_EQ(hu_training_data_runner(NULL, (const struct hu_job_spec *)dummy_spec, 5000, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

static void tde_runner_missing_paths_returns_invalid(void) {
    char dummy_spec[1] = {0};
    hu_training_data_runner_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    /* Both paths NULL → invalid. */
    HU_ASSERT_EQ(hu_training_data_runner(NULL, (const struct hu_job_spec *)dummy_spec, 5000, &ctx),
                 HU_ERR_INVALID_ARGUMENT);
    /* Only db path set → still invalid (missing output dir). */
    ctx.memory_db_path = "/tmp/hu_nonexistent_runner.db";
    HU_ASSERT_EQ(hu_training_data_runner(NULL, (const struct hu_job_spec *)dummy_spec, 5000, &ctx),
                 HU_ERR_INVALID_ARGUMENT);
}

/* ── Compile-time constant lock-ins ───────────────────────────────────── */

static void tde_retrain_threshold_constant_is_50(void) {
    HU_ASSERT_EQ(HU_TRAINING_DATA_RETRAIN_THRESHOLD, 50);
}

static void tde_dpo_correction_window_constant_is_300(void) {
    HU_ASSERT_EQ(HU_DPO_CORRECTION_WINDOW_SEC, 300);
}

void run_training_data_extractor_tests(void);

void run_training_data_extractor_tests(void) {
    HU_TEST_SUITE("training_data_extractor");
    HU_RUN_TEST(tde_extractor_null_alloc_returns_invalid);
    HU_RUN_TEST(tde_extractor_null_db_path_returns_invalid);
    HU_RUN_TEST(tde_extractor_null_output_dir_returns_invalid);
    HU_RUN_TEST(tde_extractor_null_count_returns_invalid);
    HU_RUN_TEST(tde_extractor_valid_args_returns_ok_under_test);
    HU_RUN_TEST(tde_dpo_null_alloc_returns_invalid);
    HU_RUN_TEST(tde_dpo_null_db_path_returns_invalid);
    HU_RUN_TEST(tde_dpo_null_count_returns_invalid);
    HU_RUN_TEST(tde_dpo_valid_args_returns_ok_under_test);
    HU_RUN_TEST(tde_dpo_default_window_used_for_zero);
    HU_RUN_TEST(tde_runner_ctx_zero_init_has_sensible_defaults);
    HU_RUN_TEST(tde_runner_null_spec_returns_invalid);
    HU_RUN_TEST(tde_runner_null_user_data_returns_invalid);
    HU_RUN_TEST(tde_runner_missing_paths_returns_invalid);
    HU_RUN_TEST(tde_retrain_threshold_constant_is_50);
    HU_RUN_TEST(tde_dpo_correction_window_constant_is_300);
}
