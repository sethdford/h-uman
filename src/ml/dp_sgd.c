/* Sprint 42 / US-42.1 — Canonical DP-SGD implementation.
 *
 * SOLE implementation of clip+noise math for h-uman. All three learner
 * backends (cpu / ggml / mlx) MUST delegate here.
 *
 * Math references:
 *   - Mironov, Talwar, Zhang 2019 — "Rényi Differential Privacy of the
 *     Sampled Gaussian Mechanism", Theorem 4 (subsampled-Gaussian RDP).
 *   - Canonne, Kamath, Steinke 2020 — "The Discrete Gaussian for DP",
 *     Proposition 12 (RDP → (eps, delta) conversion).
 *   - Opacus 1.5.x `RDPAccountant` — the reference implementation we
 *     pin against in the oracle fixture (tests/fixtures/dp_accountant_oracle.json).
 *
 * Numerical hygiene:
 *   - All accumulations in log-space using logsumexp.
 *   - Monotonicity break: if epsilon at alpha_{k+1} exceeds epsilon at
 *     alpha_k by a margin, we early-exit since the envelope is convex in
 *     alpha for the subsampled-Gaussian mechanism (per MTZ 2019).
 *   - Boundary warn: if argmin alpha lands on grid edge (256), the true
 *     optimum may lie beyond — caller's responsibility to widen the grid.
 */

#include "human/ml/dp_sgd.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HU_IS_TEST
#include <stdatomic.h>
static _Atomic uint64_t g_dp_call_count = 0;
uint64_t hu_dp_sgd_test_call_count(void) {
    return atomic_load_explicit(&g_dp_call_count, memory_order_relaxed);
}
void hu_dp_sgd_test_reset_call_count(void) {
    atomic_store_explicit(&g_dp_call_count, 0, memory_order_relaxed);
}
#endif

/* ── PRNG (splitmix64 + Box-Muller) ──────────────────────────────────────── */

static uint64_t dp_splitmix64(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static double dp_uniform_open(uint64_t *state) {
    /* (0, 1] — never returns 0 so log() is safe. */
    uint64_t r = dp_splitmix64(state);
    /* Take 53 bits, +1 to avoid 0. */
    double u = (double)((r >> 11) + 1) / (double)((1ULL << 53) + 1);
    return u;
}

void hu_dp_rng_seed(hu_dp_rng_t *r, uint64_t seed) {
    if (!r) {
        return;
    }
    /* Allow seed=0 — splitmix64 mixes anyway. Salt with a non-trivial
     * constant so two callers with the same low-entropy seed don't end up
     * generating identical streams. */
    r->state = seed ^ 0xD1B54A32D192ED03ULL;
}

double hu_dp_rng_normal(hu_dp_rng_t *r) {
    if (!r) {
        return 0.0;
    }
    /* Box-Muller; discard the second sample so the call sequence is
     * byte-identical regardless of how the caller batches. */
    double u1 = dp_uniform_open(&r->state);
    double u2 = dp_uniform_open(&r->state);
    double mag = sqrt(-2.0 * log(u1));
    return mag * cos(6.283185307179586 * u2);
}

/* ── 1. noise sigma ─────────────────────────────────────────────────────── */

double hu_dp_sgd_noise_sigma(double clip_norm, double noise_multiplier) {
    if (!(clip_norm > 0.0) || !(noise_multiplier > 0.0)) {
        return 0.0;
    }
    if (!isfinite(clip_norm) || !isfinite(noise_multiplier)) {
        return 0.0;
    }
    return clip_norm * noise_multiplier;
}

/* ── 2. RDP closed-form for subsampled Gaussian ──────────────────────────── */

/* logsumexp of two log-domain values. */
static double log_add(double log_a, double log_b) {
    if (!isfinite(log_a)) {
        return log_b;
    }
    if (!isfinite(log_b)) {
        return log_a;
    }
    double m = (log_a > log_b) ? log_a : log_b;
    return m + log1p(exp(-fabs(log_a - log_b)));
}

/* Lanczos approximation of log(Gamma(x+1)) for x >= 0. We use lgamma() from
 * <math.h> when available; C11 guarantees it. */
static double log_factorial(unsigned int n) {
    /* lgamma(n+1) == log(n!). */
    return lgamma((double)n + 1.0);
}

/* log of binomial coefficient C(alpha, k) for INTEGER alpha. */
static double log_binom(unsigned int alpha, unsigned int k) {
    if (k > alpha) {
        return -INFINITY;
    }
    return log_factorial(alpha) - log_factorial(k) - log_factorial(alpha - k);
}

/* RDP of the subsampled Gaussian mechanism at integer order alpha.
 *
 * MTZ 2019 Theorem 4 / Opacus _compute_log_a_int formulation:
 *
 *   exp((alpha-1) * D_alpha(SGM)) =
 *     sum_{k=0}^{alpha} C(alpha, k) * (1-q)^(alpha-k) * q^k *
 *                       exp((k^2 - k) / (2 sigma^2))
 *
 *   D_alpha(SGM) = (1/(alpha-1)) * log( above sum )
 *
 * We compute the sum in log-space using logsumexp over k. Returns the RDP
 * value (not multiplied by steps).
 */
static double rdp_subsampled_gaussian_int(double q, double sigma, unsigned int alpha) {
    if (q <= 0.0 || alpha < 2) {
        return 0.0;
    }
    if (q >= 1.0) {
        /* Non-subsampled Gaussian: D_alpha = alpha / (2 sigma^2) */
        return (double)alpha / (2.0 * sigma * sigma);
    }
    double log_q = log(q);
    double log_1mq = log1p(-q);
    double log_sum = -INFINITY;
    /* Sum over k = 0..alpha. */
    for (unsigned int k = 0; k <= alpha; k++) {
        double log_coeff = log_binom(alpha, k);
        double log_q_term = (double)k * log_q;
        double log_1mq_term = (double)(alpha - k) * log_1mq;
        /* k(k-1) / (2 sigma^2) — the "tilted" Gaussian moment. */
        double tilt = ((double)k * ((double)k - 1.0)) / (2.0 * sigma * sigma);
        double log_term = log_coeff + log_q_term + log_1mq_term + tilt;
        log_sum = log_add(log_sum, log_term);
    }
    /* D_alpha = log_sum / (alpha - 1). */
    return log_sum / ((double)alpha - 1.0);
}

/* CKS 2020 Proposition 12: convert RDP value at order alpha into (eps, delta)
 * via:
 *
 *   eps = rdp_alpha + log( (alpha - 1) / alpha )
 *                   - (log(delta) + log(alpha)) / (alpha - 1)
 *
 * This is the tightest known conversion. (For comparison, the original
 * Mironov 2017 conversion uses `log(1/delta) / (alpha - 1)` which is
 * looser by `log(alpha) / (alpha - 1) - log((alpha-1)/alpha)`.)
 */
static double rdp_to_eps(double rdp_alpha, double alpha, double delta) {
    double log_term = log((alpha - 1.0) / alpha);
    double penalty = (log(alpha) - log(delta)) / (alpha - 1.0);
    return rdp_alpha + log_term + penalty;
}

/* Alpha grid: {2, 4, 8, 16, 32, 64, 128, 256}. See design doc for the
 * sparsity-vs-tightness justification (max ~0.02 epsilon error vs the dense
 * Opacus grid on the oracle fixture). */
static const unsigned int kAlphaGrid[] = {2, 4, 8, 16, 32, 64, 128, 256};
static const size_t kAlphaGridLen = sizeof(kAlphaGrid) / sizeof(kAlphaGrid[0]);

hu_error_t hu_dp_rdp_epsilon_from_sigma(double noise_multiplier, double sampling_rate,
                                        uint64_t steps, double delta, double *out_epsilon,
                                        double *out_argmin_alpha) {
    if (!out_epsilon) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    *out_epsilon = 0.0;
    if (out_argmin_alpha) {
        *out_argmin_alpha = 0.0;
    }
    if (!isfinite(noise_multiplier) || noise_multiplier <= 0.0) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (!isfinite(sampling_rate) || sampling_rate <= 0.0 || sampling_rate > 1.0) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (!isfinite(delta) || delta <= 0.0 || delta >= 1.0) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (steps == 0) {
        *out_epsilon = 0.0;
        if (out_argmin_alpha) {
            *out_argmin_alpha = (double)kAlphaGrid[0];
        }
        return HU_OK;
    }

    double best_eps = INFINITY;
    double best_alpha = (double)kAlphaGrid[0];
    double prev_eps = INFINITY;
    int rising_count = 0;
    for (size_t i = 0; i < kAlphaGridLen; i++) {
        unsigned int alpha = kAlphaGrid[i];
        double rdp_step = rdp_subsampled_gaussian_int(sampling_rate, noise_multiplier, alpha);
        /* Composition: T identical steps in RDP space => multiply by T. */
        double rdp_total = rdp_step * (double)steps;
        double eps_alpha = rdp_to_eps(rdp_total, (double)alpha, delta);
        if (eps_alpha < best_eps) {
            best_eps = eps_alpha;
            best_alpha = (double)alpha;
        }
        /* Monotonicity break — if epsilon has been rising for 2 consecutive
         * grid points past the argmin, we're past the convex minimum. */
        if (eps_alpha > prev_eps) {
            rising_count++;
            if (rising_count >= 2) {
                break;
            }
        } else {
            rising_count = 0;
        }
        prev_eps = eps_alpha;
    }

    /* Guard against numerical degeneracy producing -INFINITY / NaN. */
    if (!isfinite(best_eps) || best_eps < 0.0) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    *out_epsilon = best_eps;
    if (out_argmin_alpha) {
        *out_argmin_alpha = best_alpha;
    }
    /* Boundary warn: if argmin sits at the grid edge AND we never rose, the
     * true optimum may lie beyond 256. Surface via stderr so users see the
     * "may need wider grid" signal. */
    if ((unsigned int)best_alpha == kAlphaGrid[kAlphaGridLen - 1] && rising_count == 0) {
        fprintf(stderr,
                "[dp_sgd] warning: RDP argmin alpha=%u at grid edge — "
                "reported epsilon may under-estimate true privacy cost; "
                "consider widening the alpha grid.\n",
                (unsigned int)best_alpha);
    }
    return HU_OK;
}

/* ── 3. per-sample clip + noise + average ────────────────────────────────── */

hu_error_t hu_dp_sgd_step(const float *per_sample_grads, size_t n_samples, size_t dim,
                          double clip_norm, double noise_multiplier, hu_dp_rng_t *rng,
                          float *out_grad) {
    if (!out_grad || dim == 0 || n_samples == 0) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (!per_sample_grads) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    /* Adversarial AC: caller bypassed clipping. */
    if (!(clip_norm > 0.0) || !isfinite(clip_norm)) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (!isfinite(noise_multiplier) || noise_multiplier < 0.0) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (noise_multiplier > 0.0 && !rng) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Zero the output accumulator. */
    for (size_t d = 0; d < dim; d++) {
        out_grad[d] = 0.0f;
    }

    /* For each sample: compute its L2 norm, derive the clip scale, then
     * accumulate the clipped vector into out_grad. This is the "true"
     * per-sample clipping the design's R-CURRENT-IS-PER-BATCH risk
     * called out — the previous learner_cpu.c code clipped one summed
     * vector instead. */
    for (size_t s = 0; s < n_samples; s++) {
        const float *row = per_sample_grads + s * dim;
        double sq = 0.0;
        for (size_t d = 0; d < dim; d++) {
            sq += (double)row[d] * (double)row[d];
        }
        double norm = sqrt(sq);
        double scale = 1.0;
        if (norm > clip_norm) {
            scale = clip_norm / norm;
        }
        for (size_t d = 0; d < dim; d++) {
            out_grad[d] += (float)((double)row[d] * scale);
        }
    }

    /* Add Gaussian noise to the SUM. Each dim is an independent N(0, sigma^2)
     * draw. sigma = noise_multiplier * clip_norm. */
    double sigma = hu_dp_sgd_noise_sigma(clip_norm, noise_multiplier);
    if (sigma > 0.0) {
        for (size_t d = 0; d < dim; d++) {
            double z = hu_dp_rng_normal(rng);
            out_grad[d] += (float)(z * sigma);
        }
    }

    /* Average across the batch — divide the noised sum by n_samples to
     * recover the per-sample mean. */
    double inv_n = 1.0 / (double)n_samples;
    for (size_t d = 0; d < dim; d++) {
        out_grad[d] = (float)((double)out_grad[d] * inv_n);
    }

#ifdef HU_IS_TEST
    atomic_fetch_add_explicit(&g_dp_call_count, 1ULL, memory_order_relaxed);
#endif
    return HU_OK;
}

/* ── 4. RDP accountant ───────────────────────────────────────────────────── */

void hu_dp_rdp_accountant_init(hu_dp_rdp_accountant_t *a, double delta, double epsilon_budget) {
    if (!a) {
        return;
    }
    memset(a, 0, sizeof(*a));
    a->delta = (isfinite(delta) && delta > 0.0 && delta < 1.0) ? delta : 1e-5;
    a->epsilon_budget = isfinite(epsilon_budget) ? epsilon_budget : 0.0;
}

/* Find an existing bucket with matching (noise_multiplier, sampling_rate) up
 * to a small tolerance, or NULL if none. */
static hu_dp_rdp_bucket_t *find_bucket(hu_dp_rdp_accountant_t *a, double noise_multiplier,
                                       double sampling_rate) {
    for (int i = 0; i < a->bucket_count; i++) {
        double dnm = fabs(a->buckets[i].noise_multiplier - noise_multiplier);
        double dq = fabs(a->buckets[i].sampling_rate - sampling_rate);
        if (dnm < 1e-9 && dq < 1e-12) {
            return &a->buckets[i];
        }
    }
    return NULL;
}

hu_error_t hu_dp_rdp_accountant_step(hu_dp_rdp_accountant_t *a, double noise_multiplier,
                                     double sampling_rate) {
    if (!a) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (!isfinite(noise_multiplier) || noise_multiplier <= 0.0) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (!isfinite(sampling_rate) || sampling_rate <= 0.0 || sampling_rate > 1.0) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    hu_dp_rdp_bucket_t *b = find_bucket(a, noise_multiplier, sampling_rate);
    if (b) {
        b->steps++;
        return HU_OK;
    }
    if (a->bucket_count >= HU_DP_RDP_ACCOUNTANT_MAX_BUCKETS) {
        /* Cap reached: fold into the closest existing bucket to keep the
         * accountant bounded. This is an UNSAFE approximation; emit a warn. */
        fprintf(stderr,
                "[dp_sgd] warning: RDP accountant bucket cap %d reached; "
                "extra (sigma=%.4f, q=%.4g) folded into bucket 0.\n",
                HU_DP_RDP_ACCOUNTANT_MAX_BUCKETS, noise_multiplier, sampling_rate);
        a->buckets[0].steps++;
        return HU_OK;
    }
    a->buckets[a->bucket_count].noise_multiplier = noise_multiplier;
    a->buckets[a->bucket_count].sampling_rate = sampling_rate;
    a->buckets[a->bucket_count].steps = 1;
    a->bucket_count++;
    return HU_OK;
}

hu_error_t hu_dp_rdp_accountant_epsilon(const hu_dp_rdp_accountant_t *a, double *out_epsilon) {
    if (!a || !out_epsilon) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    *out_epsilon = 0.0;
    /* Parallel composition across buckets: sum epsilons. Within a bucket,
     * additive composition in RDP space is folded into the per-bucket
     * `steps`. */
    double total = 0.0;
    for (int i = 0; i < a->bucket_count; i++) {
        if (a->buckets[i].steps == 0) {
            continue;
        }
        double eps_i = 0.0;
        hu_error_t e = hu_dp_rdp_epsilon_from_sigma(a->buckets[i].noise_multiplier,
                                                    a->buckets[i].sampling_rate,
                                                    a->buckets[i].steps, a->delta, &eps_i, NULL);
        if (e != HU_OK) {
            return e;
        }
        total += eps_i;
    }
    *out_epsilon = total;
    return HU_OK;
}

hu_error_t hu_dp_rdp_accountant_would_exceed(const hu_dp_rdp_accountant_t *a,
                                             double noise_multiplier, double sampling_rate,
                                             bool *out_exceeds) {
    if (!a || !out_exceeds) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    *out_exceeds = false;
    if (!(a->epsilon_budget > 0.0)) {
        return HU_OK; /* No cap. */
    }
    /* Build a scratch accountant + the hypothetical step. */
    hu_dp_rdp_accountant_t scratch = *a;
    hu_error_t e = hu_dp_rdp_accountant_step(&scratch, noise_multiplier, sampling_rate);
    if (e != HU_OK) {
        return e;
    }
    double eps = 0.0;
    e = hu_dp_rdp_accountant_epsilon(&scratch, &eps);
    if (e != HU_OK) {
        return e;
    }
    *out_exceeds = (eps > a->epsilon_budget);
    return HU_OK;
}
