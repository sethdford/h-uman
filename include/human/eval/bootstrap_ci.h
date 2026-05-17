#ifndef HU_EVAL_BOOTSTRAP_CI_H
#define HU_EVAL_BOOTSTRAP_CI_H

/* Phase 5 Task 2 (RL SOTA) — bootstrap confidence-interval helper.
 *
 * Percentile-bootstrap CI on a vector of per-conversation scalar scores
 * (NOT a single aggregated mean — round-1 BLOCKER-2 fix; the degenerate
 * single-sample bootstrap is rejected at the type level by requiring
 * an array + length).
 *
 * The production entry point requires n >= 30 — round-3 NEW-MED-3
 * Wilson rule-of-thumb floor for the central-limit approximation that
 * underlies percentile bootstrap to be defensible.  A relaxed
 * _for_test variant accepts n >= 10 so unit tests can pin the
 * algorithm without paying for 30+ synthetic scores per case.
 *
 * Determinism: rand_r(&local_seed) per call (NOT the global libc rand
 * pool), so concurrent callers do not interfere and so seed=42 twice
 * produces byte-identical (mean, ci_low, ci_high) on the same machine.
 *
 * hu_bootstrap_compare_means implements the Task 9 baseline-vs-policy
 * two-sample test: pooled bootstrap simulating H0 of equal-distribution
 * (resample n_a and n_b draws from the A∪B pool independently each
 * iteration); the returned p-value is the fraction of bootstrap
 * |diff_means| at least as extreme as the observed |mean_a − mean_b|.
 *
 * Gating: this TU is wired into HU_CORE_SOURCES behind HU_ENABLE_RL_FULL
 * in CMakeLists.txt so default release/dev builds have zero binary
 * impact.
 */

#include "human/core/error.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Production entry — percentile bootstrap CI on a vector of per-
 * conversation scalar scores.
 *
 * scores:        pointer to n doubles (per-conversation values; NOT a
 *                single aggregated mean — round-1 BLOCKER-2).
 * n:             vector length.  MUST be >= 30 (round-3 NEW-MED-3
 *                Wilson floor); smaller n returns
 *                HU_ERR_INVALID_ARGUMENT without touching the out
 *                params so a noisy small-N caller cannot silently
 *                downgrade the Task 5 eval gate.
 * confidence:    in (0.0, 1.0) — e.g. 0.95 for a 95% CI.
 * n_resamples:   typical 1000–10000.  MUST be >= 1.
 * seed:          per-call rand_r() seed for reproducibility.
 * out_lower,
 * out_upper,
 * out_mean:      mandatory output pointers; written only on HU_OK.
 *                out_mean is the original-sample mean (NOT the
 *                bootstrap-mean of means).
 *
 * Returns HU_OK on success, HU_ERR_INVALID_ARGUMENT on any
 * precondition violation, or HU_ERR_OUT_OF_MEMORY if the scratch
 * allocations fail.
 */
hu_error_t hu_bootstrap_ci(const double *scores, size_t n,
                           double confidence,
                           size_t n_resamples,
                           unsigned int seed,
                           double *out_lower,
                           double *out_upper,
                           double *out_mean);

/* TEST-ONLY variant — same algorithm, relaxed n >= 10 precondition so
 * unit tests can pin the math without 30+ synthetic scores per case.
 *
 * Production callers (Task 5 hu_eval_gate_decide, Task 9 competitive
 * harness) MUST use the strict entry above; this exists to let the
 * test surface assert that the relaxed floor still rejects
 * pathologically small samples (n < 10).
 */
hu_error_t hu_bootstrap_ci_for_test(const double *scores, size_t n,
                                    double confidence,
                                    size_t n_resamples,
                                    unsigned int seed,
                                    double *out_lower,
                                    double *out_upper,
                                    double *out_mean);

/* Two-sample bootstrap test for A-vs-B difference of means.
 *
 * Used by the Task 9 competitive harness to compute the p-value of the
 * observed |mean(A) − mean(B)|.  Pooled-bootstrap formulation: each
 * iteration draws n_a + n_b indices from the pooled A∪B vector with
 * replacement (simulating H0 of identical distribution), splits them
 * into a length-n_a half and a length-n_b half, and records the
 * difference of means.  The returned p-value is the fraction of
 * bootstrap |diff_means| ≥ |observed_diff|.
 *
 * Requires n_a >= 2 and n_b >= 2 (variance is undefined otherwise);
 * production callers SHOULD pass n_a, n_b >= 30 for Wilson-floor
 * validity, but this is the caller's responsibility — the function
 * does not silently re-impose the 30-floor here because the Task 9
 * use case is "compare two roughly-equal-N runs", not "decide on a
 * single CI's validity".
 *
 * out_mean_a, out_mean_b: original-sample means (echoed for caller
 *                         convenience so the harness need not
 *                         recompute).
 * out_p_value:           p-value in [0.0, 1.0] of the observed
 *                         |diff_means| under the H0 of identical
 *                         pooled distribution.
 *
 * Returns HU_OK on success, HU_ERR_INVALID_ARGUMENT on any
 * precondition violation, or HU_ERR_OUT_OF_MEMORY if the scratch
 * allocations fail.
 */
hu_error_t hu_bootstrap_compare_means(const double *scores_a, size_t n_a,
                                      const double *scores_b, size_t n_b,
                                      size_t n_resamples,
                                      unsigned int seed,
                                      double *out_mean_a,
                                      double *out_mean_b,
                                      double *out_p_value);

#ifdef __cplusplus
}
#endif

#endif /* HU_EVAL_BOOTSTRAP_CI_H */
