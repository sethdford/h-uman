/* tests/test_grpo_loss.c — Phase 4 Task 3 (RL SOTA): GRPO loss math.
 *
 * Pins the four loss-only primitives in src/ml/grpo.c:
 *
 *   - hu_grpo_compute_advantages       — group-relative Â_i
 *   - hu_grpo_compute_clipped_advantage — PPO clip term (pessimistic min)
 *   - hu_grpo_compute_loss              — full GRPO loss
 *   - hu_grpo_compute_loss_grad_logp    — analytical ∂L/∂log_π_pol
 *
 * Round-3 critic fold-in pinned:
 *   D7 / R6   std=0 → zero advantages, no NaN division
 *   D8        log_ratio clamp to ±20 (no exp overflow)
 *   R8 / F3   PPO clip is PESSIMISTIC MIN, never optimistic max
 *   MED-1     kl_beta == 0 means KL DISABLED; kl_beta < 0 → default 0.04
 *   H4        finite-diff grad-check matches analytical within 5%
 *             relative magnitude (Phase 3 round-3 fix for the same
 *             primitive)
 *
 * NO model coupling — caller-owned arrays only.  Full GRPO trainer
 * lifecycle / sampling / backward integration tests land at Task 6
 * (tests/test_grpo.c) once Task 5 wires the hu_rl_trainer_t vtable.
 */
#include "test_framework.h"
#include "human/core/error.h"
#include "grpo_loss_priv.h"  /* via target_include_directories(human_tests) → src/ml */

#include <math.h>
#include <stddef.h>
#include <string.h>

/* --- 1. Advantage helper: std=0 (D7) --------------------------------- */
static void test_grpo_advantages_zero_when_all_rewards_equal(void) {
    /* D7 / R6: cold-start group where the synthetic reward fn fires on
     * no tokens.  All r_i equal → std = 0 → the divisor floors at
     * advantage_eps and the numerator is also 0, so every Â_i is
     * exactly 0.  No NaN, no Inf. */
    const double rewards[4] = {0.5, 0.5, 0.5, 0.5};
    double adv[4] = {1.0, 1.0, 1.0, 1.0};  /* poison to detect a no-op bug */

    hu_grpo_compute_advantages(rewards, 4, /*advantage_eps=*/1e-8, adv);

    for (size_t i = 0; i < 4; i++) {
        HU_ASSERT_TRUE(isfinite(adv[i]));
        HU_ASSERT_FLOAT_EQ(adv[i], 0.0, 1e-12);
    }
}

/* --- 2. Advantage helper: standardization (mean=0, pop-std=1) -------- */
static void test_grpo_advantages_standardize_to_mean_zero_unit_var(void) {
    /* With rewards {1, 2, 3, 4}: mean = 2.5, population std = sqrt(1.25)
     * ≈ 1.118.  Advantages standardize to mean(adv) = 0 and population
     * std(adv) = 1 (within double-precision numerical tolerance). */
    const double rewards[4] = {1.0, 2.0, 3.0, 4.0};
    double adv[4] = {0};

    hu_grpo_compute_advantages(rewards, 4, /*advantage_eps=*/1e-8, adv);

    double mean = 0.0;
    for (size_t i = 0; i < 4; i++) mean += adv[i];
    mean /= 4.0;
    HU_ASSERT_FLOAT_EQ(mean, 0.0, 1e-9);

    double sq = 0.0;
    for (size_t i = 0; i < 4; i++) {
        const double d = adv[i] - mean;
        sq += d * d;
    }
    const double pop_std = sqrt(sq / 4.0);
    HU_ASSERT_FLOAT_EQ(pop_std, 1.0, 1e-6);

    /* Direct spot-check: adv[0] = (1 - 2.5) / sqrt(1.25) = -1.5/sqrt(1.25). */
    const double expected_std = sqrt(1.25);
    HU_ASSERT_FLOAT_EQ(adv[0], -1.5 / expected_std, 1e-9);
    HU_ASSERT_FLOAT_EQ(adv[3], +1.5 / expected_std, 1e-9);
}

/* --- 3. Full loss + grad: std=0 group is graceful (D7 + R6) --------- */
static void test_grpo_loss_handles_zero_std_group_without_nan(void) {
    /* Cold-start: all rewards equal, all log-ratios zero.  Advantage
     * helper returns 0 for every Â_i.  Pessimistic clip min on (ratio=1,
     * adv=0) is 0.  Loss = 0 (policy) + 0·KL = 0.  Gradient = 0. */
    const double rewards[3] = {0.5, 0.5, 0.5};
    double advantages[3] = {0};
    hu_grpo_compute_advantages(rewards, 3, 1e-8, advantages);

    const double log_ratios[3] = {0.0, 0.0, 0.0};
    const double loss = hu_grpo_compute_loss(advantages, log_ratios, 3,
                                              /*clip_eps=*/0.2,
                                              /*kl_value=*/0.0,
                                              /*kl_beta=*/0.04);
    HU_ASSERT_TRUE(isfinite(loss));
    HU_ASSERT_FALSE(isnan(loss));
    HU_ASSERT_FLOAT_EQ(loss, 0.0, 1e-12);

    double grad[3] = {1.0, 1.0, 1.0};  /* poison */
    hu_grpo_compute_loss_grad_logp(advantages, log_ratios, 3, 0.2, grad);
    for (size_t i = 0; i < 3; i++) {
        HU_ASSERT_TRUE(isfinite(grad[i]));
        HU_ASSERT_FLOAT_EQ(grad[i], 0.0, 1e-12);
    }
}

/* --- 4. log_ratio clamp: ±50 does not overflow exp() (D8) ----------- */
static void test_grpo_loss_log_ratio_overflow_clamp_kicks_in(void) {
    /* Without the ±20 clamp, exp(+50) ≈ 5.2e21 (finite double, but ×
     * advantage becomes inf in normal arithmetic), exp(-50) ≈ 2e-22
     * (denormal-ish but still safe).  More importantly, exp(+1e6) is
     * inf, and inf · finite = inf → propagates to NaN downstream.
     * With the clamp, exp(±20) ≈ 4.85e8 / 2.06e-9, both well-conditioned. */
    const double advantages[2] = {1.0, 1.0};
    const double log_ratios[2] = {50.0, -50.0};
    const double loss = hu_grpo_compute_loss(advantages, log_ratios, 2,
                                              /*clip_eps=*/0.2,
                                              /*kl_value=*/0.0,
                                              /*kl_beta=*/0.0);
    HU_ASSERT_TRUE(isfinite(loss));
    HU_ASSERT_FALSE(isnan(loss));

    /* Extreme stress: ±1e6 (the F1/F2 failure-mode probe from the
     * umbrella spec).  Must still be finite. */
    const double log_ratios_extreme[2] = {1e6, -1e6};
    const double loss_extreme = hu_grpo_compute_loss(
        advantages, log_ratios_extreme, 2, 0.2, 0.0, 0.0);
    HU_ASSERT_TRUE(isfinite(loss_extreme));
    HU_ASSERT_FALSE(isnan(loss_extreme));
}

/* --- 5. Clip is pessimistic MIN, NOT optimistic max (R8 / F3) ------- */
static void test_grpo_loss_clip_is_pessimistic_min_not_max(void) {
    /* Case A — ratio above the upper bound, positive advantage:
     *   unclipped = 1.5 * 2.0 = 3.0
     *   clipped   = 1.2 * 2.0 = 2.4   (clip(1.5, 0.8, 1.2) = 1.2)
     *   min = 2.4  ← pessimistic clipped wins (penalizes over-shooting)
     *   (max would be 3.0; flipping to max accelerates reward hacking.) */
    const double a1 = hu_grpo_compute_clipped_advantage(
        /*ratio=*/1.5, /*advantage=*/2.0, /*clip_eps=*/0.2);
    HU_ASSERT_FLOAT_EQ(a1, 2.4, 1e-9);

    /* Case B — ratio above the upper bound, NEGATIVE advantage:
     *   unclipped = 1.5 * (-2.0) = -3.0
     *   clipped   = 1.2 * (-2.0) = -2.4
     *   min = -3.0  ← pessimistic UNclipped wins (more negative)
     *   (max would be -2.4; this is the case where the winning branch
     *   FLIPS with the sign of the advantage — the case-analysis the
     *   gradient logic must get right.) */
    const double a2 = hu_grpo_compute_clipped_advantage(1.5, -2.0, 0.2);
    HU_ASSERT_FLOAT_EQ(a2, -3.0, 1e-9);

    /* Case C — ratio below the lower bound, positive advantage:
     *   unclipped = 0.5 * 2.0 = 1.0
     *   clipped   = 0.8 * 2.0 = 1.6
     *   min = 1.0  ← unclipped wins (under-shooting yields the smaller
     *   surrogate; the clip would have given us undue credit). */
    const double a3 = hu_grpo_compute_clipped_advantage(0.5, 2.0, 0.2);
    HU_ASSERT_FLOAT_EQ(a3, 1.0, 1e-9);
}

/* --- 6. Clip is a no-op when ratio is in band (R8) ------------------ */
static void test_grpo_loss_clip_no_op_when_ratio_in_band(void) {
    /* clip_eps=0.2 → band [0.8, 1.2].  Ratios in [0.9, 1.1] are fully
     * inside, so clipped == unclipped and the min returns the same
     * value either way. */
    const double r1 = hu_grpo_compute_clipped_advantage(1.0, 0.5, 0.2);
    HU_ASSERT_FLOAT_EQ(r1, 0.5, 1e-12);

    const double r2 = hu_grpo_compute_clipped_advantage(1.1, 2.0, 0.2);
    HU_ASSERT_FLOAT_EQ(r2, 2.2, 1e-9);

    const double r3 = hu_grpo_compute_clipped_advantage(0.9, -1.5, 0.2);
    HU_ASSERT_FLOAT_EQ(r3, -1.35, 1e-9);

    /* Exactly on the boundary: ratio = 1.0 + clip_eps.  clip() returns
     * the same value, so the min is also a no-op. */
    const double r4 = hu_grpo_compute_clipped_advantage(1.2, 1.0, 0.2);
    HU_ASSERT_FLOAT_EQ(r4, 1.2, 1e-9);
}

/* --- 7. kl_beta = 0 means KL DISABLED (MED-1 escape valve) ---------- */
static void test_grpo_loss_kl_zero_when_kl_beta_disabled(void) {
    /* MED-1 contract: kl_beta == 0 must FULLY SKIP the KL term.
     * kl_value can be arbitrarily large (even +inf in principle), and
     * the loss must still equal the pure policy term. */
    const double advantages[2] = {0.5, -0.5};
    const double log_ratios[2] = {0.0, 0.0};

    const double loss_no_kl = hu_grpo_compute_loss(
        advantages, log_ratios, 2, 0.2,
        /*kl_value=*/1.0e9,   /* huge; would dominate the policy term
                               * if not skipped */
        /*kl_beta=*/0.0);     /* DISABLED */
    HU_ASSERT_TRUE(isfinite(loss_no_kl));

    /* Reference: same inputs with kl_value = 0 (KL trivially zero). */
    const double loss_kl_zero = hu_grpo_compute_loss(
        advantages, log_ratios, 2, 0.2, /*kl_value=*/0.0, /*kl_beta=*/0.04);
    HU_ASSERT_TRUE(isfinite(loss_kl_zero));

    /* Both branches reduce to the pure policy loss.  Equal within
     * numerical tolerance. */
    HU_ASSERT_FLOAT_EQ(loss_no_kl, loss_kl_zero, 1e-9);
}

/* --- 8. kl_beta < 0 selects the default 0.04 (MED-1) ---------------- */
static void test_grpo_loss_kl_negative_sentinel_uses_default_004(void) {
    /* kl_beta < 0 is the sentinel for "use the literature default 0.04"
     * (DeepSeek R1, umbrella §11 Q10).  Verify by passing -1 and
     * comparing against an explicit kl_beta = 0.04 call. */
    const double advantages[2] = {0.5, -0.5};
    const double log_ratios[2] = {0.0, 0.0};
    const double kl_value = 1.0;

    const double loss_neg = hu_grpo_compute_loss(
        advantages, log_ratios, 2, 0.2, kl_value, /*kl_beta=*/-1.0);

    const double loss_default = hu_grpo_compute_loss(
        advantages, log_ratios, 2, 0.2, kl_value, /*kl_beta=*/0.04);

    HU_ASSERT_TRUE(isfinite(loss_neg));
    HU_ASSERT_TRUE(isfinite(loss_default));
    HU_ASSERT_FLOAT_EQ(loss_neg, loss_default, 1e-12);

    /* Sanity: with kl_value = 1, the loss must be at least 0.04 above
     * the kl-disabled baseline (proves the term IS being applied, not
     * just that both calls return the same number). */
    const double loss_disabled = hu_grpo_compute_loss(
        advantages, log_ratios, 2, 0.2, kl_value, /*kl_beta=*/0.0);
    HU_ASSERT_FLOAT_EQ(loss_neg - loss_disabled, 0.04, 1e-9);
}

/* --- 9. Finite-diff grad-check on log_ratio[0] (H4) ----------------- */
static void test_grpo_loss_finite_diff_matches_analytical_on_log_ratio_probe(void) {
    /* H4 round-3 fix (Phase 3 precedent): the analytical gradient must
     * agree with the centered finite-difference probe within 5%
     * relative magnitude on a representative point.  KL term disabled
     * (kl_beta = 0) to isolate the policy gradient.
     *
     * Probe point chosen so log_ratio[0] = 0.05 → ratio ≈ 1.0513,
     * inside the trust region [0.8, 1.2] → un-clipped branch wins →
     * analytical gradient is non-zero (case (a) in the doc-block).
     */
    const double advantages[3] = { 0.5, -0.3,  0.7};
    double log_ratios[3]       = { 0.05, -0.05, 0.10};
    const double clip_eps = 0.2;
    const double kl_beta  = 0.0;
    const double kl_value = 0.0;

    /* Analytical gradient (caller-owned grad array). */
    double grad_analytical[3] = {0};
    hu_grpo_compute_loss_grad_logp(advantages, log_ratios, 3, clip_eps,
                                    grad_analytical);
    HU_ASSERT_TRUE(isfinite(grad_analytical[0]));

    /* Centered finite difference on log_ratio[0]:
     *   d L / d log_ratio[0] ≈ (L(+h) - L(-h)) / (2h)
     * h = 1e-4 is the sweet spot for double precision (smaller incurs
     * cancellation error, larger picks up second-order effects). */
    const double h = 1e-4;
    const double saved = log_ratios[0];

    log_ratios[0] = saved + h;
    const double loss_plus = hu_grpo_compute_loss(
        advantages, log_ratios, 3, clip_eps, kl_value, kl_beta);

    log_ratios[0] = saved - h;
    const double loss_minus = hu_grpo_compute_loss(
        advantages, log_ratios, 3, clip_eps, kl_value, kl_beta);

    log_ratios[0] = saved;
    const double grad_fd = (loss_plus - loss_minus) / (2.0 * h);
    HU_ASSERT_TRUE(isfinite(grad_fd));

    /* Relative-magnitude tolerance: |a - b| / max(|a|, eps) < 5%. */
    const double scale = fabs(grad_analytical[0]) > 1e-8
                             ? fabs(grad_analytical[0])
                             : 1e-8;
    const double rel_err = fabs(grad_analytical[0] - grad_fd) / scale;
    HU_ASSERT_TRUE(rel_err < 0.05);

    /* Sign check: both must be negative (case (a), Â_0 = 0.5 > 0,
     * ratio > 1 → grad = -ratio·Â/N < 0). */
    HU_ASSERT_TRUE(grad_analytical[0] < 0.0);
    HU_ASSERT_TRUE(grad_fd            < 0.0);
}

void run_grpo_loss_tests(void) {
    HU_TEST_SUITE("grpo_loss");
    HU_RUN_TEST(test_grpo_advantages_zero_when_all_rewards_equal);
    HU_RUN_TEST(test_grpo_advantages_standardize_to_mean_zero_unit_var);
    HU_RUN_TEST(test_grpo_loss_handles_zero_std_group_without_nan);
    HU_RUN_TEST(test_grpo_loss_log_ratio_overflow_clamp_kicks_in);
    HU_RUN_TEST(test_grpo_loss_clip_is_pessimistic_min_not_max);
    HU_RUN_TEST(test_grpo_loss_clip_no_op_when_ratio_in_band);
    HU_RUN_TEST(test_grpo_loss_kl_zero_when_kl_beta_disabled);
    HU_RUN_TEST(test_grpo_loss_kl_negative_sentinel_uses_default_004);
    HU_RUN_TEST(test_grpo_loss_finite_diff_matches_analytical_on_log_ratio_probe);
}
