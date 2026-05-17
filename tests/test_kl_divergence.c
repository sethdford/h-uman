/* tests/test_kl_divergence.c — Phase 4 Task 1 (RL SOTA).
 *
 * Pins the API contract of the Schulman k1/k2/k3 estimators + the k3
 * analytical backward grad. Six tests covering the round-3 fix H4
 * surface (mean forward, /v backward) plus the v == 0 graceful path. */

#include "human/ml/kl_divergence.h"
#include "test_framework.h"

#include <math.h>
#include <string.h>

/* k1 = mean(logp_pol - logp_ref). When logp_pol < logp_ref pointwise
 * (policy assigns LOWER log-prob to every token than the reference)
 * the mean of (pol - ref) is negative. This is the property that
 * pins k1 as BIASED — Schulman's note that k1 can be negative even
 * though true KL is non-negative is the motivation for k3. */
static void test_kl_k1_can_be_negative(void) {
    const size_t v = 4;
    double logp_pol[4] = {-2.0, -2.0, -2.0, -2.0};
    double logp_ref[4] = {-1.0, -1.0, -1.0, -1.0};
    double k1 = 99.0;
    hu_kl_k1(logp_pol, logp_ref, v, &k1);
    /* mean( (-2) - (-1) ) = -1.0 across all 4 positions */
    HU_ASSERT_FLOAT_EQ(k1, -1.0, 1e-12);
    HU_ASSERT(k1 < 0.0);
}

/* k2 = 0.5 * mean((pol - ref)^2) — squared term is always non-negative,
 * so k2 is always non-negative regardless of input drift. */
static void test_kl_k2_always_non_negative(void) {
    const size_t v = 5;
    double logp_pol[5] = {-0.5, -1.2, -3.0, -2.1, -0.9};
    double logp_ref[5] = {-1.8, -0.3, -2.7, -1.4, -2.0};
    double k2 = -1.0;
    hu_kl_k2(logp_pol, logp_ref, v, &k2);
    HU_ASSERT(k2 >= 0.0);

    /* Another drift in the opposite direction — still non-negative. */
    double logp_pol2[5] = {-3.0, -2.5, -1.5, -0.8, -0.2};
    double logp_ref2[5] = {-0.1, -0.4, -0.9, -1.6, -3.3};
    double k2b = -1.0;
    hu_kl_k2(logp_pol2, logp_ref2, v, &k2b);
    HU_ASSERT(k2b >= 0.0);

    /* Equal inputs collapse to 0 (boundary of the non-negative claim). */
    double k2c = -1.0;
    hu_kl_k2(logp_pol, logp_pol, v, &k2c);
    HU_ASSERT_FLOAT_EQ(k2c, 0.0, 1e-12);
}

/* k3 = mean(exp(r_i) - r_i - 1) with r_i = logp_ref[i] - logp_pol[i].
 * When logp_pol == logp_ref pointwise, r_i = 0 -> exp(0) - 0 - 1 = 0,
 * and the mean is exactly 0. This is the GRPO-critical property:
 * "KL penalty must be zero when policy equals reference". */
static void test_kl_k3_zero_when_pol_equals_ref(void) {
    const size_t v = 6;
    double logp[6] = {-0.5, -1.2, -2.4, -0.1, -3.3, -1.7};
    double k3 = 99.0;
    hu_kl_k3(logp, logp, v, &k3);
    HU_ASSERT_FLOAT_EQ(k3, 0.0, 1e-12);
}

/* k3 is always non-negative for any input by construction of the
 * Schulman estimator: f(r) = exp(r) - r - 1 has f'(r) = exp(r) - 1
 * with single root at r=0 and f(0) = 0, so f(r) >= 0 everywhere. */
static void test_kl_k3_always_non_negative(void) {
    const size_t v = 5;

    /* Case A: small drift. */
    double pol_a[5] = {-0.5, -1.0, -1.5, -2.0, -2.5};
    double ref_a[5] = {-0.4, -1.1, -1.6, -1.9, -2.6};
    double k_a = -1.0;
    hu_kl_k3(pol_a, ref_a, v, &k_a);
    HU_ASSERT(k_a >= 0.0);

    /* Case B: opposite-sign drift. */
    double pol_b[5] = {-2.5, -2.0, -1.5, -1.0, -0.5};
    double ref_b[5] = {-0.4, -1.1, -1.6, -1.9, -2.6};
    double k_b = -1.0;
    hu_kl_k3(pol_b, ref_b, v, &k_b);
    HU_ASSERT(k_b >= 0.0);

    /* Case C: large drift — still finite and non-negative thanks to
     * safe_exp() clamp. */
    double pol_c[5] = {-50.0, -50.0, -50.0, -50.0, -50.0};
    double ref_c[5] = {-1.0, -1.0, -1.0, -1.0, -1.0};
    double k_c = -1.0;
    hu_kl_k3(pol_c, ref_c, v, &k_c);
    HU_ASSERT(k_c >= 0.0);
    HU_ASSERT(isfinite(k_c));
}

/* Round-3 fix H4: the analytical gradient is (1 - exp(r_i)) / v —
 * the /v term comes from the MEAN forward. The finite-difference
 * probe MUST therefore use the same MEAN forward and the resulting
 * numerical gradient MUST match the analytical /v form within
 * standard finite-difference tolerance. */
static void test_kl_k3_backward_finite_diff_matches_analytical(void) {
    const size_t v = 4;
    double logp_pol[4] = {-0.5, -1.0, -1.5, -2.0};
    double logp_ref[4] = {-0.3, -1.4, -1.7, -1.6};

    double grad[4];
    hu_kl_k3_backward(logp_pol, logp_ref, v, grad);

    /* Sanity: analytical formula stated explicitly. */
    for (size_t i = 0; i < v; ++i) {
        double r = logp_ref[i] - logp_pol[i];
        double expected = (1.0 - exp(r)) / (double)v;
        HU_ASSERT_FLOAT_EQ(grad[i], expected, 1e-12);
    }

    /* Finite-difference vs analytical (central difference, eps=1e-5). */
    const double eps = 1e-5;
    for (size_t i = 0; i < v; ++i) {
        double saved = logp_pol[i];

        double pol_plus[4];
        double pol_minus[4];
        memcpy(pol_plus, logp_pol, sizeof(pol_plus));
        memcpy(pol_minus, logp_pol, sizeof(pol_minus));
        pol_plus[i] = saved + eps;
        pol_minus[i] = saved - eps;

        double k_plus = 0.0;
        double k_minus = 0.0;
        hu_kl_k3(pol_plus, logp_ref, v, &k_plus);
        hu_kl_k3(pol_minus, logp_ref, v, &k_minus);

        double fd = (k_plus - k_minus) / (2.0 * eps);
        HU_ASSERT_FLOAT_EQ(grad[i], fd, 1e-5);
    }
}

/* v == 0 must not read past the (possibly null) inputs, must write 0
 * to *out_kl_mean (so callers don't read uninitialized memory), and
 * must leave grad_logp_pol untouched (zero-length array, nothing to
 * write). No NaN/Inf may appear. */
static void test_kl_k3_handles_zero_vocab_gracefully(void) {
    /* Forward: v == 0 with non-null arrays writes 0. */
    double dummy = 1.0;
    double k = 999.0;
    hu_kl_k1(&dummy, &dummy, 0, &k);
    HU_ASSERT_FLOAT_EQ(k, 0.0, 1e-12);
    HU_ASSERT(isfinite(k));

    k = 999.0;
    hu_kl_k2(&dummy, &dummy, 0, &k);
    HU_ASSERT_FLOAT_EQ(k, 0.0, 1e-12);
    HU_ASSERT(isfinite(k));

    k = 999.0;
    hu_kl_k3(&dummy, &dummy, 0, &k);
    HU_ASSERT_FLOAT_EQ(k, 0.0, 1e-12);
    HU_ASSERT(isfinite(k));

    /* Forward: NULL inputs are silently treated as v == 0 (defensive
     * path so a caller forgetting to allocate doesn't UB). */
    k = 999.0;
    hu_kl_k3(NULL, &dummy, 4, &k);
    HU_ASSERT_FLOAT_EQ(k, 0.0, 1e-12);
    k = 999.0;
    hu_kl_k3(&dummy, NULL, 4, &k);
    HU_ASSERT_FLOAT_EQ(k, 0.0, 1e-12);

    /* Backward: v == 0 writes nothing — sentinel must survive. */
    double sentinel[1] = {123.456};
    hu_kl_k3_backward(&dummy, &dummy, 0, sentinel);
    HU_ASSERT_FLOAT_EQ(sentinel[0], 123.456, 1e-12);
    HU_ASSERT(isfinite(sentinel[0]));

    /* Backward: NULL inputs / null grad pointer are also silent. */
    hu_kl_k3_backward(NULL, &dummy, 4, sentinel);
    HU_ASSERT_FLOAT_EQ(sentinel[0], 123.456, 1e-12);
    hu_kl_k3_backward(&dummy, &dummy, 4, NULL);
    /* No assertion needed — we just verify no crash / no UB. */
}

void run_kl_divergence_tests(void) {
    HU_TEST_SUITE("kl_divergence");
    HU_RUN_TEST(test_kl_k1_can_be_negative);
    HU_RUN_TEST(test_kl_k2_always_non_negative);
    HU_RUN_TEST(test_kl_k3_zero_when_pol_equals_ref);
    HU_RUN_TEST(test_kl_k3_always_non_negative);
    HU_RUN_TEST(test_kl_k3_backward_finite_diff_matches_analytical);
    HU_RUN_TEST(test_kl_k3_handles_zero_vocab_gracefully);
}
