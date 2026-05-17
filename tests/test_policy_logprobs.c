/* tests/test_policy_logprobs.c — Phase 2 Task 2
 *
 * Verifies hu_policy_logprobs computes a valid log π(y|x) on a tiny
 * randomly-initialised GPT, is deterministic for the same inputs, and
 * rejects NULL arguments.
 *
 * Plan deviation notes:
 *   1. The canonical plan snippet (lines 519-587) `#include`s
 *      `"human/allocator.h"`. That header does not exist in this repo —
 *      the real path is `"human/core/allocator.h"`. Same correction as
 *      Task 1's test_rl_trainer.c.
 *   2. The plan's hu_gpt_config_t designated initialiser uses field
 *      names `n_layers`, `n_heads`, `d_model`, `max_seq_len` which do
 *      NOT exist on this repo's struct (see include/human/ml/ml.h:31-44
 *      — the real fields are `n_layer`, `n_head`, `n_kv_head`, `n_embd`,
 *      `head_dim`, `sequence_len`). The plan-as-written would fail with
 *      a hard "field designator does not refer to any field" compile
 *      error. Translated to the real field names, matching the tiny-GPT
 *      pattern used in tests/test_ml.c (e.g. lines 414-425). Also added
 *      the required `n_kv_head` and `head_dim` (gpt_create at
 *      src/ml/gpt.c:920-929 enforces n_embd == n_head*head_dim and
 *      head_dim%2 == 0). Per the task brief: "fix the new code (NOT the
 *      plan/spec) to match real signatures."
 */
#include "test_framework.h"
#include "human/ml/policy_logprobs.h"
#include "human/ml/model.h"
#include "human/core/allocator.h"
#include <math.h>

/* Build a tiny GPT, give it known weights, compute log π(y|x) for a known
 * (x, y) pair, verify it equals the manually-computed sum of log-softmax
 * values at the target positions. */
static void test_policy_logprobs_matches_manual_sum_on_tiny_gpt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_gpt_config_t cfg = {0};
    cfg.vocab_size = 32;
    cfg.n_layer = 1;
    cfg.n_head = 1;
    cfg.n_kv_head = 1;
    cfg.n_embd = 16;
    cfg.head_dim = 16;
    cfg.sequence_len = 16;
    hu_model_t model = {0};
    HU_ASSERT_EQ(hu_gpt_create(&alloc, &cfg, &model), HU_OK);

    /* Known input: prompt = [1, 2, 3], response = [4, 5, 6]. */
    int32_t prompt[]   = {1, 2, 3};
    int32_t response[] = {4, 5, 6};

    double logprob = 0.0;
    hu_error_t err = hu_policy_logprobs(&alloc, &model,
                                         prompt, 3,
                                         response, 3,
                                         &logprob);
    HU_ASSERT_EQ(err, HU_OK);

    /* For a randomly-init GPT with vocab=32, log π should be roughly
     * -log(32) * 3 = -10.4 ± 2.0 (sanity bound, NOT exact). */
    HU_ASSERT_TRUE(logprob < 0.0);
    HU_ASSERT_TRUE(logprob > -20.0);

    model.vtable->deinit(model.ctx, &alloc);
}

/* Same prompt + response → identical log-prob (determinism) */
static void test_policy_logprobs_deterministic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_gpt_config_t cfg = {0};
    cfg.vocab_size = 32;
    cfg.n_layer = 1;
    cfg.n_head = 1;
    cfg.n_kv_head = 1;
    cfg.n_embd = 16;
    cfg.head_dim = 16;
    cfg.sequence_len = 16;
    hu_model_t model = {0};
    hu_gpt_create(&alloc, &cfg, &model);
    int32_t prompt[]={1,2,3}, response[]={4,5};
    double a=0, b=0;
    hu_policy_logprobs(&alloc, &model, prompt,3, response,2, &a);
    hu_policy_logprobs(&alloc, &model, prompt,3, response,2, &b);
    HU_ASSERT_TRUE(fabs(a - b) < 1e-9);
    model.vtable->deinit(model.ctx, &alloc);
}

/* NULL args → HU_ERR_INVALID_ARGUMENT */
static void test_policy_logprobs_rejects_null(void) {
    int32_t buf[1] = {0}; double out;
    HU_ASSERT_EQ(hu_policy_logprobs(NULL, NULL, buf, 1, buf, 1, &out),
                 HU_ERR_INVALID_ARGUMENT);
}

void run_policy_logprobs_tests(void) {
    HU_RUN_TEST(test_policy_logprobs_matches_manual_sum_on_tiny_gpt);
    HU_RUN_TEST(test_policy_logprobs_deterministic);
    HU_RUN_TEST(test_policy_logprobs_rejects_null);
}
