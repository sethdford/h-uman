/* tests/test_reward_source.c — Phase 4 Task 4 (RL SOTA)
 *
 * Pins the hu_reward_source_t leaf vtable contract:
 *
 *   1. Synthetic backend returns finite scores (smoke).
 *   2. Synthetic backend scores per the documented convention:
 *        +1 per token in [1..5], -1 per token in [26..30], 0 otherwise.
 *      Tested on three handcrafted completions with known deltas (3, -3, 1).
 *   3. Synthetic backend returns 0 for an empty completion (n_tokens == 0).
 *   4. RM backend composes a Phase 3 HUML hu_reward_model_t and returns
 *      finite scores for two completions. The HUML RM is cross-platform
 *      and needs no Qwen GGUF, so this should pass everywhere; the
 *      HU_SKIP_IF guard exists only for defense-in-depth if a future
 *      change makes hu_reward_model_create_huml conditionally fail.
 *   5. JUDGE factory returns HU_ERR_NOT_SUPPORTED — pinned so Phase 5
 *      replacement is a forced compile-and-test event, not a silent
 *      drop-in.
 *
 * The reward source borrows the hu_reward_model_t pointer (per the
 * header contract); these tests construct, score, and then deinit in
 * source → rm order so the rm always outlives the source.
 */
#include "test_framework.h"

#include "human/core/allocator.h"
#include "human/ml/reward_model.h"
#include "human/ml/reward_source.h"
#include "human/ml/rollout.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Helper: build a completion that BORROWS a caller-owned token buffer.
 * No allocator-owned storage — the test owns the int32_t[] and we copy
 * the pointer in. token_ids_cap is irrelevant here because we never
 * call hu_rollout_free_completions on these stack-borrowed completions. */
static hu_rollout_completion_t make_borrowed_completion(int32_t *toks, size_t n) {
    hu_rollout_completion_t c = {0};
    c.token_ids = toks;
    c.n_tokens = n;
    c.token_ids_cap = n;
    c.sum_logprob = 0.0;
    return c;
}

static void test_reward_source_synthetic_returns_finite_scores(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_reward_source_t src = {0};
    HU_ASSERT_EQ(hu_reward_source_create_synthetic(&alloc, &src), HU_OK);
    HU_ASSERT_NOT_NULL(src.vtable);
    HU_ASSERT_NOT_NULL(src.vtable->score);
    HU_ASSERT_NOT_NULL(src.vtable->name);
    HU_ASSERT_NOT_NULL(src.vtable->deinit);
    HU_ASSERT_STR_EQ(src.vtable->name(&src), "synthetic");

    int32_t prompt[3] = {0, 1, 2};
    int32_t comp_a[3] = {1, 2, 3};
    int32_t comp_b[3] = {26, 27, 28};
    hu_rollout_completion_t comps[2] = {
        make_borrowed_completion(comp_a, 3),
        make_borrowed_completion(comp_b, 3),
    };
    double rewards[2] = {0.0, 0.0};
    HU_ASSERT_EQ(src.vtable->score(&src, prompt, 3, comps, 2, rewards), HU_OK);
    HU_ASSERT_TRUE(isfinite(rewards[0]));
    HU_ASSERT_TRUE(isfinite(rewards[1]));

    src.vtable->deinit(&src);
    HU_ASSERT_NULL(src.vtable);
    HU_ASSERT_NULL(src.ctx);
}

static void test_reward_source_synthetic_counts_good_tokens_minus_bad(void) {
    /* Token-counting convention pin: +1 per token in [1..5], -1 per
     * token in [26..30]. Mirrors Phase 3 Task 3's make_synthetic_pairs.
     * If this test fails because the convention changed, the GRPO E2E
     * test (Task 7) and the cold-start CLI path (Task 9) silently break
     * — fix the convention here, not the test gate. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_reward_source_t src = {0};
    HU_ASSERT_EQ(hu_reward_source_create_synthetic(&alloc, &src), HU_OK);

    int32_t all_good[3] = {1, 2, 3};      /* +3 */
    int32_t all_bad[3]  = {26, 27, 28};   /* -3 */
    int32_t mixed[3]    = {1, 26, 2};     /* +1 + (-1) + 1 = +1 */
    hu_rollout_completion_t comps[3] = {
        make_borrowed_completion(all_good, 3),
        make_borrowed_completion(all_bad, 3),
        make_borrowed_completion(mixed, 3),
    };
    double rewards[3] = {NAN, NAN, NAN};
    HU_ASSERT_EQ(src.vtable->score(&src, NULL, 0, comps, 3, rewards), HU_OK);
    HU_ASSERT_FLOAT_EQ(rewards[0],  3.0, 1e-12);
    HU_ASSERT_FLOAT_EQ(rewards[1], -3.0, 1e-12);
    HU_ASSERT_FLOAT_EQ(rewards[2],  1.0, 1e-12);

    src.vtable->deinit(&src);
}

static void test_reward_source_synthetic_handles_empty_completion(void) {
    /* Edge case: n_tokens == 0 → score 0.0. Real rollouts can produce
     * zero-token completions if the policy emits EOS immediately at
     * step 0 with max_new_tokens > 0 — the trainer must not crash on
     * these. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_reward_source_t src = {0};
    HU_ASSERT_EQ(hu_reward_source_create_synthetic(&alloc, &src), HU_OK);

    hu_rollout_completion_t empty = {0};
    empty.token_ids = NULL;
    empty.n_tokens = 0;
    empty.token_ids_cap = 0;
    double reward = 1234.5;
    HU_ASSERT_EQ(src.vtable->score(&src, NULL, 0, &empty, 1, &reward), HU_OK);
    HU_ASSERT_FLOAT_EQ(reward, 0.0, 1e-12);

    src.vtable->deinit(&src);
}

static void test_reward_source_rm_smoke_returns_finite_score(void) {
    /* Compose a Phase 3 HUML reward model and score two completions
     * through it. The HUML RM needs no GGUF backbone (Xavier-init value
     * head over a toy GPT) so it should construct unconditionally; the
     * skip guard is defense-in-depth for a future change that wires
     * the HUML path to need a checkpoint. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_reward_model_config_t cfg = {
        .backend = HU_REWARD_MODEL_BACKEND_HUML,
        .vocab_size = 32,
        .hidden_dim = 32,
    };
    hu_reward_model_t rm = {0};
    hu_error_t rm_err = hu_reward_model_create_huml(&alloc, &cfg, &rm);
    HU_SKIP_IF(rm_err != HU_OK,
                "hu_reward_model_create_huml failed; HUML backbone unavailable in this build");
    HU_ASSERT_NOT_NULL(rm.vtable);

    hu_reward_source_t src = {0};
    HU_ASSERT_EQ(hu_reward_source_create_rm(&alloc, &rm, &src), HU_OK);
    HU_ASSERT_STR_EQ(src.vtable->name(&src), "reward_model");

    int32_t prompt[3] = {0, 1, 2};
    /* RM rejects empty completion text, so use 2+ tokens per completion. */
    int32_t comp_a[3] = {1, 2, 3};
    int32_t comp_b[3] = {26, 27, 28};
    hu_rollout_completion_t comps[2] = {
        make_borrowed_completion(comp_a, 3),
        make_borrowed_completion(comp_b, 3),
    };
    double rewards[2] = {NAN, NAN};
    HU_ASSERT_EQ(src.vtable->score(&src, prompt, 3, comps, 2, rewards), HU_OK);
    HU_ASSERT_TRUE(isfinite(rewards[0]));
    HU_ASSERT_TRUE(isfinite(rewards[1]));

    /* Deinit source FIRST (source borrows rm) — keeps the lifetime
     * order documented in the header an executable contract. */
    src.vtable->deinit(&src);
    rm.vtable->deinit(rm.ctx, &alloc);
}

static void test_reward_source_judge_returns_not_supported_until_phase_5(void) {
    /* Phase 5 will land the real llm-judge impl. Until then, the factory
     * returns HU_ERR_NOT_SUPPORTED — pinned so a future Phase 5 wire-up
     * is forced through a deliberate flip of this gate, not a silent
     * drop-in. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_reward_source_t src = {0};
    HU_ASSERT_EQ(hu_reward_source_create_judge(&alloc, &src), HU_ERR_NOT_SUPPORTED);
    /* On NOT_SUPPORTED the factory leaves `out` untouched (initialized
     * to zero by the caller) — no deinit needed, no double-free risk. */
    HU_ASSERT_NULL(src.vtable);
    HU_ASSERT_NULL(src.ctx);
}

void run_reward_source_tests(void) {
    HU_TEST_SUITE("reward_source");
    HU_RUN_TEST(test_reward_source_synthetic_returns_finite_scores);
    HU_RUN_TEST(test_reward_source_synthetic_counts_good_tokens_minus_bad);
    HU_RUN_TEST(test_reward_source_synthetic_handles_empty_completion);
    HU_RUN_TEST(test_reward_source_rm_smoke_returns_finite_score);
    HU_RUN_TEST(test_reward_source_judge_returns_not_supported_until_phase_5);
}
