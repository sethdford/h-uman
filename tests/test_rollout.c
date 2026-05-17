/* tests/test_rollout.c — Phase 4 Task 2 (RL SOTA)
 *
 * Pins hu_rollout_t HUML factory + sampling contract:
 *   - factory returns a fully-populated vtable
 *   - same seed → byte-identical token IDs and identical sum_logprob
 *   - distinct seeds → distinct sequences (independence between rollouts)
 *   - n_rollouts honoured (request N=4 → 4 completions populated)
 *   - max_new_tokens cap honoured
 *   - cross-platform determinism pin (R13 — xorshift64 seed=42 is
 *     constant across glibc / Apple libc; we pin the actual first
 *     token ID so any RNG drift fails the suite).
 *
 * Tests exercise the toy GPT — small enough that the full sample loop
 * runs in milliseconds under ASan and that the pin assertion stays
 * deterministic across platforms (R13).
 */
#include "test_framework.h"
#include "human/ml/rollout.h"
#include "human/ml/model.h"
#include "human/ml/ml.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <math.h>
#include <string.h>

/* Build a tiny GPT for rollout tests. vocab=32, n_embd=16, sequence_len=64
 * is large enough for prompt(3) + max_new_tokens(8) and small enough
 * that the forward path is ~microseconds. (Mirrors the canonical plan
 * snippet at docs/plans/2026-05-11-rl-loop-phase-4-grpo.md:886-892.) */
static int make_toy_gpt(hu_allocator_t *alloc, hu_model_t *out, hu_gpt_config_t *cfg) {
    *cfg = (hu_gpt_config_t){
        .vocab_size = 32, .n_layer = 1, .n_head = 1, .n_kv_head = 1,
        .n_embd = 16, .head_dim = 16, .sequence_len = 64,
    };
    return hu_gpt_create(alloc, cfg, out) == HU_OK ? 1 : 0;
}

static void test_rollout_huml_factory_returns_populated_vtable(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_model_t model = {0};
    hu_gpt_config_t cfg = {0};
    HU_ASSERT_TRUE(make_toy_gpt(&alloc, &model, &cfg));

    hu_rollout_t r = {0};
    HU_ASSERT_EQ(hu_rollout_create_huml(&alloc, &model, /*seed=*/42ull, &r), HU_OK);
    HU_ASSERT_NOT_NULL(r.vtable);
    HU_ASSERT_NOT_NULL(r.vtable->sample);
    HU_ASSERT_NOT_NULL(r.vtable->name);
    HU_ASSERT_NOT_NULL(r.vtable->deinit);
    HU_ASSERT_NOT_NULL(r.ctx);
    HU_ASSERT_STR_EQ(r.vtable->name(r.ctx), "rollout_huml");

    /* MLX factory is the Phase 4 Task 8 surface — Task 2 declares it
     * returning HU_ERR_NOT_SUPPORTED so the GRPO trainer's MLX dispatch
     * path links cleanly. */
    hu_rollout_t mlx_r = {0};
    HU_ASSERT_EQ(hu_rollout_create_mlx(&alloc, "gemma-3-4b-it", 42ull, &mlx_r),
                 HU_ERR_NOT_SUPPORTED);

    r.vtable->deinit(r.ctx, &alloc);
    model.vtable->deinit(model.ctx, &alloc);
}

static void test_rollout_huml_null_args_return_invalid_argument(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_model_t model = {0};
    hu_rollout_t r = {0};

    /* Create-time validation. */
    HU_ASSERT_EQ(hu_rollout_create_huml(NULL, &model, 42ull, &r), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_rollout_create_huml(&alloc, NULL, 42ull, &r), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_rollout_create_huml(&alloc, &model, 42ull, NULL), HU_ERR_INVALID_ARGUMENT);

    /* hu_rollout_free_completions(NULL, ...) and free(.., NULL, ..) must
     * be no-ops, not crashes — used in error-path cleanup by callers. */
    hu_rollout_free_completions(NULL, NULL, 0);
    hu_rollout_free_completions(&alloc, NULL, 5);
}

static void test_rollout_huml_seed_42_produces_deterministic_token_ids(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_model_t model = {0};
    hu_gpt_config_t cfg = {0};
    HU_ASSERT_TRUE(make_toy_gpt(&alloc, &model, &cfg));

    int32_t prompt[3] = {1, 2, 3};
    hu_rollout_completion_t a[2] = {{0}};
    hu_rollout_completion_t b[2] = {{0}};

    /* Two batches with the SAME seed must produce byte-identical results.
     * This is the core determinism contract that GRPO's finite-diff grad
     * checks rely on (R9): re-sampling between L(θ+ε) and L(θ−ε) would
     * make numerical gradients meaningless. */
    hu_rollout_t r1 = {0}, r2 = {0};
    HU_ASSERT_EQ(hu_rollout_create_huml(&alloc, &model, /*seed=*/42ull, &r1), HU_OK);
    HU_ASSERT_EQ(hu_rollout_create_huml(&alloc, &model, /*seed=*/42ull, &r2), HU_OK);
    HU_ASSERT_EQ(r1.vtable->sample(r1.ctx, &alloc, prompt, 3, /*N=*/2,
                                    /*max_new_tokens=*/4, /*temperature=*/0.7, a), HU_OK);
    HU_ASSERT_EQ(r2.vtable->sample(r2.ctx, &alloc, prompt, 3, /*N=*/2,
                                    /*max_new_tokens=*/4, /*temperature=*/0.7, b), HU_OK);

    HU_ASSERT_EQ(a[0].n_tokens, b[0].n_tokens);
    HU_ASSERT_EQ(a[1].n_tokens, b[1].n_tokens);
    for (size_t i = 0; i < a[0].n_tokens; i++)
        HU_ASSERT_EQ(a[0].token_ids[i], b[0].token_ids[i]);
    for (size_t i = 0; i < a[1].n_tokens; i++)
        HU_ASSERT_EQ(a[1].token_ids[i], b[1].token_ids[i]);
    /* F3 (Phase 4 end-gate audit): same machine + same input + same
     * sampled tokens MUST produce BIT-IDENTICAL sum_logprob, not just
     * within 1e-9 tolerance.  A tolerance-only pin would silently
     * accept sub-1e-9 float drift in the policy forward — exactly the
     * kind of drift that would make GRPO non-deterministic between
     * L(θ±ε) probes without this test catching it.  memcmp on the
     * raw bytes is strictly stronger than the prior fabs(...) < 1e-9
     * check (and just as portable: IEEE-754 doubles are bit-stable
     * under identical compute). */
    HU_ASSERT_EQ(memcmp(&a[0].sum_logprob, &b[0].sum_logprob, sizeof(double)), 0);
    HU_ASSERT_EQ(memcmp(&a[1].sum_logprob, &b[1].sum_logprob, sizeof(double)), 0);
    /* Float-precision tolerance check kept as a redundant (looser)
     * pin so a future relaxation of the bit-exact check still catches
     * gross drift. */
    HU_ASSERT_TRUE(fabs(a[0].sum_logprob - b[0].sum_logprob) < 1e-9);
    HU_ASSERT_TRUE(fabs(a[1].sum_logprob - b[1].sum_logprob) < 1e-9);
    /* sum_logprob is a sum of logs of probabilities ≤ 1, so it must
     * be ≤ 0 whenever n_tokens > 0 (rules out the silent-zero failure
     * mode flagged by F1 — a fresh, uninitialised double could happen
     * to match between runs and pass the bit-equal check above). */
    if (a[0].n_tokens > 0) HU_ASSERT_TRUE(a[0].sum_logprob <= 0.0);
    if (a[1].n_tokens > 0) HU_ASSERT_TRUE(a[1].sum_logprob <= 0.0);

    hu_rollout_free_completions(&alloc, a, 2);
    hu_rollout_free_completions(&alloc, b, 2);
    r1.vtable->deinit(r1.ctx, &alloc);
    r2.vtable->deinit(r2.ctx, &alloc);
    model.vtable->deinit(model.ctx, &alloc);
}

static void test_rollout_huml_n_rollouts_returns_n_completions(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_model_t model = {0};
    hu_gpt_config_t cfg = {0};
    HU_ASSERT_TRUE(make_toy_gpt(&alloc, &model, &cfg));

    int32_t prompt[3] = {1, 2, 3};
    /* Request the GRPO default of N=4 (Phase 4 plan §D6). */
    hu_rollout_completion_t c[4] = {{0}};
    hu_rollout_t r = {0};
    HU_ASSERT_EQ(hu_rollout_create_huml(&alloc, &model, 42ull, &r), HU_OK);
    HU_ASSERT_EQ(r.vtable->sample(r.ctx, &alloc, prompt, 3, /*N=*/4, 4, 0.7, c), HU_OK);

    /* Every requested completion must be populated with a token_ids buffer. */
    for (size_t i = 0; i < 4; i++) {
        HU_ASSERT_NOT_NULL(c[i].token_ids);
        HU_ASSERT_TRUE(c[i].n_tokens <= 4);
        /* sum_logprob is a sum of logs of probabilities ≤ 1, so it is
         * always non-positive when at least one token was sampled. */
        if (c[i].n_tokens > 0) HU_ASSERT_TRUE(c[i].sum_logprob <= 0.0);
        /* Sampled tokens must be valid vocab indices. */
        for (size_t t = 0; t < c[i].n_tokens; t++) {
            HU_ASSERT_TRUE(c[i].token_ids[t] >= 0);
            HU_ASSERT_TRUE(c[i].token_ids[t] < (int32_t)cfg.vocab_size);
        }
    }

    hu_rollout_free_completions(&alloc, c, 4);
    r.vtable->deinit(r.ctx, &alloc);
    model.vtable->deinit(model.ctx, &alloc);
}

static void test_rollout_huml_distinct_seeds_produce_distinct_completions(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_model_t model = {0};
    hu_gpt_config_t cfg = {0};
    HU_ASSERT_TRUE(make_toy_gpt(&alloc, &model, &cfg));

    int32_t prompt[3] = {1, 2, 3};
    hu_rollout_completion_t a[1] = {{0}}, b[1] = {{0}};

    hu_rollout_t r1 = {0}, r2 = {0};
    HU_ASSERT_EQ(hu_rollout_create_huml(&alloc, &model, /*seed=*/1ull, &r1), HU_OK);
    HU_ASSERT_EQ(hu_rollout_create_huml(&alloc, &model, /*seed=*/2ull, &r2), HU_OK);
    HU_ASSERT_EQ(r1.vtable->sample(r1.ctx, &alloc, prompt, 3, 1, 8, 0.7, a), HU_OK);
    HU_ASSERT_EQ(r2.vtable->sample(r2.ctx, &alloc, prompt, 3, 1, 8, 0.7, b), HU_OK);

    /* Distinct splitmix64-derived states should drive the multinomial
     * sampler down different branches at least once over an 8-token
     * completion (probability of identical sequences is vanishingly
     * small for a vocab=32 toy GPT with random init). */
    int any_differ = 0;
    size_t common = a[0].n_tokens < b[0].n_tokens ? a[0].n_tokens : b[0].n_tokens;
    for (size_t i = 0; i < common; i++) {
        if (a[0].token_ids[i] != b[0].token_ids[i]) { any_differ = 1; break; }
    }
    HU_ASSERT_TRUE(any_differ || a[0].n_tokens != b[0].n_tokens);

    hu_rollout_free_completions(&alloc, a, 1);
    hu_rollout_free_completions(&alloc, b, 1);
    r1.vtable->deinit(r1.ctx, &alloc);
    r2.vtable->deinit(r2.ctx, &alloc);
    model.vtable->deinit(model.ctx, &alloc);
}

static void test_rollout_huml_respects_max_new_tokens_cap(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_model_t model = {0};
    hu_gpt_config_t cfg = {0};
    HU_ASSERT_TRUE(make_toy_gpt(&alloc, &model, &cfg));

    int32_t prompt[2] = {5, 6};
    hu_rollout_completion_t c[1] = {{0}};
    hu_rollout_t r = {0};
    HU_ASSERT_EQ(hu_rollout_create_huml(&alloc, &model, 42ull, &r), HU_OK);
    /* Cap = 3. EOS may stop us earlier; we never exceed the cap. */
    HU_ASSERT_EQ(r.vtable->sample(r.ctx, &alloc, prompt, 2, 1,
                                  /*max_new_tokens=*/3, 0.7, c), HU_OK);
    HU_ASSERT_TRUE(c[0].n_tokens <= 3);
    /* token_ids buffer must be non-NULL even when n_tokens == 0 — the
     * impl allocates a size-1 stub so callers can dereference safely. */
    HU_ASSERT_NOT_NULL(c[0].token_ids);

    hu_rollout_free_completions(&alloc, c, 1);
    r.vtable->deinit(r.ctx, &alloc);
    model.vtable->deinit(model.ctx, &alloc);
}

/* Cross-platform determinism pin (R13).
 *
 * The xorshift64 PRNG used by HUML rollout has the SAME constants on every
 * platform (unlike libc rand()/rand_r() which produce platform-divergent
 * sequences from the same seed). The pin below was captured on macOS
 * arm64 with the `rl_sota` preset; if the same seed produces a different
 * token on Linux or x86_64, the xorshift64 path has a platform-dependent
 * bug (likely `double` rounding or signed-shift UB) and must be fixed
 * before this test is considered passing.
 *
 * The Phase 4 plan (Task 2 Step 5, critic M5) makes this assertion a
 * BLOCKING acceptance criterion — if it's still commented out at end-
 * gate, sprint-auditor fails Task 2.
 *
 * MLX rollout factory tests are gated by HU_HAVE_MLX_LM_GRPO at compile
 * time per round-3 critic L1 — `#if !defined(HU_HAVE_MLX_LM_GRPO) ||
 * HU_HAVE_MLX_LM_GRPO == 0` early-return rather than HU_SKIP_IF. */
static void test_rollout_huml_seed_42_produces_identical_token_ids_macos_and_linux(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_model_t model = {0};
    hu_gpt_config_t cfg = {0};
    HU_ASSERT_TRUE(make_toy_gpt(&alloc, &model, &cfg));

    int32_t prompt[3] = {1, 2, 3};
    hu_rollout_completion_t c[1] = {{0}};
    hu_rollout_t r = {0};
    HU_ASSERT_EQ(hu_rollout_create_huml(&alloc, &model, /*seed=*/42ull, &r), HU_OK);
    HU_ASSERT_EQ(r.vtable->sample(r.ctx, &alloc, prompt, 3, 1,
                                  /*max_new_tokens=*/4,
                                  /*temperature=*/0.7, c), HU_OK);
    HU_ASSERT_TRUE(c[0].n_tokens > 0);

    /* Pin the FIRST sampled token ID. Captured on macOS arm64 (Apple M-
     * series, build-rl-sota, dev preset → rl_sota inherits dev → ASan).
     * Determinism is structural (xorshift64 + splitmix64 use literal
     * 64-bit constants) so the same value MUST appear on Linux x86_64 /
     * arm64 / glibc / musl. If CI fails here, the bug is in this file
     * (or in hu_gpt_create's parameter init), not in the test. */
    HU_ASSERT_EQ(c[0].token_ids[0], 25);
    /* F3 (Phase 4 end-gate audit): sum_logprob is a sum of logs of
     * probabilities ≤ 1, so it must be < 0 whenever n_tokens > 0.  A
     * silent failure mode the audit flagged was a fresh-zero
     * sum_logprob slipping through (e.g., uninitialised by a probe-
     * forward error path); pinning the sign is a cheap structural
     * witness even without a hardcoded cross-platform value. */
    HU_ASSERT_TRUE(c[0].sum_logprob < 0.0);

    hu_rollout_free_completions(&alloc, c, 1);
    r.vtable->deinit(r.ctx, &alloc);
    model.vtable->deinit(model.ctx, &alloc);
}

void run_rollout_tests(void) {
    HU_TEST_SUITE("rollout");
    HU_RUN_TEST(test_rollout_huml_factory_returns_populated_vtable);
    HU_RUN_TEST(test_rollout_huml_null_args_return_invalid_argument);
    HU_RUN_TEST(test_rollout_huml_seed_42_produces_deterministic_token_ids);
    HU_RUN_TEST(test_rollout_huml_n_rollouts_returns_n_completions);
    HU_RUN_TEST(test_rollout_huml_distinct_seeds_produce_distinct_completions);
    HU_RUN_TEST(test_rollout_huml_respects_max_new_tokens_cap);
    HU_RUN_TEST(test_rollout_huml_seed_42_produces_identical_token_ids_macos_and_linux);
}
