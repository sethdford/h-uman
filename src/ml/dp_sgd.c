/* US-8.1 — Real DP-SGD with RDP Accounting.
 *
 * Side-by-side module delivering correct primitives for DP-SGD:
 *   - per-sample gradient clipping (Abadi 2016 Alg. 1)
 *   - sub-sampled-Gaussian RDP accountant (Mironov 2017 §5;
 *     Mironov-Talwar-Zhang 2019 Theorem 4)
 *   - σ calibration to a target (ε, δ)
 *
 * Determinism: `hu_dp_sgd_step` consumes a stack-local splitmix64 PRNG; no
 * `rand()`, no `/dev/urandom`, no `time(NULL)`. Same (inputs, seed) ⇒ same
 * output bytes.
 *
 * See `include/human/ml/dp_sgd.h` for the public contract and
 * `sprints/sprint-8/designs/US-8.1.md` for the design rationale. */

#include "human/ml/dp_sgd.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef HU_ENABLE_ML

/* ---------- internal helpers (pure, static) ----------------------------- */

/* L2 norm of a vector of length n. */
static double dp_l2_norm(const double *v, size_t n) {
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) {
        acc += v[i] * v[i];
    }
    return sqrt(acc);
}

/* Numerically stable log-sum-exp for an array of N log-values. */
static double dp_logsumexp(const double *logvals, size_t n) {
    if (n == 0) {
        return -INFINITY;
    }
    double m = logvals[0];
    for (size_t i = 1; i < n; ++i) {
        if (logvals[i] > m) {
            m = logvals[i];
        }
    }
    if (!isfinite(m)) {
        return m;
    }
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) {
        s += exp(logvals[i] - m);
    }
    return m + log(s);
}

/* log(C(n,k)) via log-gamma. Integer α, k. */
static double dp_log_binomial(unsigned n, unsigned k) {
    return lgamma((double)n + 1.0) - lgamma((double)k + 1.0) - lgamma((double)(n - k) + 1.0);
}

/* RDP_α(σ, q) for the sub-sampled Gaussian mechanism (integer α >= 2).
 *
 * Mironov-Talwar-Zhang 2019, Theorem 4. The bound is
 *
 *   RDP_α(SGM(σ, q)) ≤ (1 / (α-1)) * log( sum_{k=0..α}
 *                       C(α, k) * (1-q)^(α-k) * q^k * exp(k(k-1) / (2σ²)) ).
 *
 * For q == 0 the result is α / (2σ²) (Gaussian mechanism without sub-sampling).
 * For q == 1 the result is also α / (2σ²) (no privacy amplification).
 * We evaluate the sum in log-space using log-sum-exp for stability when
 * k(k-1)/(2σ²) is large. */
static double dp_rdp_sampled_gaussian(unsigned alpha, double sigma, double q) {
    if (!(sigma > 0.0) || !(q >= 0.0 && q <= 1.0) || alpha < 2) {
        return NAN;
    }
    const double inv_2sigma_sq = 1.0 / (2.0 * sigma * sigma);
    /* No subsampling, or full-sampling: standard Gaussian RDP. */
    if (q <= 0.0 || q >= 1.0) {
        return (double)alpha * inv_2sigma_sq;
    }
    /* Build log-space terms. We cap α at a reasonable bound for stack use;
     * the public grid only reaches α = 64, so a small fixed buffer suffices. */
    double log_terms[HU_DP_RDP_ALPHA_MAX + 1];
    const double log_q = log(q);
    const double log_1mq = log1p(-q);
    for (unsigned k = 0; k <= alpha; ++k) {
        const double log_bin = dp_log_binomial(alpha, k);
        const double log_pow = (double)(alpha - k) * log_1mq + (double)k * log_q;
        const double exponent = (double)k * (double)(k - 1) * inv_2sigma_sq;
        log_terms[k] = log_bin + log_pow + exponent;
    }
    const double lse = dp_logsumexp(log_terms, (size_t)alpha + 1u);
    return lse / (double)(alpha - 1);
}

/* Convert a single RDP_α value to a (ε, δ)-DP ε at order α.
 *
 * Canonne, Kamath, Steinke 2020 (Proposition 12) sharpens the classical
 * Mironov 2017 Proposition 3:
 *
 *   ε(α) = RDP_α + log((α-1)/α) - (log δ + log α) / (α-1)
 *
 * which is tighter than the naive `RDP_α + log(1/δ) / (α-1)` by a constant.
 * We take min over α in the caller. */
static double dp_rdp_to_eps(double rdp_alpha_value, unsigned alpha, double delta) {
    if (!(delta > 0.0 && delta < 1.0) || alpha < 2 || !isfinite(rdp_alpha_value)) {
        return INFINITY;
    }
    const double a = (double)alpha;
    /* ε = RDP_α - (log δ + log α) / (α-1) + log((α-1)/α)
     *   = RDP_α + (- log δ - log α) / (α-1) + log(1 - 1/α)
     */
    return rdp_alpha_value + (-log(delta) - log(a)) / (a - 1.0) + log1p(-1.0 / a);
}

/* splitmix64 PRNG (deterministic, on-stack, platform-portable). */
static uint64_t dp_splitmix64_next(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Uniform double in (0, 1] from a 64-bit splitmix64 stream.
 * Excludes exact zero to keep `log(u)` well-defined in Box-Muller. */
static double dp_uniform01(uint64_t *state) {
    for (;;) {
        uint64_t r = dp_splitmix64_next(state);
        /* Use top 53 bits for a uniform in [0, 1). */
        double u = (double)(r >> 11) * (1.0 / 9007199254740992.0);
        if (u > 0.0) {
            return u;
        }
        /* exceedingly rare; loop to avoid log(0) */
    }
}

/* Standard-normal sample via Box-Muller. Consumes 2 PRNG draws per call. */
static double dp_standard_normal(uint64_t *state) {
    double u1 = dp_uniform01(state);
    double u2 = dp_uniform01(state);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

/* ---------- pure-predicate public API ----------------------------------- */

double hu_dp_rdp_epsilon_from_sigma(double sigma, double sample_rate, size_t steps, double delta) {
    if (!(sigma > 0.0) || !(sample_rate >= 0.0 && sample_rate <= 1.0) ||
        !(delta > 0.0 && delta < 1.0)) {
        return NAN;
    }
    if (steps == 0) {
        return 0.0;
    }
    double best_eps = INFINITY;
    for (unsigned alpha = HU_DP_RDP_ALPHA_MIN; alpha <= HU_DP_RDP_ALPHA_MAX; ++alpha) {
        const double rdp_one_step = dp_rdp_sampled_gaussian(alpha, sigma, sample_rate);
        if (!isfinite(rdp_one_step)) {
            continue;
        }
        const double rdp_total = rdp_one_step * (double)steps;
        const double eps = dp_rdp_to_eps(rdp_total, alpha, delta);
        if (eps < best_eps) {
            best_eps = eps;
        }
    }
    if (!isfinite(best_eps)) {
        return NAN;
    }
    if (best_eps < 0.0) {
        best_eps = 0.0;
    }
    return best_eps;
}

double hu_dp_sgd_noise_sigma(double clip_norm, double target_epsilon, double target_delta,
                             size_t steps, size_t dataset_size, double sample_rate) {
    (void)dataset_size; /* informational; not used in calibration */
    if (!(clip_norm > 0.0) || !(target_epsilon > 0.0) ||
        !(target_delta > 0.0 && target_delta < 1.0) ||
        !(sample_rate >= 0.0 && sample_rate <= 1.0) || steps == 0) {
        return NAN;
    }
    /* Binary search σ ∈ [σ_lo, σ_hi]. ε is monotone-decreasing in σ. */
    double lo = 0.1;
    double hi = 100.0;
    const double eps_at_hi = hu_dp_rdp_epsilon_from_sigma(hi, sample_rate, steps, target_delta);
    if (!isfinite(eps_at_hi) || eps_at_hi > target_epsilon) {
        /* Even at σ = 100 we cannot meet the budget — unsatisfiable. */
        return NAN;
    }
    const double eps_at_lo = hu_dp_rdp_epsilon_from_sigma(lo, sample_rate, steps, target_delta);
    if (isfinite(eps_at_lo) && eps_at_lo <= target_epsilon) {
        /* σ = 0.1 already fits the budget; return it. */
        return lo;
    }
    /* ε(σ_lo) > target > ε(σ_hi). Bisect. */
    for (int iter = 0; iter < 80; ++iter) {
        const double mid = 0.5 * (lo + hi);
        const double eps_mid = hu_dp_rdp_epsilon_from_sigma(mid, sample_rate, steps, target_delta);
        if (!isfinite(eps_mid)) {
            /* numerical issue; widen toward larger σ for safety. */
            lo = mid;
            continue;
        }
        if (eps_mid > target_epsilon) {
            lo = mid;
        } else {
            hi = mid;
        }
        if ((hi - lo) < 1e-4 * hi) {
            break;
        }
    }
    /* Return the upper end (always satisfies budget). */
    return hi;
}

/* ---------- DP-SGD step ------------------------------------------------- */

hu_error_t hu_dp_sgd_step(const double *per_sample_grads, size_t batch_size, size_t num_params,
                          double clip_norm, double sigma, uint64_t seed, double *out_aggregate) {
    if (per_sample_grads == NULL || out_aggregate == NULL || batch_size == 0 || num_params == 0 ||
        !(clip_norm > 0.0) || !(sigma >= 0.0)) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Zero the output. */
    for (size_t j = 0; j < num_params; ++j) {
        out_aggregate[j] = 0.0;
    }

    /* Per-sample (per-row) clip + sum. Each row is scaled by
     * min(1, clip_norm / ||row||) before adding into the aggregate. */
    for (size_t i = 0; i < batch_size; ++i) {
        const double *row = per_sample_grads + i * num_params;
        const double norm = dp_l2_norm(row, num_params);
        double scale = 1.0;
        if (norm > clip_norm) {
            scale = clip_norm / norm;
        }
        for (size_t j = 0; j < num_params; ++j) {
            out_aggregate[j] += row[j] * scale;
        }
    }

    /* Gaussian noise with std = sigma * clip_norm. */
    if (sigma > 0.0) {
        const double noise_std = sigma * clip_norm;
        uint64_t prng_state = seed ^ 0xDEADBEEFCAFEBABEULL;
        /* Pre-mix the state so seed == 0 doesn't produce a near-zero first draw. */
        (void)dp_splitmix64_next(&prng_state);
        for (size_t j = 0; j < num_params; ++j) {
            out_aggregate[j] += noise_std * dp_standard_normal(&prng_state);
        }
    }
    return HU_OK;
}

/* ---------- RDP accountant lifecycle ------------------------------------ */

void hu_dp_accountant_rdp_init(hu_dp_rdp_accountant_t *acct) {
    if (acct == NULL) {
        return;
    }
    memset(acct, 0, sizeof(*acct));
}

hu_error_t hu_dp_accountant_rdp_record(hu_dp_rdp_accountant_t *acct, double sigma,
                                       double sample_rate) {
    if (acct == NULL || !(sigma > 0.0) || !(sample_rate >= 0.0 && sample_rate <= 1.0)) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (acct->event_count >= HU_DP_RDP_MAX_EVENTS) {
        return HU_ERR_LIMIT_REACHED;
    }
    acct->events[acct->event_count].sigma = sigma;
    acct->events[acct->event_count].sample_rate = sample_rate;
    acct->event_count++;
    /* Incremental update: rdp_alpha[i] += RDP_α(σ, q) (events compose additively
     * across the α-axis per Mironov 2017 §4 Proposition 1). */
    for (unsigned alpha = HU_DP_RDP_ALPHA_MIN; alpha <= HU_DP_RDP_ALPHA_MAX; ++alpha) {
        const double rdp = dp_rdp_sampled_gaussian(alpha, sigma, sample_rate);
        if (isfinite(rdp)) {
            acct->rdp_alpha[alpha - HU_DP_RDP_ALPHA_MIN] += rdp;
        }
    }
    return HU_OK;
}

double hu_dp_accountant_rdp_epsilon(const hu_dp_rdp_accountant_t *acct, double delta) {
    if (acct == NULL || !(delta > 0.0 && delta < 1.0)) {
        return NAN;
    }
    if (acct->event_count == 0) {
        return 0.0;
    }
    double best_eps = INFINITY;
    for (unsigned alpha = HU_DP_RDP_ALPHA_MIN; alpha <= HU_DP_RDP_ALPHA_MAX; ++alpha) {
        const double rdp_total = acct->rdp_alpha[alpha - HU_DP_RDP_ALPHA_MIN];
        const double eps = dp_rdp_to_eps(rdp_total, alpha, delta);
        if (eps < best_eps) {
            best_eps = eps;
        }
    }
    if (!isfinite(best_eps)) {
        return NAN;
    }
    if (best_eps < 0.0) {
        best_eps = 0.0;
    }
    return best_eps;
}

#else /* !HU_ENABLE_ML */

/* Provide weak no-op stubs so the symbol set is stable across build configs.
 * Tests run only under HU_ENABLE_ML so these stubs are exercised only at
 * link-time on minimal builds. */

double hu_dp_sgd_noise_sigma(double clip_norm, double target_epsilon, double target_delta,
                             size_t steps, size_t dataset_size, double sample_rate) {
    (void)clip_norm;
    (void)target_epsilon;
    (void)target_delta;
    (void)steps;
    (void)dataset_size;
    (void)sample_rate;
    return NAN;
}

double hu_dp_rdp_epsilon_from_sigma(double sigma, double sample_rate, size_t steps, double delta) {
    (void)sigma;
    (void)sample_rate;
    (void)steps;
    (void)delta;
    return NAN;
}

hu_error_t hu_dp_sgd_step(const double *per_sample_grads, size_t batch_size, size_t num_params,
                          double clip_norm, double sigma, uint64_t seed, double *out_aggregate) {
    (void)per_sample_grads;
    (void)batch_size;
    (void)num_params;
    (void)clip_norm;
    (void)sigma;
    (void)seed;
    (void)out_aggregate;
    return HU_ERR_NOT_SUPPORTED;
}

void hu_dp_accountant_rdp_init(hu_dp_rdp_accountant_t *acct) {
    if (acct != NULL) {
        memset(acct, 0, sizeof(*acct));
    }
}

hu_error_t hu_dp_accountant_rdp_record(hu_dp_rdp_accountant_t *acct, double sigma,
                                       double sample_rate) {
    (void)acct;
    (void)sigma;
    (void)sample_rate;
    return HU_ERR_NOT_SUPPORTED;
}

double hu_dp_accountant_rdp_epsilon(const hu_dp_rdp_accountant_t *acct, double delta) {
    (void)acct;
    (void)delta;
    return NAN;
}

#endif /* HU_ENABLE_ML */
