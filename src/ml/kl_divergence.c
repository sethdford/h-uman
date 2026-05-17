/* src/ml/kl_divergence.c — Phase 4 Task 1 (RL SOTA).
 *
 * Schulman 2020 k1/k2/k3 KL-divergence estimators + analytical k3
 * backward gradient. Pure C leaf math primitive — no model coupling,
 * no allocator dependency, no global state. See kl_divergence.h for
 * the API contract and Schulman's derivation summary.
 *
 * All forward estimators return the MEAN over vocab so the scale is
 * independent of vocab size. The k3 backward divides by v for
 * consistency (round-3 fix H4 per Phase 4 plan).
 *
 * safe_exp() clamps the exponent to [-700, +700] before calling exp()
 * so an adversarial drift between policy and reference cannot produce
 * NaN/Inf inside the GRPO loss (where this primitive is the inner
 * call). The clamp is wider than any plausible logit drift; tight
 * enough to keep exp() in the IEEE-754 normalized range. */

#include "human/ml/kl_divergence.h"

#include <math.h>
#include <stddef.h>

/* Clamp exponent to [-700, +700] to keep exp() in IEEE-754 normalized
 * range. exp(709.78...) overflows to +Inf in double; exp(-745) flushes
 * to zero. The clamp is loose enough that it never fires for
 * well-behaved log-prob differences (typical |r| < 20) and tight
 * enough to keep the result finite for adversarial inputs. */
static inline double safe_exp(double x) {
    if (x > 700.0) {
        x = 700.0;
    } else if (x < -700.0) {
        x = -700.0;
    }
    return exp(x);
}

void hu_kl_k1(const double *logp_pol, const double *logp_ref, size_t v,
              double *out_kl_mean) {
    if (!out_kl_mean) {
        return;
    }
    if (!logp_pol || !logp_ref || v == 0) {
        *out_kl_mean = 0.0;
        return;
    }
    double acc = 0.0;
    for (size_t i = 0; i < v; ++i) {
        acc += logp_pol[i] - logp_ref[i];
    }
    *out_kl_mean = acc / (double)v;
}

void hu_kl_k2(const double *logp_pol, const double *logp_ref, size_t v,
              double *out_kl_mean) {
    if (!out_kl_mean) {
        return;
    }
    if (!logp_pol || !logp_ref || v == 0) {
        *out_kl_mean = 0.0;
        return;
    }
    double acc = 0.0;
    for (size_t i = 0; i < v; ++i) {
        double d = logp_pol[i] - logp_ref[i];
        acc += d * d;
    }
    *out_kl_mean = 0.5 * (acc / (double)v);
}

void hu_kl_k3(const double *logp_pol, const double *logp_ref, size_t v,
              double *out_kl_mean) {
    if (!out_kl_mean) {
        return;
    }
    if (!logp_pol || !logp_ref || v == 0) {
        *out_kl_mean = 0.0;
        return;
    }
    double acc = 0.0;
    for (size_t i = 0; i < v; ++i) {
        double r = logp_ref[i] - logp_pol[i];
        acc += safe_exp(r) - r - 1.0;
    }
    *out_kl_mean = acc / (double)v;
}

void hu_kl_k3_backward(const double *logp_pol, const double *logp_ref,
                       size_t v, double *grad_logp_pol) {
    if (!grad_logp_pol || !logp_pol || !logp_ref || v == 0) {
        return;
    }
    double inv_v = 1.0 / (double)v;
    for (size_t i = 0; i < v; ++i) {
        double r = logp_ref[i] - logp_pol[i];
        grad_logp_pol[i] = (1.0 - safe_exp(r)) * inv_v;
    }
}
