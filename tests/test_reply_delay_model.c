/* Exercises hu_reply_delay_from_model / hu_reply_delay_mode_from_env /
 * hu_reply_delay_shadow_log in src/daemon/daemon_reply_delay.c. Contract
 * C5, Part C. Writes a small fixture JSON matching the shape
 * scripts/fit_reply_delay_model.py produces. */
#include "human/daemon/reply_delay.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *FIXTURE_JSON =
    "{"
    "\"length_bucket_thresholds\": {\"lo_chars\": 40, \"hi_chars\": 160},"
    "\"freq_tercile_boundaries\": {\"lo_count\": 5, \"hi_count\": 20},"
    "\"cells\": {"
    "  \"h10_lshort_flow\": {\"n\": 12, \"quantiles\": "
    "     {\"p10\": 10, \"p25\": 20, \"p50\": 30, \"p75\": 60, \"p90\": 120}}"
    "},"
    "\"hour_len_marginals\": {"
    "  \"h10_lshort\": {\"n\": 20, \"quantiles\": "
    "     {\"p10\": 15, \"p25\": 25, \"p50\": 40, \"p75\": 70, \"p90\": 130}}"
    "},"
    "\"hour_marginals\": {"
    "  \"h10\": {\"n\": 50, \"quantiles\": "
    "     {\"p10\": 20, \"p25\": 40, \"p50\": 90, \"p75\": 200, \"p90\": 500}},"
    "  \"h23\": {\"n\": 8, \"quantiles\": "
    "     {\"p10\": 100, \"p25\": 200, \"p50\": 300, \"p75\": 400, \"p90\": 500}}"
    "},"
    "\"global\": {\"n\": 500, \"quantiles\": "
    "  {\"p10\": 30, \"p25\": 60, \"p50\": 150, \"p75\": 400, \"p90\": 900}}"
    "}";

/* Heap-allocated (not static/stack) — mkstemp requires a FRESH "XXXXXX"
 * template on every call, and the caller unlink()s + frees the path when
 * done with it. */
static char *write_fixture(void) {
    char *path = strdup("/tmp/hu_reply_delay_model_XXXXXX");
    HU_ASSERT_NOT_NULL(path);
    int fd = mkstemp(path);
    HU_ASSERT(fd >= 0);
    FILE *f = fdopen(fd, "w");
    HU_ASSERT_NOT_NULL(f);
    fputs(FIXTURE_JSON, f);
    fclose(f);
    return path;
}

static void test_mode_from_env_default_off(void) {
    unsetenv("HU_REPLY_DELAY_MODEL");
    HU_ASSERT_EQ((int)hu_reply_delay_mode_from_env(), (int)HU_REPLY_DELAY_MODE_OFF);
}

static void test_mode_from_env_recognizes_shadow_and_live(void) {
    setenv("HU_REPLY_DELAY_MODEL", "shadow", 1);
    HU_ASSERT_EQ((int)hu_reply_delay_mode_from_env(), (int)HU_REPLY_DELAY_MODE_SHADOW);
    setenv("HU_REPLY_DELAY_MODEL", "live", 1);
    HU_ASSERT_EQ((int)hu_reply_delay_mode_from_env(), (int)HU_REPLY_DELAY_MODE_LIVE);
    unsetenv("HU_REPLY_DELAY_MODEL");
}

static void test_mode_from_env_unrecognized_value_fails_closed(void) {
    setenv("HU_REPLY_DELAY_MODEL", "definitely-not-a-mode", 1);
    HU_ASSERT_EQ((int)hu_reply_delay_mode_from_env(), (int)HU_REPLY_DELAY_MODE_OFF);
    unsetenv("HU_REPLY_DELAY_MODEL");
}

static void test_from_model_missing_file_returns_negative(void) {
    int64_t d =
        hu_reply_delay_from_model("/tmp/hu_reply_delay_model_does_not_exist.json", 10, 5, 2.0, 42);
    HU_ASSERT_TRUE(d < 0);
}

static void test_from_model_null_path_returns_negative(void) {
    HU_ASSERT_TRUE(hu_reply_delay_from_model(NULL, 10, 5, 2.0, 42) < 0);
}

static void test_from_model_malformed_json_returns_negative(void) {
    char path[] = "/tmp/hu_reply_delay_model_bad_XXXXXX";
    int fd = mkstemp(path);
    HU_ASSERT(fd >= 0);
    FILE *f = fdopen(fd, "w");
    fputs("{not valid json", f);
    fclose(f);

    HU_ASSERT_TRUE(hu_reply_delay_from_model(path, 10, 5, 2.0, 42) < 0);
    unlink(path);
}

static void test_from_model_exact_cell_hit_within_bounds(void) {
    char *path = write_fixture();
    /* hour=10, incoming_len=5 (<40 -> "short"), contact_freq=2.0 (<=5 -> "low")
     * -> exact cell h10_lshort_flow, quantiles [10,20,30,60,120]. */
    for (uint32_t seed = 1; seed < 20; seed++) {
        int64_t d = hu_reply_delay_from_model(path, 10, 5, 2.0, seed);
        HU_ASSERT_TRUE(d >= 10 && d <= 120);
    }
    unlink(path);
    free(path);
}

static void test_from_model_falls_back_to_hour_len_marginal(void) {
    char *path = write_fixture();
    /* hour=10, incoming_len=5, contact_freq=50.0 (-> "high") — no
     * h10_lshort_fhigh cell exists, so this must fall back to
     * h10_lshort (hour_len_marginals), bounds [15,130]. */
    int64_t d = hu_reply_delay_from_model(path, 10, 5, 50.0, 7);
    HU_ASSERT_TRUE(d >= 15 && d <= 130);
    unlink(path);
    free(path);
}

static void test_from_model_falls_back_to_hour_marginal(void) {
    char *path = write_fixture();
    /* hour=10, incoming_len=500 ("long") — no long-length cell or
     * hour_len_marginal for h10_llong, falls back to h10 marginal,
     * bounds [20,500]. */
    int64_t d = hu_reply_delay_from_model(path, 10, 500, 2.0, 3);
    HU_ASSERT_TRUE(d >= 20 && d <= 500);
    unlink(path);
    free(path);
}

static void test_from_model_falls_back_to_global(void) {
    char *path = write_fixture();
    /* hour=5 has no hour_marginals entry at all -> global [30,900]. */
    int64_t d = hu_reply_delay_from_model(path, 5, 5, 2.0, 3);
    HU_ASSERT_TRUE(d >= 30 && d <= 900);
    unlink(path);
    free(path);
}

static void test_from_model_clamps_out_of_range_hour(void) {
    char *path = write_fixture();
    /* hour=99 clamps to 23, which has a real marginal [100,500]. */
    int64_t d = hu_reply_delay_from_model(path, 99, 5, 2.0, 3);
    HU_ASSERT_TRUE(d >= 100 && d <= 500);
    /* hour=-5 clamps to 0, which has no data anywhere but global. */
    d = hu_reply_delay_from_model(path, -5, 5, 2.0, 3);
    HU_ASSERT_TRUE(d >= 30 && d <= 900);
    unlink(path);
    free(path);
}

static void test_from_model_deterministic_for_same_seed(void) {
    char *path = write_fixture();
    int64_t a = hu_reply_delay_from_model(path, 10, 5, 2.0, 777);
    int64_t b = hu_reply_delay_from_model(path, 10, 5, 2.0, 777);
    HU_ASSERT_EQ(a, b);
    unlink(path);
    free(path);
}

static void test_from_model_seed_zero_does_not_crash_or_loop(void) {
    char *path = write_fixture();
    /* xorshift32's fixed point is 0 -> must be remapped internally. */
    int64_t d = hu_reply_delay_from_model(path, 10, 5, 2.0, 0);
    HU_ASSERT_TRUE(d >= 10 && d <= 120);
    unlink(path);
    free(path);
}

static void test_shadow_log_off_by_default_does_not_crash(void) {
    unsetenv("HU_REPLY_DELAY_MODEL");
    unsetenv("HU_REPLY_DELAY_MODEL_PATH");
    /* Must be a safe no-op even with no model file anywhere. */
    hu_reply_delay_shadow_log(10, 5, 2.0, 42, "contact", 7, 99);
}

static void test_shadow_log_in_shadow_mode_with_missing_model_does_not_crash(void) {
    setenv("HU_REPLY_DELAY_MODEL", "shadow", 1);
    setenv("HU_REPLY_DELAY_MODEL_PATH", "/tmp/hu_reply_delay_model_missing_xyz.json", 1);
    hu_reply_delay_shadow_log(10, 5, 2.0, 42, "contact", 7, 99);
    unsetenv("HU_REPLY_DELAY_MODEL");
    unsetenv("HU_REPLY_DELAY_MODEL_PATH");
}

static void test_shadow_log_in_shadow_mode_with_real_model_does_not_crash(void) {
    char *path = write_fixture();
    setenv("HU_REPLY_DELAY_MODEL", "shadow", 1);
    setenv("HU_REPLY_DELAY_MODEL_PATH", path, 1);
    hu_reply_delay_shadow_log(10, 5, 2.0, 42, "contact", 7, 99);
    unsetenv("HU_REPLY_DELAY_MODEL");
    unsetenv("HU_REPLY_DELAY_MODEL_PATH");
    unlink(path);
    free(path);
}

void run_reply_delay_model_tests(void) {
    HU_TEST_SUITE("reply_delay_model");
    HU_RUN_TEST(test_mode_from_env_default_off);
    HU_RUN_TEST(test_mode_from_env_recognizes_shadow_and_live);
    HU_RUN_TEST(test_mode_from_env_unrecognized_value_fails_closed);
    HU_RUN_TEST(test_from_model_missing_file_returns_negative);
    HU_RUN_TEST(test_from_model_null_path_returns_negative);
    HU_RUN_TEST(test_from_model_malformed_json_returns_negative);
    HU_RUN_TEST(test_from_model_exact_cell_hit_within_bounds);
    HU_RUN_TEST(test_from_model_falls_back_to_hour_len_marginal);
    HU_RUN_TEST(test_from_model_falls_back_to_hour_marginal);
    HU_RUN_TEST(test_from_model_falls_back_to_global);
    HU_RUN_TEST(test_from_model_clamps_out_of_range_hour);
    HU_RUN_TEST(test_from_model_deterministic_for_same_seed);
    HU_RUN_TEST(test_from_model_seed_zero_does_not_crash_or_loop);
    HU_RUN_TEST(test_shadow_log_off_by_default_does_not_crash);
    HU_RUN_TEST(test_shadow_log_in_shadow_mode_with_missing_model_does_not_crash);
    HU_RUN_TEST(test_shadow_log_in_shadow_mode_with_real_model_does_not_crash);
}
