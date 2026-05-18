#ifndef HU_ML_DP_SGD_H
#define HU_ML_DP_SGD_H

/* Sprint 42 / US-42.1 — canonical DP-SGD implementation.
 *
 * Three pure-predicate-style functions plus a tiny stateful accountant. The
 * three learner backends (cpu, ggml, mlx) MUST delegate all clip+noise math
 * to `hu_dp_sgd_step()` rather than implementing it locally. The accountant
 * tracks composition in Rényi-DP space using the Mironov-Talwar-Zhang 2019
 * closed-form for the subsampled Gaussian mechanism, then converts to
 * (epsilon, delta) via Canonne-Kamath-Steinke 2020 Proposition 12 at query
 * time.
 *
 *   1. hu_dp_sgd_noise_sigma()       — clip_norm * noise_multiplier
 *   2. hu_dp_rdp_epsilon_from_sigma()— closed-form RDP → (eps, delta)
 *   3. hu_dp_sgd_step()              — per-sample clip + noise + average
 *
 * The previous implementation in src/ml/learner_cpu.c (lines 320-343) was
 * **mathematically void**: it computed ONE summed gradient across the batch,
 * clipped that one vector, then noised. True DP-SGD requires each sample's
 * gradient to be clipped INDEPENDENTLY before summation; any single sample's
 * contribution to the released sum must be bounded by C. See the design at
 * sprints/sprint-42/designs/US-42.1.md for the full rationale.
 *
 * All functions are deterministic given a seeded `hu_rng_t *` (or no RNG for
 * the pure-math ones). No global state, no `rand()`, no `/dev/urandom`.
 */

#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── PRNG ─────────────────────────────────────────────────────────────────
 * The clip+noise step needs Gaussian samples. We embed a tiny splitmix64
 * PRNG so the canonical implementation is self-contained — backends pass
 * a seed, we deterministically produce noise. */
typedef struct hu_dp_rng {
    uint64_t state;
} hu_dp_rng_t;

void hu_dp_rng_seed(hu_dp_rng_t *r, uint64_t seed);
double hu_dp_rng_normal(hu_dp_rng_t *r); /* mean 0, stddev 1 */

/* ── 1. noise sigma ───────────────────────────────────────────────────────
 * Effective Gaussian standard deviation for the released SUM of per-sample
 * clipped gradients (each bounded by clip_norm). In Opacus / TF-Privacy:
 *
 *     sigma = noise_multiplier * clip_norm
 *
 * Returns 0 if inputs are non-positive; callers should treat non-positive
 * return as "DP disabled or invalid". */
double hu_dp_sgd_noise_sigma(double clip_norm, double noise_multiplier);

/* ── 2. RDP → (epsilon, delta) ────────────────────────────────────────────
 * Closed-form RDP for the subsampled Gaussian mechanism (MTZ 2019 Thm 4),
 * composed additively across `steps` and converted to (epsilon, delta) via
 * CKS 2020 Prop 12.
 *
 * Inputs:
 *   noise_multiplier  σ / C ; > 0
 *   sampling_rate     q ∈ (0, 1]; Poisson subsampling
 *   steps             non-negative integer; 0 → epsilon=0
 *   delta             target δ ∈ (0, 1)
 *
 * Output:
 *   out_epsilon       minimum epsilon over the alpha grid [2,4,...,256]
 *   out_argmin_alpha  the alpha that achieved that minimum (NULL ok)
 *
 * Returns HU_OK on success; HU_ERR_INVALID_ARGUMENT on any non-finite or
 * out-of-range input. Computation is performed in log-space (logsumexp) to
 * avoid overflow for small q and large alpha. */
hu_error_t hu_dp_rdp_epsilon_from_sigma(double noise_multiplier, double sampling_rate,
                                        uint64_t steps, double delta, double *out_epsilon,
                                        double *out_argmin_alpha);

/* ── 3. clip + noise + sum + average ──────────────────────────────────────
 * Apply per-sample gradient clipping and Gaussian noise to a batch of
 * per-sample gradients. Each row in `per_sample_grads` is one sample's
 * D-dimensional gradient; the function clips EACH ROW independently to
 * `clip_norm` L2-norm, sums the clipped rows, adds N(0, sigma^2 * I) noise,
 * then divides by `n_samples` to recover the batch mean.
 *
 * Layout:
 *   per_sample_grads  row-major, shape [n_samples, dim], length n_samples*dim
 *   out_grad          length `dim`, receives the noised average
 *
 * Rejects:
 *   - per_sample_grads NULL with n_samples > 0
 *   - out_grad NULL or dim == 0 or n_samples == 0
 *   - clip_norm <= 0 (the adversarial AC: caller bypassed clipping)
 *   - noise_multiplier < 0
 *   - rng NULL when noise_multiplier > 0
 *
 * Returns HU_OK on success, HU_ERR_INVALID_ARGUMENT otherwise.
 *
 * NB: the AC-42.1.1 contract is that for any row whose pre-clip L2-norm
 * exceeds clip_norm, its CONTRIBUTION to the released sum is bounded as if
 * its norm were exactly clip_norm. Tests verify this empirically. */
hu_error_t hu_dp_sgd_step(const float *per_sample_grads, size_t n_samples, size_t dim,
                          double clip_norm, double noise_multiplier, hu_dp_rng_t *rng,
                          float *out_grad);

/* ── 4. accountant ────────────────────────────────────────────────────────
 * Stateful RDP accountant. Records (noise_multiplier, sampling_rate, steps)
 * tuples; at query time re-runs the closed-form across the alpha grid against
 * the cumulative state. Same-(σ, q) calls compose additively in RDP space —
 * we collapse them into one (σ, q, total_steps) bucket; different (σ, q)
 * calls are summed as parallel mechanisms.
 *
 * The accountant matches Opacus 1.5.x RDPAccountant semantics. */
#define HU_DP_RDP_ACCOUNTANT_MAX_BUCKETS 8

typedef struct hu_dp_rdp_bucket {
    double noise_multiplier;
    double sampling_rate;
    uint64_t steps;
} hu_dp_rdp_bucket_t;

typedef struct hu_dp_rdp_accountant {
    hu_dp_rdp_bucket_t buckets[HU_DP_RDP_ACCOUNTANT_MAX_BUCKETS];
    int bucket_count;
    double delta;          /* fixed target δ for queries */
    double epsilon_budget; /* configured cap; <= 0 means "no cap" */
} hu_dp_rdp_accountant_t;

void hu_dp_rdp_accountant_init(hu_dp_rdp_accountant_t *a, double delta, double epsilon_budget);

/* Record one training step that drew samples at the given rate under the
 * given noise multiplier. */
hu_error_t hu_dp_rdp_accountant_step(hu_dp_rdp_accountant_t *a, double noise_multiplier,
                                     double sampling_rate);

/* Current total epsilon at the accountant's delta. */
hu_error_t hu_dp_rdp_accountant_epsilon(const hu_dp_rdp_accountant_t *a, double *out_epsilon);

/* Would a hypothetical next step with (noise_multiplier, sampling_rate)
 * push the total epsilon past epsilon_budget? Returns true via *out_exceeds.
 * If epsilon_budget <= 0 the answer is always false. */
hu_error_t hu_dp_rdp_accountant_would_exceed(const hu_dp_rdp_accountant_t *a,
                                             double noise_multiplier, double sampling_rate,
                                             bool *out_exceeds);

/* ── 5. test-only call counter ────────────────────────────────────────────
 * Pinned by AC-42.1.4 / AC-42.1.5. Every successful call to
 * hu_dp_sgd_step() bumps a process-global counter when compiled with
 * HU_IS_TEST. Tests assert count > 0 when a backend trains under
 * dp_enabled=true, and count == 0 when dp_enabled=false. */
#ifdef HU_IS_TEST
uint64_t hu_dp_sgd_test_call_count(void);
void hu_dp_sgd_test_reset_call_count(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HU_ML_DP_SGD_H */
