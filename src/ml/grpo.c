/* src/ml/grpo.c — Phase 4 Task 3 (RL SOTA): GRPO loss math.
 *
 * GRPO (Group Relative Policy Optimization, Shao et al. 2024 —
 * DeepSeekMath §4.1.2).  https://arxiv.org/abs/2402.03300
 *
 * THIS FILE — Task 3 scope: pure-math loss helpers only.
 *   - hu_grpo_compute_advantages
 *   - hu_grpo_compute_clipped_advantage
 *   - hu_grpo_compute_loss
 *   - hu_grpo_compute_loss_grad_logp
 * No model coupling.  No allocator.  No vtable.  Caller-owned arrays.
 *
 * Task 5 (forthcoming, sibling agent stream) replaces the bottom-of-
 * file placeholder for hu_grpo_huml_create with the real
 * hu_rl_trainer_t vtable impl (sampling via hu_rollout_t, scoring via
 * hu_reward_model_t, backward via hu_policy_logprobs).  The placeholder
 * here returns HU_ERR_NOT_SUPPORTED so the dispatcher in
 * src/ml/rl_trainer.c links cleanly through the Task 3 ↔ Task 5 gap.
 *
 * Conditional-compilation contract (round-3 critic fix L3 — NOT
 * __attribute__((weak)), strict C11 per AGENTS.md §3): defining
 * HU_GRPO_HAVE_HUML_IMPL here makes the Task 0 #ifndef stub in
 * src/ml/rl_trainer.c fall out of the link, leaving exactly one
 * definition of hu_grpo_huml_create per binary.
 *
 * Numerical hardening — round-3 critic fold-in (see grpo_loss_priv.h
 * doc-block for the full contract): D7/R6 std-floor, D8 log_ratio
 * clamp to ±20, R8/F3 pessimistic clip min, MED-1 kl_beta=0 disables
 * KL, H4 closed-form gradient with finite-diff agreement to 5%.
 *
 * Reference implementations consulted:
 *   - huggingface/trl/trainer/grpo_trainer.py
 *   - volcengine/verl/trainer/ppo/core_algos.py
 *       (compute_grpo_outcome_advantage; population std, not sample)
 *   - DeepSeek R1 paper §3.1.1 (β=0.04 default)
 */
#define HU_GRPO_HAVE_HUML_IMPL 1

#include "human/core/error.h"
#include "human/ml/rl_trainer.h"
#include "grpo_loss_priv.h"
#include <math.h>
#include <stddef.h>

/* Numerical hardening constants — see grpo_loss_priv.h doc-block. */
static const double HU_GRPO_LOG_RATIO_CLAMP = 20.0;  /* D8 */
static const double HU_GRPO_DEFAULT_KL_BETA = 0.04;  /* DeepSeek R1; MED-1 */

/* --- 1. Group-relative advantage -------------------------------------
 *
 * Population std (matching verl/core_algos.py — NOT sample std).  When
 * std collapses below advantage_eps (numerically zero, e.g. cold-start
 * group where every rollout earns the same reward), the floor at
 * advantage_eps keeps the divisor strictly positive.  In that case the
 * numerator is ALSO zero (r_i - mean ≈ 0 when all r_i ≈ mean), so the
 * advantages come out exactly 0 — D7/R6 guarantee, pinned by
 * test_grpo_advantages_zero_when_all_rewards_equal. */
void hu_grpo_compute_advantages(const double *rewards, size_t n,
                                double advantage_eps,
                                double *out_advantages) {
    if (!rewards || !out_advantages || n == 0) return;
    if (advantage_eps <= 0.0) advantage_eps = 1e-8;

    double sum = 0.0;
    for (size_t i = 0; i < n; i++) sum += rewards[i];
    const double mean = sum / (double)n;

    double sq = 0.0;
    for (size_t i = 0; i < n; i++) {
        const double d = rewards[i] - mean;
        sq += d * d;
    }
    const double std = sqrt(sq / (double)n);

    /* D7/R6: floor at advantage_eps.  When std is exactly 0 (or sub-
     * normal), denom = advantage_eps and the numerator is 0, so the
     * advantages are exactly 0 — never NaN. */
    const double denom = std < advantage_eps ? advantage_eps : std;
    for (size_t i = 0; i < n; i++) {
        out_advantages[i] = (rewards[i] - mean) / denom;
    }
}

/* --- 2. PPO clipped advantage (PESSIMISTIC MIN) ----------------------
 *
 * R8 / F3 contract: ALWAYS the smaller of the two surrogates.  Both
 * sign cases of the advantage need this — flipping to optimistic max
 * accelerates reward hacking (umbrella §10 R9).  Pinned by
 * test_grpo_loss_clip_is_pessimistic_min_not_max. */
double hu_grpo_compute_clipped_advantage(double ratio, double advantage,
                                          double clip_eps) {
    const double lo = 1.0 - clip_eps;
    const double hi = 1.0 + clip_eps;
    double clipped_ratio = ratio;
    if (clipped_ratio < lo) clipped_ratio = lo;
    if (clipped_ratio > hi) clipped_ratio = hi;

    const double surr_unclipped = ratio * advantage;
    const double surr_clipped   = clipped_ratio * advantage;
    return surr_unclipped < surr_clipped ? surr_unclipped : surr_clipped;
}

/* --- 3. Full GRPO loss -----------------------------------------------
 *
 *   L = -(1/N) Σ L_clip_i + β_eff · KL
 *
 * Where:
 *   - L_clip_i = min(ρ_i Â_i, clip(ρ_i, 1±ε) Â_i)        (R8/F3)
 *   - ρ_i      = exp(clamp(log_ratios[i], -20, +20))     (D8)
 *   - β_eff    sentinel: <0 → 0.04, ==0 → DISABLED, >0 → literal (MED-1)
 *
 * MED-1 early-exit: when kl_beta == 0 the KL term is fully skipped —
 * kl_value is never read.  Pinned by
 * test_grpo_loss_kl_zero_when_kl_beta_disabled. */
double hu_grpo_compute_loss(const double *advantages,
                            const double *log_ratios,
                            size_t n,
                            double clip_eps,
                            double kl_value,
                            double kl_beta) {
    if (!advantages || !log_ratios || n == 0) return 0.0;

    double sum_l_clip = 0.0;
    for (size_t i = 0; i < n; i++) {
        double lr = log_ratios[i];
        /* D8 clamp BEFORE exp() — prevents inf propagation. */
        if (lr >  HU_GRPO_LOG_RATIO_CLAMP) lr =  HU_GRPO_LOG_RATIO_CLAMP;
        if (lr < -HU_GRPO_LOG_RATIO_CLAMP) lr = -HU_GRPO_LOG_RATIO_CLAMP;
        const double ratio = exp(lr);
        sum_l_clip += hu_grpo_compute_clipped_advantage(ratio, advantages[i], clip_eps);
    }
    const double policy_loss = -(sum_l_clip / (double)n);

    /* MED-1 sentinel.  kl_beta == 0 returns policy_loss WITHOUT touching
     * kl_value — guarantee the test_grpo_loss_kl_zero_when_kl_beta_disabled
     * "kl_value can be huge" contract. */
    if (kl_beta == 0.0) {
        return policy_loss;
    }
    const double beta_eff = kl_beta < 0.0 ? HU_GRPO_DEFAULT_KL_BETA : kl_beta;
    return policy_loss + beta_eff * kl_value;
}

/* --- 4. Analytical gradient ∂L/∂log_π_pol[i] -------------------------
 *
 * L = -(1/N) Σ L_clip_i  (the KL term gradient lives in kl_divergence.c)
 * L_clip_i = min(ρ_i Â_i, clip(ρ_i, 1±ε) Â_i)
 *
 * Case (a) — un-clipped branch is the min (or ties):
 *     ρ_i = exp(log_π_pol[i] - log_π_old[i])
 *     ∂ρ_i / ∂log_π_pol[i] = ρ_i
 *     ∂L_clip_i / ∂log_π_pol[i] = ρ_i · Â_i
 *     ∂L / ∂log_π_pol[i] = -(1/N) · ρ_i · Â_i
 *
 * Case (b) — clipped branch is the min:
 *     clip(ρ_i, 1±ε) is locally constant ⇒ derivative is exactly 0.
 *
 * Case discrimination is by direct comparison of the two surrogate
 * values — NOT by ratio band membership alone.  When Â_i < 0, the
 * winning branch is the OPPOSITE of when Â_i > 0 (the pessimistic min
 * flips with sign), so the comparison is the only robust witness.
 *
 * Ties default to case (a) (the gradient is non-zero at exactly the
 * boundary), matching the test_grpo_loss_finite_diff_* convention that
 * the policy receives a non-zero gradient when ρ_i is in band. */
void hu_grpo_compute_loss_grad_logp(const double *advantages,
                                    const double *log_ratios,
                                    size_t n,
                                    double clip_eps,
                                    double *grad_logp_pol) {
    if (!advantages || !log_ratios || !grad_logp_pol || n == 0) return;
    const double lo = 1.0 - clip_eps;
    const double hi = 1.0 + clip_eps;
    const double inv_n = 1.0 / (double)n;

    for (size_t i = 0; i < n; i++) {
        double lr = log_ratios[i];
        if (lr >  HU_GRPO_LOG_RATIO_CLAMP) lr =  HU_GRPO_LOG_RATIO_CLAMP;
        if (lr < -HU_GRPO_LOG_RATIO_CLAMP) lr = -HU_GRPO_LOG_RATIO_CLAMP;
        const double ratio = exp(lr);

        double clipped_ratio = ratio;
        if (clipped_ratio < lo) clipped_ratio = lo;
        if (clipped_ratio > hi) clipped_ratio = hi;

        const double a = advantages[i];
        const double surr_unclipped = ratio * a;
        const double surr_clipped   = clipped_ratio * a;

        if (surr_unclipped <= surr_clipped) {
            /* Un-clipped branch wins (or ties on the boundary). */
            grad_logp_pol[i] = -inv_n * ratio * a;
        } else {
            /* Clipped branch wins ⇒ gradient is exactly 0. */
            grad_logp_pol[i] = 0.0;
        }
    }
}

/* --- Task 5 placeholder (real impl lands in Task 5) ------------------
 *
 * The Task 0 stub in src/ml/rl_trainer.c is gated on #ifndef
 * HU_GRPO_HAVE_HUML_IMPL.  Defining the macro at the top of THIS file
 * makes that stub fall out of the link; the strict C11 conditional-
 * compilation contract (round-3 critic L3) keeps exactly one external
 * definition of hu_grpo_huml_create per binary.
 *
 * Until Task 5 lands the real hu_rl_trainer_t vtable impl, this
 * placeholder returns HU_ERR_NOT_SUPPORTED.  Callers in
 * src/ml/rl_trainer.c (the kind-string dispatcher) propagate the error
 * verbatim — the dispatcher itself stays Task-5-shape-agnostic. */
hu_error_t hu_grpo_huml_create(hu_allocator_t *alloc,
                               const hu_rl_trainer_config_t *cfg,
                               hu_rl_trainer_t *out) {
    (void)alloc;
    (void)cfg;
    (void)out;
    return HU_ERR_NOT_SUPPORTED;
}
