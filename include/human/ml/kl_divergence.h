/* include/human/ml/kl_divergence.h — Phase 4 Task 1 (RL SOTA).
 *
 * Token-vocabulary KL divergence between a policy log-prob vector
 * `logp_pol[v]` and a frozen reference log-prob vector `logp_ref[v]`
 * at a single token position. Pure C leaf math primitive — no model
 * coupling, no allocator dependency, no global state. All four
 * entrypoints operate on caller-owned `double *` arrays.
 *
 * Three Schulman estimators (Schulman 2020, "Approximating KL
 * Divergence", http://joschu.net/blog/kl-approx.html):
 *
 *   k1: mean_i (logp_pol[i] - logp_ref[i])
 *       Biased low (E[k1] = -KL when sampled the other way). Can be
 *       negative. Useful as a debug signal / sanity probe.
 *
 *   k2: 0.5 * mean_i (logp_pol[i] - logp_ref[i])^2
 *       Always >= 0, biased estimator of KL. Has lower variance than
 *       k1 in many regimes but is not unbiased.
 *
 *   k3: mean_i (exp(r_i) - r_i - 1)   where r_i = logp_ref[i] - logp_pol[i]
 *       Unbiased AND always >= 0. PRIMARY estimator used by the GRPO
 *       trainer (Phase 4 Task 5) for the KL penalty term `beta * KL`.
 *       Used by trl, DeepSeek-Math/R1, and Schulman's original RLHF
 *       experiments.
 *
 * Round-3 fix H4 (per docs/plans/2026-05-11-rl-loop-phase-4-grpo.md):
 *   k3 forward is the MEAN over vocab (not sum) so that scale is
 *   independent of vocab size. The analytical backward MUST divide
 *   by v to stay consistent:
 *
 *     dKL_k3 / d(logp_pol[i]) = (1 - exp(r_i)) / v
 *
 * k1 and k2 are exported for future RL methods (DAPO, PPO+ref, sanity
 * probes) per AGENTS.md "vtable / extension point" reasoning; the
 * GRPO trainer itself only consumes k3.
 *
 * Edge cases: v == 0 writes 0 to *out_kl_mean and writes nothing to
 * grad_logp_pol (zero-length array, no NaN/Inf produced). NULL input
 * arrays are silently treated as v == 0 to avoid undefined behavior
 * if a caller forgets to allocate before calling. */

#ifndef HU_ML_KL_DIVERGENCE_H
#define HU_ML_KL_DIVERGENCE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* k1 estimator — mean(logp_pol[i] - logp_ref[i]).
 * BIASED. CAN BE NEGATIVE. Provided for debugging / future RL methods.
 * Writes the scalar mean to *out_kl_mean. */
void hu_kl_k1(const double *logp_pol, const double *logp_ref, size_t v,
              double *out_kl_mean);

/* k2 estimator — 0.5 * mean((logp_pol[i] - logp_ref[i])^2).
 * Biased but always >= 0. Provided for debugging / future RL methods.
 * Writes the scalar mean to *out_kl_mean. */
void hu_kl_k2(const double *logp_pol, const double *logp_ref, size_t v,
              double *out_kl_mean);

/* k3 Schulman unbiased estimator — mean(exp(r_i) - r_i - 1)
 * where r_i = logp_ref[i] - logp_pol[i]. Always >= 0. PRIMARY estimator
 * used by GRPO. Writes the scalar mean to *out_kl_mean. */
void hu_kl_k3(const double *logp_pol, const double *logp_ref, size_t v,
              double *out_kl_mean);

/* Analytical backward gradient of k3 w.r.t. logp_pol (the reference
 * is frozen, so dKL/d(logp_ref) = 0 and is not exposed):
 *
 *   grad_logp_pol[i] = (1 - exp(r_i)) / v   where r_i = logp_ref[i] - logp_pol[i]
 *
 * Divides by v to match the MEAN form of the forward (round-3 fix H4).
 * For v == 0 writes nothing (zero-length array). */
void hu_kl_k3_backward(const double *logp_pol, const double *logp_ref,
                       size_t v, double *grad_logp_pol);

#ifdef __cplusplus
}
#endif

#endif /* HU_ML_KL_DIVERGENCE_H */
