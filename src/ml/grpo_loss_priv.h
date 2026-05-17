/* src/ml/grpo_loss_priv.h — Phase 4 Task 3 (RL SOTA), private.
 *
 * Pure-math helpers for the GRPO loss kernel (Group Relative Policy
 * Optimization, Shao et al. 2024 — DeepSeekMath §4.1.2):
 *
 *   1. hu_grpo_compute_advantages       — group-relative Â_i = (r_i - mean)/std
 *   2. hu_grpo_compute_clipped_advantage — pessimistic-min PPO clip term
 *   3. hu_grpo_compute_loss              — full GRPO loss (mean over group + β·KL)
 *   4. hu_grpo_compute_loss_grad_logp    — analytical ∂L/∂log_π_pol[i]
 *
 * PRIVATE header: NOT exported via include/.  Only src/ml/grpo.c and
 * tests/test_grpo_loss.c include it.  Public surface
 * (include/human/ml/grpo.h) stays factory-only.
 *
 * Numerical hardening contract — round-3 critic fold-in:
 *   D7 / R6  std=0 short-circuit (all-equal rewards → zero advantages,
 *            never NaN; division uses a floor at advantage_eps)
 *   D8       log_ratio clamp to ±20 (prevents exp() overflow before
 *            multiplying by advantage)
 *   R8 / F3  PPO clip is PESSIMISTIC MIN, never optimistic max
 *   MED-1    kl_beta == 0 means KL DISABLED (R4 escape valve;
 *            kl_value is IGNORED).  kl_beta < 0 selects the literature
 *            default 0.04 (DeepSeek R1; umbrella §11 Q10).
 *            kl_beta > 0 uses the literal value.
 *   H4       gradient on log_π_pol is the closed-form chain-rule
 *            result; finite-diff probe verifies within 5% relative
 *            magnitude (Phase 3 round-3 fix for the same primitive).
 */
#ifndef HU_ML_GRPO_LOSS_PRIV_H
#define HU_ML_GRPO_LOSS_PRIV_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Group-relative advantage:
 *   mean  = (1/N) Σ r_i
 *   std   = sqrt( (1/N) Σ (r_i - mean)^2 )       (population std, matching verl)
 *   denom = max(std, advantage_eps)              (D7/R6: never divide by 0)
 *   Â_i   = (r_i - mean) / denom
 *
 * D7/R6 guarantee: when std == 0 (all-equal rewards), numerator is also
 * 0, so all advantages come out exactly 0 — no NaN, no Inf.  This is
 * the cold-start case where the synthetic reward fn fires on no tokens.
 *
 * Caller OWNS rewards[n] and out_advantages[n].  n == 0 is a no-op.
 * advantage_eps must be > 0; non-positive values are coerced to 1e-8. */
void hu_grpo_compute_advantages(const double *rewards, size_t n,
                                double advantage_eps,
                                double *out_advantages);

/* PPO clip with PESSIMISTIC MIN (R8 / F3):
 *
 *   surr_unclipped = ratio * advantage
 *   surr_clipped   = clip(ratio, 1-ε, 1+ε) * advantage
 *   return min(surr_unclipped, surr_clipped)
 *
 * ALWAYS the smaller of the two — never optimistic max.  Flipping to
 * max accelerates reward hacking (umbrella §10 R9), so this contract
 * is pinned by test_grpo_loss_clip_is_pessimistic_min_not_max.
 *
 * The log_ratio → ratio clamp (D8) lives in hu_grpo_compute_loss; this
 * helper assumes its `ratio` argument is already finite. */
double hu_grpo_compute_clipped_advantage(double ratio, double advantage,
                                          double clip_eps);

/* Full GRPO loss:
 *
 *   ρ_i  = exp( clamp(log_ratios[i], -20, +20) )         (D8)
 *   L    = - (1/N) Σ min(ρ_i Â_i, clip(ρ_i, 1±ε) Â_i)
 *        + β_eff · kl_value
 *
 * β_eff sentinel:
 *   kl_beta <  0  → 0.04 (DeepSeek R1 default)
 *   kl_beta == 0  → 0    (KL DISABLED; kl_value is IGNORED — early-exit)
 *   kl_beta >  0  → kl_beta
 *
 * Caller OWNS advantages[n] and log_ratios[n].  Returns 0.0 on n == 0
 * or null inputs (graceful no-op for empty groups). */
double hu_grpo_compute_loss(const double *advantages,
                            const double *log_ratios,
                            size_t n,
                            double clip_eps,
                            double kl_value,
                            double kl_beta);

/* Analytical gradient ∂L/∂log_π_pol[i] for the PPO clip term:
 *
 * Two cases per the pessimistic min:
 *   (a) Un-clipped branch wins ⇒ ∂L/∂log_π_pol[i] = -(1/N) ρ_i Â_i
 *       (chain rule: ∂ρ_i/∂log_π_pol[i] = ρ_i, then negate the mean.)
 *   (b) Clipped branch wins   ⇒ ∂L/∂log_π_pol[i] = 0
 *       (clip() is locally constant inside the trust region.)
 *
 * Case discrimination is by SIGN COMPARISON of the two surrogates —
 * NOT by ratio band membership alone, since the winning branch flips
 * when advantage is negative (see test_grpo_loss_clip_is_pessimistic_*).
 *
 * NOTE: this is the policy-only gradient.  KL term gradient lives in
 * kl_divergence.c (k3 backward landed in Phase 4 Task 1).
 *
 * Caller OWNS all four arrays; n == 0 is a no-op. */
void hu_grpo_compute_loss_grad_logp(const double *advantages,
                                    const double *log_ratios,
                                    size_t n,
                                    double clip_eps,
                                    double *grad_logp_pol);

#ifdef __cplusplus
}
#endif
#endif /* HU_ML_GRPO_LOSS_PRIV_H */
