/* Phase 5 Task 2 (RL SOTA) — bootstrap CI helper tests.
 *
 * Pins the round-1 BLOCKER-2 and round-3 NEW-MED-3 contracts:
 *   - Vector signature (per-conversation scores, NOT a single scalar).
 *   - n >= 30 floor in production hu_bootstrap_ci.
 *   - n >= 10 floor in hu_bootstrap_ci_for_test.
 *   - Determinism on identical seed.
 *   - Finite + mean-bracketing CI on a known Normal(0.5, 0.1) sample.
 *   - hu_bootstrap_compare_means p-value low for distinct distributions,
 *     high for identical distributions.
 *
 * Synthetic Normal samples are generated via a deterministic Box-Muller
 * transform fed by rand_r() with a fixed seed — no real RNG in tests
 * (HU_IS_TEST principle: deterministic, no external entropy).
 */

#include "human/core/error.h"
#include "human/eval/bootstrap_ci.h"
#include "test_framework.h"

#include <math.h>
#include <stdlib.h>

#ifndef HU_BOOTSTRAP_CI_TEST_PI
#define HU_BOOTSTRAP_CI_TEST_PI 3.14159265358979323846
#endif

/* ── Deterministic Normal sampler (Box-Muller via rand_r) ─────────── */

static double hu__test_uniform(unsigned int *seedp) {
    /* Add 1 so we never get exactly 0 (would blow up Box-Muller's log). */
    return ((double)rand_r(seedp) + 1.0) / ((double)RAND_MAX + 2.0);
}

static double hu__test_normal(double mean, double std, unsigned int *seedp) {
    double u1 = hu__test_uniform(seedp);
    double u2 = hu__test_uniform(seedp);
    double z = sqrt(-2.0 * log(u1)) * cos(2.0 * HU_BOOTSTRAP_CI_TEST_PI * u2);
    return mean + std * z;
}

static void hu__test_fill_normal(double *xs, size_t n, double mean, double std,
                                  unsigned int seed) {
    unsigned int s = seed;
    for (size_t i = 0; i < n; ++i) xs[i] = hu__test_normal(mean, std, &s);
}

/* ── Tests ────────────────────────────────────────────────────────── */

static void test_bootstrap_ci_rejects_n_below_30_in_production(void) {
    /* round-3 NEW-MED-3: Wilson floor n >= 30 in production. */
    double xs[29];
    for (size_t i = 0; i < 29; ++i) xs[i] = 0.5;
    double lo = 99.0, hi = 99.0, m = 99.0;
    hu_error_t e = hu_bootstrap_ci(xs, 29, /*conf=*/0.95, /*B=*/100,
                                   /*seed=*/42, &lo, &hi, &m);
    HU_ASSERT_EQ(e, HU_ERR_INVALID_ARGUMENT);
    /* Out params untouched (per header contract). */
    HU_ASSERT_FLOAT_EQ(lo, 99.0, 1e-12);
    HU_ASSERT_FLOAT_EQ(hi, 99.0, 1e-12);
    HU_ASSERT_FLOAT_EQ(m,  99.0, 1e-12);
}

static void test_bootstrap_ci_for_test_rejects_n_below_10(void) {
    /* _for_test variant accepts n >= 10; n=9 must still be rejected. */
    double xs[9];
    for (size_t i = 0; i < 9; ++i) xs[i] = 0.5;
    double lo = 0, hi = 0, m = 0;
    hu_error_t e = hu_bootstrap_ci_for_test(xs, 9, 0.95, 100, 42, &lo, &hi, &m);
    HU_ASSERT_EQ(e, HU_ERR_INVALID_ARGUMENT);

    /* And n=10 is the threshold — must succeed. */
    double ys[10];
    for (size_t i = 0; i < 10; ++i) ys[i] = (double)i / 10.0;
    e = hu_bootstrap_ci_for_test(ys, 10, 0.95, 200, 42, &lo, &hi, &m);
    HU_ASSERT_EQ(e, HU_OK);
}

static void test_bootstrap_ci_returns_finite_lower_upper_for_known_distribution(void) {
    /* round-1 BLOCKER-2: vector input, not scalar; the resample
     * distribution is non-degenerate so the CI brackets the mean. */
    enum { N = 50 };
    double xs[N];
    hu__test_fill_normal(xs, N, /*mean=*/0.5, /*std=*/0.1, /*seed=*/1234);

    double lo = 0, hi = 0, m = 0;
    hu_error_t e = hu_bootstrap_ci(xs, N, /*conf=*/0.95, /*B=*/1000,
                                   /*seed=*/42, &lo, &hi, &m);
    HU_ASSERT_EQ(e, HU_OK);

    HU_ASSERT_TRUE(isfinite(lo));
    HU_ASSERT_TRUE(isfinite(hi));
    HU_ASSERT_TRUE(isfinite(m));
    /* Mean of Normal(0.5, 0.1) with n=50 should be within ~3 std-errors
     * (3 * 0.1/sqrt(50) ≈ 0.042) of 0.5 — keep a generous 0.15 slack to
     * absorb the deterministic Box-Muller seed variance without flake. */
    HU_ASSERT_TRUE(fabs(m - 0.5) < 0.15);
    /* CI brackets the sample mean. */
    HU_ASSERT_TRUE(lo < m);
    HU_ASSERT_TRUE(m < hi);
    /* CI has positive width (i.e. the resample distribution is
     * non-degenerate — BLOCKER-2 pin). */
    HU_ASSERT_TRUE(hi - lo > 0.0);
}

static void test_bootstrap_ci_deterministic_with_same_seed(void) {
    /* Determinism: identical (scores, seed) → byte-identical CI. */
    enum { N = 50 };
    double xs[N];
    hu__test_fill_normal(xs, N, 0.5, 0.1, /*seed=*/7);

    double lo1 = 0, hi1 = 0, m1 = 0;
    double lo2 = 0, hi2 = 0, m2 = 0;
    HU_ASSERT_EQ(hu_bootstrap_ci(xs, N, 0.95, 1000, /*seed=*/42, &lo1, &hi1, &m1), HU_OK);
    HU_ASSERT_EQ(hu_bootstrap_ci(xs, N, 0.95, 1000, /*seed=*/42, &lo2, &hi2, &m2), HU_OK);

    HU_ASSERT_FLOAT_EQ(lo1, lo2, 0.0);  /* exact bit-identical */
    HU_ASSERT_FLOAT_EQ(hi1, hi2, 0.0);
    HU_ASSERT_FLOAT_EQ(m1,  m2,  0.0);
}

static void test_bootstrap_compare_means_p_value_low_for_clearly_different_distributions(void) {
    /* A ~ N(0, 0.1), B ~ N(1, 0.1), n=100 each — distributions sit ~10
     * std errors apart, so the pooled-bootstrap p-value should be ≪ 0.05. */
    enum { N = 100 };
    double a[N], b[N];
    hu__test_fill_normal(a, N, /*mean=*/0.0, /*std=*/0.1, /*seed=*/101);
    hu__test_fill_normal(b, N, /*mean=*/1.0, /*std=*/0.1, /*seed=*/202);

    double mean_a = 0, mean_b = 0, p = 0;
    hu_error_t e = hu_bootstrap_compare_means(a, N, b, N,
                                              /*B=*/1000, /*seed=*/42,
                                              &mean_a, &mean_b, &p);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_TRUE(p < 0.05);
    /* Sanity: sample means roughly match the truth. */
    HU_ASSERT_TRUE(fabs(mean_a - 0.0) < 0.05);
    HU_ASSERT_TRUE(fabs(mean_b - 1.0) < 0.05);
}

static void test_bootstrap_compare_means_p_value_high_for_same_distribution(void) {
    /* A and B both ~ N(0.5, 0.1) — the pooled bootstrap should
     * (correctly) fail to detect any difference, p > 0.1. */
    enum { N = 100 };
    double a[N], b[N];
    hu__test_fill_normal(a, N, 0.5, 0.1, /*seed=*/303);
    hu__test_fill_normal(b, N, 0.5, 0.1, /*seed=*/404);

    double mean_a = 0, mean_b = 0, p = 0;
    hu_error_t e = hu_bootstrap_compare_means(a, N, b, N, 1000, 42,
                                              &mean_a, &mean_b, &p);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_TRUE(p > 0.1);
}

/* ── Suite runner ─────────────────────────────────────────────────── */

void run_bootstrap_ci_tests(void) {
    HU_TEST_SUITE("bootstrap_ci");
    HU_RUN_TEST(test_bootstrap_ci_rejects_n_below_30_in_production);
    HU_RUN_TEST(test_bootstrap_ci_for_test_rejects_n_below_10);
    HU_RUN_TEST(test_bootstrap_ci_returns_finite_lower_upper_for_known_distribution);
    HU_RUN_TEST(test_bootstrap_ci_deterministic_with_same_seed);
    HU_RUN_TEST(test_bootstrap_compare_means_p_value_low_for_clearly_different_distributions);
    HU_RUN_TEST(test_bootstrap_compare_means_p_value_high_for_same_distribution);
}
