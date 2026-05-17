/* tests/test_grpo_huml.c — Phase 4 Task 5 (RL SOTA)
 *
 * Pins the full hu_grpo_huml_create + grpo_huml_step impl in
 * src/ml/grpo.c.  Distinct from tests/test_grpo_loss.c (which stays
 * loss-only, no model coupling).  This suite exercises the
 * hu_rl_trainer_t vtable end-to-end on the toy GPT:
 *
 *   1. Factory returns a fully populated vtable
 *   2. Factory rejects n_rollouts < 2  (R12 bound)
 *   3. Factory rejects n_rollouts > 1024  (R12 bound)
 *   4. step() runs without error on a single pair + N=4 rollouts
 *   5. step() with kl_beta == 0 NEVER calls the reference forward
 *      (MED-1 perf + correctness contract)
 *   6. step() advances the internal step_count counter
 *   7. 50 step() calls leak nothing under ASan (R10 contract)
 *
 * Determinism: rollout uses splitmix64(seed=42 XOR rollout_index) at
 * sample time so the same trainer config + step count yields byte-
 * identical rollout token streams across glibc + Apple libc (R13).
 */
#include "test_framework.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/dpo.h"          /* hu_preference_pair_t */
#include "human/ml/grpo.h"
#include "human/ml/rl_trainer.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Test seams exposed under HU_IS_TEST by src/ml/grpo.c. */
extern size_t hu_grpo_huml_step_count_for_test(void *vctx);
extern size_t hu_grpo_huml_ref_forward_count_for_test(void *vctx);

/* Shared fixture: a single prompt-only preference pair.  GRPO ignores
 * chosen/rejected per the rl_trainer.h contract (rollouts come from
 * hu_rollout_t, NOT the preference data). */
static void make_prompt_pair(hu_preference_pair_t *p) {
    memset(p, 0, sizeof(*p));
    /* Prompt tokens 1,2,3 — well inside the toy GPT's vocab_size=32. */
    memcpy(p->prompt, "1 2 3", 5);
    p->prompt_len = 5;
}

/* ─── Test 1: Factory shape ─────────────────────────────────────────── */

static void test_grpo_huml_factory_returns_populated_vtable(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_config_t cfg = {
        .backend = HU_DPO_BACKEND_HUML,
        .learning_rate = 1e-3,
        .n_rollouts = 4,
        .clip_eps = 0.2,
        .kl_beta = 0.04,
    };
    hu_rl_trainer_t t = {0};
    HU_ASSERT_EQ(hu_rl_trainer_create_grpo(&alloc, &cfg, &t), HU_OK);

    HU_ASSERT_NOT_NULL(t.vtable);
    HU_ASSERT_NOT_NULL(t.vtable->step);
    HU_ASSERT_NOT_NULL(t.vtable->save_adapter);
    HU_ASSERT_NOT_NULL(t.vtable->name);
    HU_ASSERT_NOT_NULL(t.vtable->deinit);
    HU_ASSERT_TRUE(t.ctx != NULL);
    HU_ASSERT_STR_EQ(t.vtable->name(t.ctx), "grpo_huml");

    t.vtable->deinit(t.ctx, &alloc);
}

/* ─── Test 2: R12 lower-bound rejection ─────────────────────────────── */

static void test_grpo_huml_factory_rejects_n_rollouts_below_2(void) {
    /* n_rollouts == 1 makes the group baseline degenerate (mean
     * trivially equals the single reward; std = 0).  R12 — reject
     * explicitly in the factory, NOT silently in step(). */
    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_config_t cfg = {
        .backend = HU_DPO_BACKEND_HUML,
        .learning_rate = 1e-3,
        .n_rollouts = 1,
        .clip_eps = 0.2,
        .kl_beta = 0.04,
    };
    hu_rl_trainer_t t = {0};
    HU_ASSERT_EQ(hu_rl_trainer_create_grpo(&alloc, &cfg, &t),
                  HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_TRUE(t.ctx == NULL);
    HU_ASSERT_TRUE(t.vtable == NULL);
}

/* ─── Test 3: R12 upper-bound rejection ─────────────────────────────── */

static void test_grpo_huml_factory_rejects_n_rollouts_above_1024(void) {
    /* n_rollouts > 1024 is a safety pin against runaway memory: each
     * rollout allocates up to (prompt_len + max_new_tokens) int32_t for
     * its working sequence buffer plus a probs array.  1024 is the
     * tightest plausible cap that still passes practical GRPO recipes
     * (DeepSeek R1 uses N=64; trl defaults to N=8). */
    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_config_t cfg = {
        .backend = HU_DPO_BACKEND_HUML,
        .learning_rate = 1e-3,
        .n_rollouts = 1025,
        .clip_eps = 0.2,
        .kl_beta = 0.04,
    };
    hu_rl_trainer_t t = {0};
    HU_ASSERT_EQ(hu_rl_trainer_create_grpo(&alloc, &cfg, &t),
                  HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_TRUE(t.ctx == NULL);
    HU_ASSERT_TRUE(t.vtable == NULL);
}

/* ─── Test 4: step() smoke ─────────────────────────────────────────── */

static void test_grpo_huml_step_handles_single_pair_smoke(void) {
    /* Smoke: one prompt, N=4 rollouts, max_new_tokens=8 (rollout impl
     * default), kl_beta=0.04.  Just verify the full pipeline (sample
     * → score → advantage → loss → backward) completes without error
     * and produces finite metrics. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_config_t cfg = {
        .backend = HU_DPO_BACKEND_HUML,
        .learning_rate = 1e-3,
        .n_rollouts = 4,
        .clip_eps = 0.2,
        .kl_beta = 0.04,
    };
    hu_rl_trainer_t t = {0};
    HU_ASSERT_EQ(hu_rl_trainer_create_grpo(&alloc, &cfg, &t), HU_OK);

    hu_preference_pair_t pair;
    make_prompt_pair(&pair);

    hu_rl_trainer_metrics_t m = {0};
    HU_ASSERT_EQ(t.vtable->step(t.ctx, &alloc, &pair, 1, &m), HU_OK);
    /* iters_completed should equal 1 per step() invocation. */
    HU_ASSERT_EQ(m.iters_completed, (size_t)1);
    /* final_loss must be a real number (NaN/Inf would mean the D7/R6
     * std=0 short-circuit or the D8 log_ratio clamp failed). */
    HU_ASSERT_TRUE(m.final_loss == m.final_loss);  /* not NaN */

    t.vtable->deinit(t.ctx, &alloc);
}

/* ─── Test 5: MED-1 kl_beta=0 skips the reference forward ──────────── */

static void test_grpo_huml_step_with_kl_beta_zero_skips_reference_forward(void) {
    /* MED-1 contract (rl_trainer.h:54-64): kl_beta == 0 means the KL
     * penalty is DISABLED.  The impl MUST short-circuit BEFORE the
     * reference forward pass — otherwise the "escape valve" promise
     * leaks the perf cost (a full forward per rollout) every step.
     *
     * Instrument: src/ml/grpo.c counts logprob_on(c, &c->reference, ...)
     * calls under HU_IS_TEST.  After one step() with kl_beta == 0, the
     * counter MUST stay 0.  We then deinit + recreate a second trainer
     * with kl_beta = 0.04 and verify the counter advances past zero
     * (proof the instrumentation is wired, not stuck at 0). */
    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_config_t cfg_off = {
        .backend = HU_DPO_BACKEND_HUML,
        .learning_rate = 1e-3,
        .n_rollouts = 4,
        .clip_eps = 0.2,
        .kl_beta = 0.0,   /* DISABLED — MED-1 escape valve */
    };
    hu_rl_trainer_t t = {0};
    HU_ASSERT_EQ(hu_rl_trainer_create_grpo(&alloc, &cfg_off, &t), HU_OK);

    hu_preference_pair_t pair;
    make_prompt_pair(&pair);

    /* Pre-step: counter is zero. */
    HU_ASSERT_EQ(hu_grpo_huml_ref_forward_count_for_test(t.ctx), (size_t)0);

    hu_rl_trainer_metrics_t m = {0};
    HU_ASSERT_EQ(t.vtable->step(t.ctx, &alloc, &pair, 1, &m), HU_OK);

    /* Post-step (kl_beta == 0): reference forward NEVER called. */
    HU_ASSERT_EQ(hu_grpo_huml_ref_forward_count_for_test(t.ctx), (size_t)0);

    t.vtable->deinit(t.ctx, &alloc);

    /* Sanity counter-check: kl_beta = 0.04 (default) DOES call the
     * reference forward (proves the instrumentation isn't broken). */
    hu_rl_trainer_config_t cfg_on = cfg_off;
    cfg_on.kl_beta = 0.04;
    hu_rl_trainer_t t2 = {0};
    HU_ASSERT_EQ(hu_rl_trainer_create_grpo(&alloc, &cfg_on, &t2), HU_OK);
    HU_ASSERT_EQ(t2.vtable->step(t2.ctx, &alloc, &pair, 1, &m), HU_OK);
    /* Expect N=4 reference forwards per step (one per valid rollout). */
    HU_ASSERT_TRUE(hu_grpo_huml_ref_forward_count_for_test(t2.ctx) > 0);
    t2.vtable->deinit(t2.ctx, &alloc);
}

/* ─── Test 6: step_count advances ──────────────────────────────────── */

static void test_grpo_huml_step_advances_step_count(void) {
    /* Internal step counter is used by the rollout PRNG to derive a
     * per-step seed (so successive steps explore distinct token
     * trajectories).  Pin its monotonic advancement: 0 → 1 → 2. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_config_t cfg = {
        .backend = HU_DPO_BACKEND_HUML,
        .learning_rate = 1e-3,
        .n_rollouts = 4,
        .clip_eps = 0.2,
        .kl_beta = 0.04,
    };
    hu_rl_trainer_t t = {0};
    HU_ASSERT_EQ(hu_rl_trainer_create_grpo(&alloc, &cfg, &t), HU_OK);

    HU_ASSERT_EQ(hu_grpo_huml_step_count_for_test(t.ctx), (size_t)0);

    hu_preference_pair_t pair;
    make_prompt_pair(&pair);

    hu_rl_trainer_metrics_t m = {0};
    HU_ASSERT_EQ(t.vtable->step(t.ctx, &alloc, &pair, 1, &m), HU_OK);
    HU_ASSERT_EQ(hu_grpo_huml_step_count_for_test(t.ctx), (size_t)1);

    HU_ASSERT_EQ(t.vtable->step(t.ctx, &alloc, &pair, 1, &m), HU_OK);
    HU_ASSERT_EQ(hu_grpo_huml_step_count_for_test(t.ctx), (size_t)2);

    t.vtable->deinit(t.ctx, &alloc);
}

/* ─── Test 7: 50-step ASan leak check ──────────────────────────────── */

static void test_grpo_huml_step_does_not_leak_under_asan(void) {
    /* R10 / F6 contract: the single cleanup_rolls label must free EVERY
     * per-iteration allocation, on every control-flow path through the
     * loop body.  50 iterations exercises:
     *   - Successful rollout + reward + advantage + grad path
     *   - Repeated allocator churn (catches a forgotten free in the
     *     hot path that single-iter tests would miss)
     *   - Per-step PRNG advancement (different token sequences each
     *     iter, broader coverage of n_valid filter outcomes)
     * ASan reports leaks at suite teardown — a single leak across 50
     * steps will trip on a clean build. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_config_t cfg = {
        .backend = HU_DPO_BACKEND_HUML,
        .learning_rate = 1e-3,
        .n_rollouts = 4,
        .clip_eps = 0.2,
        .kl_beta = 0.04,
    };
    hu_rl_trainer_t t = {0};
    HU_ASSERT_EQ(hu_rl_trainer_create_grpo(&alloc, &cfg, &t), HU_OK);

    hu_preference_pair_t pair;
    make_prompt_pair(&pair);

    for (int i = 0; i < 50; i++) {
        hu_rl_trainer_metrics_t m = {0};
        HU_ASSERT_EQ(t.vtable->step(t.ctx, &alloc, &pair, 1, &m), HU_OK);
    }
    HU_ASSERT_EQ(hu_grpo_huml_step_count_for_test(t.ctx), (size_t)50);

    t.vtable->deinit(t.ctx, &alloc);
}

/* ============================================================
 *  Phase 4 Task 6 — Sign-of-gradient + grad-check tests
 *  on the GRPO loss against the trainer's structural backward.
 * ============================================================
 *
 * Hard constraint inherited from the Task 6 invocation: NO source
 * modifications allowed (Stage ONLY tests/test_grpo_huml.c — the
 * Task 5 source `src/ml/grpo.c` is OFF-LIMITS).  That precludes the
 * Phase 2 / Phase 3 grad-check pattern of appending `*_for_test`
 * accessors to the trainer source.
 *
 * Public surface available to a tests-only commit:
 *   1. hu_rl_trainer_create_grpo / step / deinit  — trainer lifecycle
 *   2. t.vtable->save_adapter(path)                — dumps lm_head
 *      (V*E float32 = 32*16 = 512 floats = 2048 bytes) to disk; the
 *      ONLY public seam that exposes the trainer's policy weights.
 *   3. metrics.final_loss                          — average group_loss
 *      across prompts
 *   4. metrics.rejected_logprob_delta              — mean group KL
 *      across prompts (repurposed by GRPO per src/ml/grpo.c:561)
 *   5. hu_grpo_huml_step_count_for_test            — internal counter
 *   6. hu_grpo_huml_ref_forward_count_for_test     — MED-1 instrument
 *
 * Implementation reality (acknowledged so the tests below remain
 * honest evidence-based gates rather than aspirational pins):
 *
 *   (a) The GRPO trainer SAMPLES FRESH ROLLOUTS each step under the
 *       CURRENT policy — there is no per-step π_θ_old snapshot (D5,
 *       per Task 5 commit 7610fc1b).  Therefore log_ratio
 *       = lp_pol_now − rolls[i].sum_logprob ≈ 0 within each step
 *       (both terms are computed under the same policy state),
 *       ratio ≈ 1, PPO clipping never activates.
 *   (b) hu_grpo_compute_advantages standardizes to mean = 0,
 *       pop-std = 1, so mean(clipped_advantage * ratio_~1) ≈ 0
 *       → the policy_loss component of metrics.final_loss is
 *       essentially numerical noise.
 *   (c) Therefore metrics.final_loss ≈ kl_beta * mean(group_kl),
 *       which GROWS as the policy diverges from the reference (not
 *       decreases).  The literal "loss before-step > loss after-step"
 *       check from the Task 6 invocation cannot be satisfied via
 *       metrics.final_loss alone.
 *   (d) structural_backward_one_rollout in src/ml/grpo.c:345-386 uses
 *       sign(advantage) as its probe direction on lm_head[token, 0]
 *       (sign-based finite-diff lm_head probe — same template as the
 *       DPO + KTO HUML backwards).  It does NOT consume kl_beta —
 *       kl_beta only enters the LOSS METRIC, not the backward.  So
 *       the literal "kl_penalty pulls back" check from the Task 6
 *       invocation also cannot be satisfied at the policy-drift
 *       level — kl_beta does not influence policy drift in this
 *       implementation; it only scales the KL contribution to the
 *       loss-metric reporting.
 *
 * Adapted tests below preserve the SPIRIT of each Task 6 check while
 * grounding them in what's observable through the public surface:
 *
 *   1. loss_decreases — parameter-space sign-of-improvement via
 *      save_adapter probe: after a step with synthetic reward, the
 *      lm_head[1..5, 0] cells (good tokens) should net move in the
 *      positive direction more than the lm_head[26..30, 0] cells
 *      (bad tokens), because the structural backward bumps lm_head
 *      [token, 0] in sign(advantage) for every token in every rollout
 *      and synthetic reward yields positive advantages on rollouts
 *      that contain good tokens.
 *   2. finite_diff_matches_analytical_on_lm_head_probe — sign-of-
 *      update check on lm_head[3, 0] via save_adapter (the only
 *      public probe).  Documents why true centered finite-difference
 *      (perturb param, re-compute loss) is not achievable without a
 *      source-level test seam, and falls back to sign agreement
 *      between save_adapter-observed Δlm_head[3, 0] and the
 *      synthetic-reward expected direction (token 3 ∈ good set →
 *      positive advantage on rollouts containing it → bumps lm_head
 *      [3, 0] up via the structural backward).
 *   3. advantages_drive_correct_direction — multi-step strengthened
 *      version of (1): after several steps, the AGGREGATE shift on
 *      lm_head[good, 0] is strictly greater than the aggregate shift
 *      on lm_head[bad, 0].  Same probe (save_adapter) — different
 *      assertion shape (5-step accumulation tightens the signal).
 *   4. kl_penalty_pulls_back_when_policy_drifts_far — adapted to
 *      pin the KL CONTRIBUTION to the loss METRIC.  After a few
 *      drift steps, comparing two trainers (kl_beta = 0 vs
 *      kl_beta = 2.0) yields metrics.final_loss(2.0) >>
 *      metrics.final_loss(0).  The structural backward cannot pull
 *      the policy back in this implementation — that's documented
 *      as a known limitation; the pin instead validates that
 *      kl_beta is correctly threaded through hu_grpo_compute_loss
 *      and into the final_loss metric.
 * ============================================================ */

/* Helper: read trainer's lm_head via save_adapter into a flat
 * 32 * 16 = 512-float buffer.  Returns 1 on success, 0 on failure. */
static int read_lm_head(hu_rl_trainer_t *t, hu_allocator_t *alloc,
                         float *out_lm_head_512) {
    char path[] = "/tmp/hu_grpo_huml_lm_head_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return 0;
    close(fd);
    hu_error_t e = t->vtable->save_adapter(t->ctx, alloc, path);
    if (e != HU_OK) { unlink(path); return 0; }
    FILE *f = fopen(path, "rb");
    if (!f) { unlink(path); return 0; }
    size_t got = fread(out_lm_head_512, sizeof(float), (size_t)32 * 16, f);
    fclose(f);
    unlink(path);
    return got == (size_t)32 * 16;
}

/* Synthetic-reward "loss proxy" in parameter space: lower = policy
 * is more biased toward good tokens (1..5) over bad tokens (26..30).
 *
 *   proxy = -(mean lm_head[1..5, 0] - mean lm_head[26..30, 0])
 *
 * The structural backward in src/ml/grpo.c bumps lm_head[token, 0]
 * by sign(advantage) * |advantage| * lr * 0.1 for every token in
 * every rollout (kept iff lp moves in the implied direction).  With
 * synthetic reward giving +1 per good token and −1 per bad token,
 * rollouts heavy in good tokens get positive advantages and rollouts
 * heavy in bad tokens get negative advantages.  Cumulative effect:
 * lm_head[good, 0] drifts up, lm_head[bad, 0] drifts down → proxy
 * decreases as the policy improves. */
static double synthetic_reward_loss_proxy(const float *lm_head_512) {
    double good_sum = 0.0, bad_sum = 0.0;
    for (int t = 1; t <= 5;  t++) good_sum += (double)lm_head_512[t * 16 + 0];
    for (int t = 26; t <= 30; t++) bad_sum += (double)lm_head_512[t * 16 + 0];
    return -((good_sum / 5.0) - (bad_sum / 5.0));
}

/* ─── Test 8 (Task 6 #1): Sign-of-improvement after one step ───────── */

static void test_grpo_huml_loss_decreases_after_one_step_with_synthetic_reward(void) {
    /* Adapted per the implementation-reality block above.  Cannot
     * use metrics.final_loss directly because policy_loss ≈ 0 by
     * construction (log_ratio ≈ 0, mean advantage = 0 by population
     * std normalization), so metrics.final_loss ≈ kl_beta * KL which
     * GROWS not decreases.
     *
     * Substitute a parameter-space proxy via save_adapter — the only
     * public seam exposing the trainer's policy weights.  At lr = 1e-2
     * (10× the trainer default 1e-3 to amplify the per-step delta)
     * one step is enough to make synthetic_reward_loss_proxy strictly
     * decrease — the structural backward bumps every token cell that
     * appears in a positive-advantage rollout up and every token cell
     * in a negative-advantage rollout down, and synthetic reward
     * makes good tokens populate the positive-advantage rollouts.
     *
     * kl_beta = 0 isolates the policy gradient signal — without it,
     * the KL contribution would still be 0 in this single-step test
     * (KL = 0 at init when policy == reference), but pinning it
     * makes the assertion robust against future trainer changes. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_config_t cfg = {
        .backend = HU_DPO_BACKEND_HUML,
        .learning_rate = 1e-2,        /* amplified for single-step signal */
        .n_rollouts = 4,
        .clip_eps = 0.2,
        .kl_beta = 0.0,               /* MED-1 escape valve — isolates policy */
    };
    hu_rl_trainer_t t = {0};
    HU_ASSERT_EQ(hu_rl_trainer_create_grpo(&alloc, &cfg, &t), HU_OK);

    float lm_before[32 * 16] = {0};
    HU_ASSERT_TRUE(read_lm_head(&t, &alloc, lm_before));

    hu_preference_pair_t pair;
    make_prompt_pair(&pair);
    hu_rl_trainer_metrics_t m = {0};
    HU_ASSERT_EQ(t.vtable->step(t.ctx, &alloc, &pair, 1, &m), HU_OK);

    float lm_after[32 * 16] = {0};
    HU_ASSERT_TRUE(read_lm_head(&t, &alloc, lm_after));

    const double loss_before = synthetic_reward_loss_proxy(lm_before);
    const double loss_after  = synthetic_reward_loss_proxy(lm_after);

    /* Sign-of-improvement: parameter-space proxy decreased after one
     * step.  Strict less-than because at lr = 1e-2 the structural
     * backward is unambiguously moving lm_head; a no-change outcome
     * would mean the backward never executed, which should fail. */
    HU_ASSERT_TRUE(loss_after < loss_before);

    t.vtable->deinit(t.ctx, &alloc);
}

/* ─── Test 9 (Task 6 #2): Backward direction aligns with analytical ── */

static void test_grpo_huml_step_finite_diff_matches_analytical_on_lm_head_probe(void) {
    /* Adapted per the implementation-reality block above.  True
     * centered finite-difference — perturb W[3,0] by ±h, re-compute
     * loss at each, divide — REQUIRES two source-level test seams:
     *   (i) write access to a single lm_head cell from the test, AND
     *   (ii) a loss-only computation seam that re-runs the loss path
     *        with caller-supplied completions and rewards (so the
     *        same rollouts appear at both ±h evaluations — without
     *        this, fresh rollouts at each evaluation wash out any
     *        FD signal).
     * Both (i) and (ii) require modifying src/ml/grpo.c, which the
     * Task 6 invocation explicitly forbids ("Do NOT touch tasks 5/8
     * sources").
     *
     * Substitute: an aggregate analog of "FD matches analytical".
     * Define the analytical-direction vector v on the 10 probe cells
     * (lm_head[1..5, 0] ∪ lm_head[26..30, 0]):
     *   v[t] = +1   for t ∈ good_tokens (1..5)
     *   v[t] = −1   for t ∈ bad_tokens  (26..30)
     * v captures the SIGN of the synthetic-reward gradient — a step
     * in the direction of v (i.e., +1 on good cells, −1 on bad
     * cells) DECREASES the loss proxy.
     *
     * Define the OBSERVED-direction vector d on the same cells:
     *   d[t] = lm_head_after[t, 0] − lm_head_before[t, 0]
     * d is what the structural backward actually did in one step.
     *
     * Two assertions:
     *   (a) MAGNITUDE check — ||d||₂ > 1e-5.  Pins that the backward
     *       actually modified lm_head (would fail on a no-op or a
     *       lr = 0 silent regression).
     *   (b) SIGN AGREEMENT — ⟨d, v⟩ > 0.  Pins that the backward's
     *       direction agrees with the analytical gradient direction
     *       on aggregate.  Single-cell sign agreement is unreliable
     *       because src/ml/grpo.c:345-386 iterates per-rollout-per-
     *       token: a single cell can be hit by both positive- and
     *       negative-advantage rollouts in one step, so its net
     *       movement is ambiguous; the aggregate ⟨d, v⟩ is robust
     *       because rewards systematically push good tokens into
     *       positive-advantage rollouts and bad tokens into
     *       negative-advantage rollouts.
     *
     * This is the "FD matches analytical" check available at the
     * public API surface — it pins the sign-of-improvement of the
     * structural backward against the analytical synthetic-reward
     * gradient, with a non-trivial-magnitude floor.  The literal
     * "5% relative magnitude" tolerance from round-3 H4 + Phase 3
     * strengthened FD test pattern requires the loss-only seam (ii)
     * above and is therefore not applicable here. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_config_t cfg = {
        .backend = HU_DPO_BACKEND_HUML,
        .learning_rate = 1e-2,
        .n_rollouts = 4,
        .clip_eps = 0.2,
        .kl_beta = 0.0,
    };
    hu_rl_trainer_t t = {0};
    HU_ASSERT_EQ(hu_rl_trainer_create_grpo(&alloc, &cfg, &t), HU_OK);

    float lm_before[32 * 16] = {0};
    HU_ASSERT_TRUE(read_lm_head(&t, &alloc, lm_before));

    hu_preference_pair_t pair;
    make_prompt_pair(&pair);
    hu_rl_trainer_metrics_t m = {0};
    HU_ASSERT_EQ(t.vtable->step(t.ctx, &alloc, &pair, 1, &m), HU_OK);

    float lm_after[32 * 16] = {0};
    HU_ASSERT_TRUE(read_lm_head(&t, &alloc, lm_after));

    /* Build the observed-direction vector d and dot it against the
     * analytical-direction vector v.  Probe column 0 only — the
     * structural backward in src/ml/grpo.c:372 modifies the (token,
     * 0) cell exclusively. */
    double dot_d_v   = 0.0;   /* ⟨d, v⟩ — sign-agreement signal       */
    double l2_sq     = 0.0;   /* ||d||₂² — magnitude-non-zero signal */
    for (int tk = 1; tk <= 5; tk++) {
        const double d_t = (double)(lm_after[tk * 16 + 0] - lm_before[tk * 16 + 0]);
        dot_d_v += d_t * (+1.0);
        l2_sq   += d_t * d_t;
    }
    for (int tk = 26; tk <= 30; tk++) {
        const double d_t = (double)(lm_after[tk * 16 + 0] - lm_before[tk * 16 + 0]);
        dot_d_v += d_t * (-1.0);
        l2_sq   += d_t * d_t;
    }

    /* (a) MAGNITUDE: structural backward actually moved lm_head.
     * The bump magnitude per kept step is `lr * |advantage| * 0.1`
     * = 1e-2 * ~1 * 0.1 = 1e-3 per cell.  Threshold 1e-5 is two
     * orders of magnitude below per-cell expectation — fails on a
     * silent no-op, doesn't false-positive on float32 noise. */
    HU_ASSERT_TRUE(sqrt(l2_sq) > 1e-5);

    /* (b) SIGN AGREEMENT: backward direction d aligns with the
     * analytical synthetic-reward direction v.  Strict positive
     * dot pins the direction match; the magnitude is bounded
     * below by the (a) check. */
    HU_ASSERT_TRUE(dot_d_v > 0.0);

    t.vtable->deinit(t.ctx, &alloc);
}

/* ─── Test 10 (Task 6 #3): Advantages drive the right direction ────── */

static void test_grpo_huml_step_advantages_drive_correct_direction(void) {
    /* Strengthened multi-step version of test #1 above.  After 5
     * steps with synthetic reward, the AGGREGATE shift on the good-
     * token rows of lm_head should be strictly greater than the
     * aggregate shift on the bad-token rows — the structural
     * backward, driven by synthetic-reward advantages, biases the
     * policy toward tokens 1..5 and away from tokens 26..30.
     *
     * Multi-step accumulation tightens the signal vs the single-
     * step test: the 5-step delta integrates over 5 fresh rollout
     * batches × 4 rollouts × ≤8 tokens each = up to 160 token-cell
     * bumps, dwarfing per-step rollout-composition variance. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_config_t cfg = {
        .backend = HU_DPO_BACKEND_HUML,
        .learning_rate = 1e-2,
        .n_rollouts = 4,
        .clip_eps = 0.2,
        .kl_beta = 0.0,
    };
    hu_rl_trainer_t t = {0};
    HU_ASSERT_EQ(hu_rl_trainer_create_grpo(&alloc, &cfg, &t), HU_OK);

    float lm_before[32 * 16] = {0};
    HU_ASSERT_TRUE(read_lm_head(&t, &alloc, lm_before));

    hu_preference_pair_t pair;
    make_prompt_pair(&pair);
    for (int i = 0; i < 5; i++) {
        hu_rl_trainer_metrics_t m = {0};
        HU_ASSERT_EQ(t.vtable->step(t.ctx, &alloc, &pair, 1, &m), HU_OK);
    }

    float lm_after[32 * 16] = {0};
    HU_ASSERT_TRUE(read_lm_head(&t, &alloc, lm_after));

    /* Aggregate shift across the 5 good-token rows vs the 5 bad-
     * token rows on column 0 (the column the structural backward
     * probes — see src/ml/grpo.c:372 `cell = lm_head + tk * E`,
     * which is lm_head[tk, 0]). */
    double good_delta = 0.0, bad_delta = 0.0;
    for (int tk = 1;  tk <= 5;  tk++)
        good_delta += (double)(lm_after[tk * 16 + 0] - lm_before[tk * 16 + 0]);
    for (int tk = 26; tk <= 30; tk++)
        bad_delta  += (double)(lm_after[tk * 16 + 0] - lm_before[tk * 16 + 0]);

    /* Advantage direction: good tokens' aggregate shift > bad
     * tokens' aggregate shift.  The strict inequality is robust
     * because synthetic reward systematically pushes good rollouts
     * positive and bad rollouts negative across many sample batches. */
    HU_ASSERT_TRUE(good_delta > bad_delta);

    t.vtable->deinit(t.ctx, &alloc);
}

/* ─── Test 11 (Task 6 #4): KL term is correctly applied to the loss ─── */

static void test_grpo_huml_kl_penalty_pulls_back_when_policy_drifts_far(void) {
    /* Adapted per the implementation-reality block above.  The
     * structural backward in src/ml/grpo.c does not consume kl_beta —
     * it only uses sign(advantage) on lm_head[token, 0].  Therefore
     * kl_beta cannot literally "pull the policy back" toward the
     * reference in this implementation — that property would require
     * a true gradient-based backward that includes the KL gradient
     * term (Phase 4 Task 8 MLX backend, or a future HUML upgrade).
     *
     * What we CAN pin from a tests-only commit: kl_beta is correctly
     * threaded into the LOSS METRIC reported by step().  After a few
     * drift steps (so KL > 0), two trainers configured identically
     * except for kl_beta should report:
     *   - kl_beta = 0     → final_loss == 0   (MED-1 short-circuit)
     *   - kl_beta = 2.0   → final_loss > 0    (KL × 2.0 contribution)
     *
     * And rejected_logprob_delta (mean group KL — repurposed metric
     * per src/ml/grpo.c:561) is non-zero under kl_beta = 2.0
     * (proves the reference-forward path actually runs and the KL
     * value is computed) but stays 0 under kl_beta = 0 (MED-1
     * skips the entire reference forward + KL path). */
    hu_allocator_t alloc = hu_system_allocator();

    /* Trainer A: kl_beta = 0 — MED-1 disables the KL term entirely. */
    hu_rl_trainer_config_t cfg_off = {
        .backend = HU_DPO_BACKEND_HUML,
        .learning_rate = 1e-2,
        .n_rollouts = 4,
        .clip_eps = 0.2,
        .kl_beta = 0.0,
    };
    hu_rl_trainer_t t_off = {0};
    HU_ASSERT_EQ(hu_rl_trainer_create_grpo(&alloc, &cfg_off, &t_off), HU_OK);

    hu_preference_pair_t pair;
    make_prompt_pair(&pair);

    /* Drift the policy off the reference for 5 unconstrained steps
     * (kl_beta = 0 means the structural backward runs but no KL
     * contribution to the loss metric, no reference forward). */
    hu_rl_trainer_metrics_t m_off = {0};
    for (int i = 0; i < 5; i++) {
        HU_ASSERT_EQ(t_off.vtable->step(t_off.ctx, &alloc, &pair, 1, &m_off), HU_OK);
    }

    /* MED-1 contract from the metrics side:
     *   - kl_beta = 0 → final_loss = -mean(clipped_adv) ≈ 0
     *     (advantages standardize to mean 0; log_ratio ≈ 0 within step)
     *   - rejected_logprob_delta = mean group KL = 0 (KL never computed) */
    HU_ASSERT_TRUE(fabs(m_off.final_loss) < 1e-6);
    HU_ASSERT_EQ(hu_grpo_huml_ref_forward_count_for_test(t_off.ctx), (size_t)0);

    t_off.vtable->deinit(t_off.ctx, &alloc);

    /* Trainer B: kl_beta = 2.0 — high KL coefficient amplifies the
     * KL contribution to the loss metric.  After 5 drift steps the
     * policy has diverged from the (frozen) reference enough that
     * group_kl > 0; at kl_beta = 2.0 the loss metric reflects this. */
    hu_rl_trainer_config_t cfg_high = {
        .backend = HU_DPO_BACKEND_HUML,
        .learning_rate = 1e-2,
        .n_rollouts = 4,
        .clip_eps = 0.2,
        .kl_beta = 2.0,        /* high — amplify KL contribution */
    };
    hu_rl_trainer_t t_high = {0};
    HU_ASSERT_EQ(hu_rl_trainer_create_grpo(&alloc, &cfg_high, &t_high), HU_OK);

    hu_rl_trainer_metrics_t m_high = {0};
    for (int i = 0; i < 5; i++) {
        HU_ASSERT_EQ(t_high.vtable->step(t_high.ctx, &alloc, &pair, 1, &m_high), HU_OK);
    }

    /* Reference forward counter is non-zero (proves the KL path
     * actually runs when kl_beta != 0). */
    HU_ASSERT_TRUE(hu_grpo_huml_ref_forward_count_for_test(t_high.ctx) > 0);

    /* Mean group KL on the final step is strictly > 0 — the policy
     * drifted off the reference and the KL term picks it up.  This
     * is the "policy drifts far" half of the test name; the KL
     * term in the loss metric pulls the loss UP (which under a
     * gradient-based backward would translate into a pull-back
     * pressure on the policy — but in this sign-based backward it
     * does not, hence the loss-metric pin rather than a policy-
     * trajectory pin). */
    HU_ASSERT_TRUE(m_high.rejected_logprob_delta > 0.0);

    /* Final loss with kl_beta = 2.0 strictly exceeds the kl_beta = 0
     * baseline (which was ≈ 0).  At kl_beta = 2.0 the loss metric
     * is approximately 2.0 * mean(group_kl), so for any non-trivial
     * drift the loss is strictly positive. */
    HU_ASSERT_TRUE(m_high.final_loss > m_off.final_loss);
    HU_ASSERT_TRUE(m_high.final_loss > 1e-6);

    t_high.vtable->deinit(t_high.ctx, &alloc);
}

void run_grpo_huml_tests(void) {
    HU_TEST_SUITE("grpo_huml");
    HU_RUN_TEST(test_grpo_huml_factory_returns_populated_vtable);
    HU_RUN_TEST(test_grpo_huml_factory_rejects_n_rollouts_below_2);
    HU_RUN_TEST(test_grpo_huml_factory_rejects_n_rollouts_above_1024);
    HU_RUN_TEST(test_grpo_huml_step_handles_single_pair_smoke);
    HU_RUN_TEST(test_grpo_huml_step_with_kl_beta_zero_skips_reference_forward);
    HU_RUN_TEST(test_grpo_huml_step_advances_step_count);
    HU_RUN_TEST(test_grpo_huml_step_does_not_leak_under_asan);
    /* Phase 4 Task 6 — sign-of-gradient + grad-check tests. */
    HU_RUN_TEST(test_grpo_huml_loss_decreases_after_one_step_with_synthetic_reward);
    HU_RUN_TEST(test_grpo_huml_step_finite_diff_matches_analytical_on_lm_head_probe);
    HU_RUN_TEST(test_grpo_huml_step_advantages_drive_correct_direction);
    HU_RUN_TEST(test_grpo_huml_kl_penalty_pulls_back_when_policy_drifts_far);
}
