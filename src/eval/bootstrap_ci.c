/* Phase 5 Task 2 (RL SOTA) — bootstrap confidence-interval helper.
 *
 * Percentile-bootstrap on a vector of per-conversation scores:
 *
 *   for r in [0, n_resamples):
 *       draw n indices uniformly with replacement from [0, n)
 *       resampled_means[r] = mean(scores[idx])
 *   sort(resampled_means)
 *   ci_lower = resampled_means[floor((1 - confidence) / 2 * n_resamples)]
 *   ci_upper = resampled_means[floor((1 + confidence) / 2 * n_resamples)]
 *   mean     = mean(scores[..n])     (original-sample mean — NOT bootstrap-mean)
 *
 * Why this signature (round-1 BLOCKER-2 fix):
 *   The earlier sketch took a single scalar baseline and "bootstrapped"
 *   it — that is mathematically degenerate (n_samples == 1).  The
 *   percentile bootstrap requires the per-conversation score vector
 *   so each resample can vary across the actual observed distribution.
 *
 * Why n >= 30 in production (round-3 NEW-MED-3):
 *   Wilson rule-of-thumb floor for the central-limit approximation
 *   that underlies percentile bootstrap to be defensible.  A relaxed
 *   _for_test variant accepts n >= 10 so unit tests can pin the
 *   algorithm without paying for 30+ synthetic scores per case.
 *
 * Determinism (round-1 R13-style):
 *   rand_r(&local_seed) — same seed twice on the same platform
 *   produces byte-identical (mean, ci_low, ci_high).  We deliberately
 *   do NOT touch global rand() (would race with concurrent callers
 *   and break reproducibility).
 *
 * Two-sample (hu_bootstrap_compare_means) — Task 9 baseline-vs-policy:
 *   observed_diff = mean(A) − mean(B)
 *   pool = A ∪ B  (length n_a + n_b)
 *   for r in [0, n_resamples):
 *       resample n_a + n_b indices from pool with replacement
 *       split into halves of size n_a, n_b
 *       boot_diff[r] = mean(half_a) − mean(half_b)
 *   p_value = count(|boot_diff| >= |observed_diff|) / n_resamples
 *
 *   The pooled formulation simulates H0 of "A and B come from the
 *   same distribution"; under that null, the bootstrap diff is
 *   centered at 0 and the observed_diff falls in the tail iff the
 *   distributions actually differ.
 *
 * This TU is wired into HU_CORE_SOURCES behind HU_ENABLE_RL_FULL in
 * CMakeLists.txt so default release/dev builds pay zero binary cost.
 */

#include "human/eval/bootstrap_ci.h"
#include "human/core/error.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int hu__bootstrap_ci_cmp_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

static double hu__bootstrap_ci_vector_mean(const double *xs, size_t n) {
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) sum += xs[i];
    return sum / (double)n;
}

static hu_error_t hu__bootstrap_ci_compute_impl(const double *scores, size_t n,
                                                double confidence,
                                                size_t n_resamples,
                                                unsigned int seed,
                                                size_t n_floor,
                                                double *out_lower,
                                                double *out_upper,
                                                double *out_mean) {
    if (!scores || !out_lower || !out_upper || !out_mean) return HU_ERR_INVALID_ARGUMENT;
    if (n < n_floor) return HU_ERR_INVALID_ARGUMENT;
    if (n_resamples == 0) return HU_ERR_INVALID_ARGUMENT;
    if (!(confidence > 0.0 && confidence < 1.0)) return HU_ERR_INVALID_ARGUMENT;

    double *resample_scratch = (double *)malloc(n * sizeof(double));
    if (!resample_scratch) return HU_ERR_OUT_OF_MEMORY;

    double *resample_means = (double *)malloc(n_resamples * sizeof(double));
    if (!resample_means) {
        free(resample_scratch);
        return HU_ERR_OUT_OF_MEMORY;
    }

    unsigned int local_seed = seed;
    for (size_t r = 0; r < n_resamples; ++r) {
        for (size_t i = 0; i < n; ++i) {
            /* rand_r returns [0, RAND_MAX]; modulo bias is negligible for
             * the typical n <= 10^4 we care about here (bootstrap is robust
             * to small index-sampling bias by construction). */
            size_t idx = (size_t)((unsigned int)rand_r(&local_seed)) % n;
            resample_scratch[i] = scores[idx];
        }
        resample_means[r] = hu__bootstrap_ci_vector_mean(resample_scratch, n);
    }

    qsort(resample_means, n_resamples, sizeof(double), hu__bootstrap_ci_cmp_double);

    double alpha_lo = (1.0 - confidence) * 0.5;
    double alpha_hi = (1.0 + confidence) * 0.5;
    size_t idx_lo = (size_t)floor(alpha_lo * (double)n_resamples);
    size_t idx_hi = (size_t)floor(alpha_hi * (double)n_resamples);
    if (idx_hi >= n_resamples) idx_hi = n_resamples - 1;

    *out_mean  = hu__bootstrap_ci_vector_mean(scores, n);
    *out_lower = resample_means[idx_lo];
    *out_upper = resample_means[idx_hi];

    free(resample_means);
    free(resample_scratch);
    return HU_OK;
}

hu_error_t hu_bootstrap_ci(const double *scores, size_t n,
                           double confidence,
                           size_t n_resamples,
                           unsigned int seed,
                           double *out_lower,
                           double *out_upper,
                           double *out_mean) {
    /* Production floor: round-3 NEW-MED-3 Wilson rule-of-thumb (n >= 30). */
    return hu__bootstrap_ci_compute_impl(scores, n, confidence, n_resamples, seed,
                                         /*n_floor=*/30,
                                         out_lower, out_upper, out_mean);
}

hu_error_t hu_bootstrap_ci_for_test(const double *scores, size_t n,
                                    double confidence,
                                    size_t n_resamples,
                                    unsigned int seed,
                                    double *out_lower,
                                    double *out_upper,
                                    double *out_mean) {
    /* Test floor: relaxed to n >= 10 so unit tests can pin the algorithm
     * without paying for 30+ synthetic scores per case.  Production
     * callers (Task 5 hu_eval_gate_decide, Task 9 competitive harness)
     * MUST use the strict entry. */
    return hu__bootstrap_ci_compute_impl(scores, n, confidence, n_resamples, seed,
                                         /*n_floor=*/10,
                                         out_lower, out_upper, out_mean);
}

hu_error_t hu_bootstrap_compare_means(const double *scores_a, size_t n_a,
                                      const double *scores_b, size_t n_b,
                                      size_t n_resamples,
                                      unsigned int seed,
                                      double *out_mean_a,
                                      double *out_mean_b,
                                      double *out_p_value) {
    if (!scores_a || !scores_b) return HU_ERR_INVALID_ARGUMENT;
    if (!out_mean_a || !out_mean_b || !out_p_value) return HU_ERR_INVALID_ARGUMENT;
    if (n_a < 2 || n_b < 2) return HU_ERR_INVALID_ARGUMENT;
    if (n_resamples == 0) return HU_ERR_INVALID_ARGUMENT;

    size_t n_pool = n_a + n_b;

    double *pool = (double *)malloc(n_pool * sizeof(double));
    if (!pool) return HU_ERR_OUT_OF_MEMORY;
    memcpy(pool, scores_a, n_a * sizeof(double));
    memcpy(pool + n_a, scores_b, n_b * sizeof(double));

    double mean_a = hu__bootstrap_ci_vector_mean(scores_a, n_a);
    double mean_b = hu__bootstrap_ci_vector_mean(scores_b, n_b);
    double observed_diff = mean_a - mean_b;
    double observed_abs  = fabs(observed_diff);

    unsigned int local_seed = seed;
    size_t at_or_above = 0;

    for (size_t r = 0; r < n_resamples; ++r) {
        /* Draw n_a indices from pool for the A-half, n_b for the B-half;
         * each draw is independent (simulating H0 of pooled distribution). */
        double sum_a = 0.0;
        for (size_t i = 0; i < n_a; ++i) {
            size_t idx = (size_t)((unsigned int)rand_r(&local_seed)) % n_pool;
            sum_a += pool[idx];
        }
        double sum_b = 0.0;
        for (size_t i = 0; i < n_b; ++i) {
            size_t idx = (size_t)((unsigned int)rand_r(&local_seed)) % n_pool;
            sum_b += pool[idx];
        }
        double mean_a_r = sum_a / (double)n_a;
        double mean_b_r = sum_b / (double)n_b;
        double diff_r   = mean_a_r - mean_b_r;
        if (fabs(diff_r) >= observed_abs) at_or_above++;
    }

    *out_mean_a  = mean_a;
    *out_mean_b  = mean_b;
    *out_p_value = (double)at_or_above / (double)n_resamples;

    free(pool);
    return HU_OK;
}
