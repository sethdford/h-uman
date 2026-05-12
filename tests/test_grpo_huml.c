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
#include <stddef.h>
#include <string.h>

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

void run_grpo_huml_tests(void) {
    HU_TEST_SUITE("grpo_huml");
    HU_RUN_TEST(test_grpo_huml_factory_returns_populated_vtable);
    HU_RUN_TEST(test_grpo_huml_factory_rejects_n_rollouts_below_2);
    HU_RUN_TEST(test_grpo_huml_factory_rejects_n_rollouts_above_1024);
    HU_RUN_TEST(test_grpo_huml_step_handles_single_pair_smoke);
    HU_RUN_TEST(test_grpo_huml_step_with_kl_beta_zero_skips_reference_forward);
    HU_RUN_TEST(test_grpo_huml_step_advances_step_count);
    HU_RUN_TEST(test_grpo_huml_step_does_not_leak_under_asan);
}
