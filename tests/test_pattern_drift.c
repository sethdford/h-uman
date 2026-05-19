/* tests/test_pattern_drift.c
 *
 * Pattern-drift compute-layer tests. The SQL scanner is gated under
 * HU_IS_TEST so we test the stub contract for it. Pure helpers are
 * exercised across boundary cases.
 *
 * Conservative-bias assertions are prominent: flat baselines, sparse
 * recent windows, and tiny histories must NOT produce alerts. */

#include "human/memory/pattern_drift.h"
#include "test_framework.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* --- hu_drift_classify_severity --- */

static void test_classify_severity_zero_is_none(void) {
    HU_ASSERT_EQ((int)hu_drift_classify_severity(0.0), (int)HU_DRIFT_NONE);
}

static void test_classify_severity_half_sigma_is_normal(void) {
    HU_ASSERT_EQ((int)hu_drift_classify_severity(0.5), (int)HU_DRIFT_NORMAL);
    HU_ASSERT_EQ((int)hu_drift_classify_severity(-0.5), (int)HU_DRIFT_NORMAL);
}

static void test_classify_severity_one_and_a_half_is_noticeable(void) {
    HU_ASSERT_EQ((int)hu_drift_classify_severity(1.5), (int)HU_DRIFT_NOTICEABLE);
    HU_ASSERT_EQ((int)hu_drift_classify_severity(-1.5), (int)HU_DRIFT_NOTICEABLE);
}

static void test_classify_severity_two_and_a_half_is_pronounced(void) {
    HU_ASSERT_EQ((int)hu_drift_classify_severity(2.5), (int)HU_DRIFT_PRONOUNCED);
    HU_ASSERT_EQ((int)hu_drift_classify_severity(-2.5), (int)HU_DRIFT_PRONOUNCED);
}

static void test_classify_severity_nan_is_none(void) {
    HU_ASSERT_EQ((int)hu_drift_classify_severity(NAN), (int)HU_DRIFT_NONE);
    HU_ASSERT_EQ((int)hu_drift_classify_severity(INFINITY), (int)HU_DRIFT_NONE);
}

/* --- hu_drift_compute_zscore --- */

static void test_zscore_simple_positive_drift(void) {
    /* recent=120, baseline=100, stddev=10, floor=1 → sigma=2.0 */
    double z = hu_drift_compute_zscore(120.0, 100.0, 10.0, 1.0);
    HU_ASSERT_FLOAT_EQ(z, 2.0, 1e-9);
}

static void test_zscore_negative_drift_is_signed(void) {
    /* recent=80, baseline=100, stddev=10 → sigma=-2.0 */
    double z = hu_drift_compute_zscore(80.0, 100.0, 10.0, 1.0);
    HU_ASSERT_FLOAT_EQ(z, -2.0, 1e-9);
}

static void test_zscore_tiny_stddev_returns_zero_sentinel(void) {
    /* baseline_stddev < min_stddev (3 < 5) → 0 sentinel even though
     * naive division would give a huge value. */
    double z = hu_drift_compute_zscore(120.0, 100.0, 3.0, 5.0);
    HU_ASSERT_FLOAT_EQ(z, 0.0, 1e-9);
}

static void test_zscore_nonfinite_inputs_return_zero(void) {
    HU_ASSERT_FLOAT_EQ(hu_drift_compute_zscore(NAN, 100, 10, 1), 0.0, 1e-9);
    HU_ASSERT_FLOAT_EQ(hu_drift_compute_zscore(100, NAN, 10, 1), 0.0, 1e-9);
    HU_ASSERT_FLOAT_EQ(hu_drift_compute_zscore(100, 100, INFINITY, 1), 0.0, 1e-9);
}

/* --- hu_drift_compute_dimension --- */

static void test_dimension_pronounced_drift_when_recent_far_below_baseline(void) {
    /* Baseline: 30 messages, mean ~100, stddev ~14 (clearly > 10% floor).
     * Recent: 6 messages, all at 50 chars → far below baseline → pronounced. */
    double baseline[30];
    for (size_t i = 0; i < 30; i++)
        baseline[i] = 80.0 + (double)((i * 7) % 41); /* spread 80..120ish */
    double recent[] = {50, 50, 52, 48, 50, 49};

    double sigma = 0, rm = 0, bm = 0;
    hu_drift_severity_t sev = HU_DRIFT_NONE;
    hu_error_t err =
        hu_drift_compute_dimension(recent, 6, 5, baseline, 30, 30, &sigma, &rm, &bm, &sev);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ((int)sev, (int)HU_DRIFT_PRONOUNCED);
    HU_ASSERT_TRUE(sigma < 0); /* negative — drifted shorter */
}

static void test_dimension_negative_direction_also_pronounced(void) {
    /* Alice writes LONGER now — drift in either direction is the same
     * severity. Baseline mean 100 with stddev ~15, recent at 200 → far
     * above baseline → pronounced. */
    double baseline[30];
    for (size_t i = 0; i < 30; i++)
        baseline[i] = 80.0 + (double)((i * 7) % 41); /* spread 80..120ish */
    double recent[10];
    for (size_t i = 0; i < 10; i++)
        recent[i] = 200.0 + (double)i;

    double sigma = 0, rm = 0, bm = 0;
    hu_drift_severity_t sev = HU_DRIFT_NONE;
    hu_error_t err =
        hu_drift_compute_dimension(recent, 10, 5, baseline, 30, 30, &sigma, &rm, &bm, &sev);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ((int)sev, (int)HU_DRIFT_PRONOUNCED);
    HU_ASSERT_TRUE(sigma > 0); /* positive — drifted longer */
}

static void test_dimension_flat_baseline_does_not_alarm(void) {
    /* Adversarial: baseline is dead-flat at 100, recent is 99. Naive
     * z-score would be huge / undefined; the conservative floor must
     * refuse to classify. */
    double baseline[30];
    for (size_t i = 0; i < 30; i++)
        baseline[i] = 100.0;
    double recent[] = {99, 99, 99, 99, 99, 99};

    double sigma = 0, rm = 0, bm = 0;
    hu_drift_severity_t sev = HU_DRIFT_NONE;
    hu_error_t err =
        hu_drift_compute_dimension(recent, 6, 5, baseline, 30, 30, &sigma, &rm, &bm, &sev);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    /* sigma must be 0 sentinel (refused classification). NORMAL severity
     * is what classify() returns for sigma=0 → wait, NONE.
     * hu_drift_classify_severity(0) == HU_DRIFT_NONE, so sev==NONE. */
    HU_ASSERT_FLOAT_EQ(sigma, 0.0, 1e-9);
    HU_ASSERT_EQ((int)sev, (int)HU_DRIFT_NONE);
}

static void test_dimension_insufficient_recent_returns_none(void) {
    /* 4 recent observations is below min_recent_n=5 → NONE, no signal. */
    double baseline[30];
    for (size_t i = 0; i < 30; i++)
        baseline[i] = 100.0 + ((i % 7) * 2.0);
    double recent[] = {50, 50, 50, 50};

    double sigma = 0, rm = 0, bm = 0;
    hu_drift_severity_t sev = HU_DRIFT_NONE;
    hu_error_t err =
        hu_drift_compute_dimension(recent, 4, 5, baseline, 30, 30, &sigma, &rm, &bm, &sev);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ((int)sev, (int)HU_DRIFT_NONE);
    HU_ASSERT_FLOAT_EQ(sigma, 0.0, 1e-9);
}

static void test_dimension_insufficient_baseline_returns_none(void) {
    double baseline[20];
    for (size_t i = 0; i < 20; i++)
        baseline[i] = 100.0;
    double recent[] = {50, 50, 50, 50, 50, 50};

    double sigma = 0, rm = 0, bm = 0;
    hu_drift_severity_t sev = HU_DRIFT_NONE;
    hu_error_t err =
        hu_drift_compute_dimension(recent, 6, 5, baseline, 20, 30, &sigma, &rm, &bm, &sev);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ((int)sev, (int)HU_DRIFT_NONE);
}

static void test_dimension_null_with_nonzero_size_is_error(void) {
    double baseline[30] = {0};
    double sigma = 0, rm = 0, bm = 0;
    hu_drift_severity_t sev = HU_DRIFT_NONE;
    hu_error_t err =
        hu_drift_compute_dimension(NULL, 6, 5, baseline, 30, 30, &sigma, &rm, &bm, &sev);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_INVALID_ARGUMENT);
}

static void test_dimension_normal_drift_within_one_sigma(void) {
    /* Drift exists but only ~0.5 sigma — should classify as NORMAL,
     * not noticeable, not pronounced. */
    double baseline[30];
    for (size_t i = 0; i < 30; i++)
        baseline[i] = 100.0 + (double)(((int)(i % 21)) - 10); /* mean 100, sample stddev ~6 */
    double recent[10];
    for (size_t i = 0; i < 10; i++)
        recent[i] = 103.0; /* recent_mean ~103 → ~0.5 sigma */

    double sigma = 0, rm = 0, bm = 0;
    hu_drift_severity_t sev = HU_DRIFT_NONE;
    hu_error_t err =
        hu_drift_compute_dimension(recent, 10, 5, baseline, 30, 30, &sigma, &rm, &bm, &sev);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_TRUE(sev == HU_DRIFT_NORMAL || sev == HU_DRIFT_NONE);
    HU_ASSERT_TRUE(fabs(sigma) < 1.0);
}

/* --- SQL scanner: HU_IS_TEST stub contract --- */

static void test_compute_for_contact_returns_not_supported_in_test_mode(void) {
    hu_drift_alert_t out[HU_DRIFT_DIM_COUNT] = {0};
    size_t n = 99;
    hu_error_t err = hu_drift_compute_for_contact("/tmp/nonexistent.db", "+15551234567", 1700000000,
                                                  out, HU_DRIFT_DIM_COUNT, &n);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ((int)n, 0);
}

static void test_scan_top_contacts_returns_not_supported_in_test_mode(void) {
    hu_drift_alert_t out[8] = {0};
    size_t n = 99;
    hu_error_t err = hu_drift_scan_top_contacts("/tmp/nonexistent.db", 1700000000, 5, out, 8, &n);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ((int)n, 0);
}

void run_pattern_drift_tests(void) {
    HU_TEST_SUITE("pattern_drift");
    HU_RUN_TEST(test_classify_severity_zero_is_none);
    HU_RUN_TEST(test_classify_severity_half_sigma_is_normal);
    HU_RUN_TEST(test_classify_severity_one_and_a_half_is_noticeable);
    HU_RUN_TEST(test_classify_severity_two_and_a_half_is_pronounced);
    HU_RUN_TEST(test_classify_severity_nan_is_none);
    HU_RUN_TEST(test_zscore_simple_positive_drift);
    HU_RUN_TEST(test_zscore_negative_drift_is_signed);
    HU_RUN_TEST(test_zscore_tiny_stddev_returns_zero_sentinel);
    HU_RUN_TEST(test_zscore_nonfinite_inputs_return_zero);
    HU_RUN_TEST(test_dimension_pronounced_drift_when_recent_far_below_baseline);
    HU_RUN_TEST(test_dimension_negative_direction_also_pronounced);
    HU_RUN_TEST(test_dimension_flat_baseline_does_not_alarm);
    HU_RUN_TEST(test_dimension_insufficient_recent_returns_none);
    HU_RUN_TEST(test_dimension_insufficient_baseline_returns_none);
    HU_RUN_TEST(test_dimension_null_with_nonzero_size_is_error);
    HU_RUN_TEST(test_dimension_normal_drift_within_one_sigma);
    HU_RUN_TEST(test_compute_for_contact_returns_not_supported_in_test_mode);
    HU_RUN_TEST(test_scan_top_contacts_returns_not_supported_in_test_mode);
}
