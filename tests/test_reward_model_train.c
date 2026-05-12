/* tests/test_reward_model_train.c — Phase 3 Task 2 (smoke portion)
 *
 * Pins the hu_reward_model_t HUML composition + M3 NaN contract for
 * one-sided KTO pairs. Two invariants in this task; Task 3 will APPEND
 * Bradley-Terry convergence + finite-diff grad-check tests to the same
 * file (the suite name "reward_model" is shared).
 *
 *   1. Smoke: create an HUML RM, score one (prompt, response), assert
 *      the score is a finite double. Per R4, the HUML RM is NOT scoped
 *      to producing semantically-correct scores — only to validating the
 *      linear projection + backbone forward composition end-to-end.
 *      Semantic plausibility lives on the MLX-Qwen path (Task 8).
 *
 *   2. M3 NaN contract: score_batch on a one-sided KTO pair
 *      (rejected_len == 0 OR chosen_len == 0) must write NaN to the
 *      empty slot and a finite score to the populated slot. This pins
 *      the contract documented in include/human/ml/reward_model.h so
 *      Task 3's Bradley-Terry trainer can filter mixed KTO+DPO batches
 *      with a single isnan() check.
 */
#include "test_framework.h"

#include "human/core/allocator.h"
#include "human/ml/dpo.h"
#include "human/ml/reward_model.h"

#include <math.h>
#include <string.h>

static void test_reward_model_huml_smoke_score_returns_finite_double(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_reward_model_config_t cfg = {
        .backend = HU_REWARD_MODEL_BACKEND_HUML,
        .vocab_size = 32,
        .hidden_dim = 32,
    };
    hu_reward_model_t rm = {0};
    HU_ASSERT_EQ(hu_reward_model_create_huml(&alloc, &cfg, &rm), HU_OK);
    HU_ASSERT_NOT_NULL(rm.vtable);

    const char *prompt = "1 2 3";
    const char *response = "4 5";
    double score = NAN;
    HU_ASSERT_EQ(rm.vtable->score(rm.ctx, &alloc, prompt, strlen(prompt),
                                   response, strlen(response), &score),
                 HU_OK);
    HU_ASSERT_TRUE(isfinite(score));

    rm.vtable->deinit(rm.ctx, &alloc);
}

static void test_reward_model_huml_score_batch_one_sided_writes_nan(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_reward_model_config_t cfg = {
        .backend = HU_REWARD_MODEL_BACKEND_HUML,
        .vocab_size = 32,
        .hidden_dim = 32,
    };
    hu_reward_model_t rm = {0};
    HU_ASSERT_EQ(hu_reward_model_create_huml(&alloc, &cfg, &rm), HU_OK);

    /* Three pairs: full DPO, KTO chosen-only (rejected empty), KTO
     * rejected-only (chosen empty). Mirrors the mixed batch shape Task 3
     * will hand to the trainer. */
    hu_preference_pair_t pairs[3];
    memset(pairs, 0, sizeof(pairs));

    strncpy(pairs[0].prompt, "1 2 3", sizeof(pairs[0].prompt) - 1);
    pairs[0].prompt_len = strlen(pairs[0].prompt);
    strncpy(pairs[0].chosen, "4 5", sizeof(pairs[0].chosen) - 1);
    pairs[0].chosen_len = strlen(pairs[0].chosen);
    strncpy(pairs[0].rejected, "6 7", sizeof(pairs[0].rejected) - 1);
    pairs[0].rejected_len = strlen(pairs[0].rejected);

    strncpy(pairs[1].prompt, "8 9", sizeof(pairs[1].prompt) - 1);
    pairs[1].prompt_len = strlen(pairs[1].prompt);
    strncpy(pairs[1].chosen, "10 11", sizeof(pairs[1].chosen) - 1);
    pairs[1].chosen_len = strlen(pairs[1].chosen);
    /* pairs[1].rejected left as empty string with length 0 */

    strncpy(pairs[2].prompt, "12 13", sizeof(pairs[2].prompt) - 1);
    pairs[2].prompt_len = strlen(pairs[2].prompt);
    /* pairs[2].chosen left empty (len 0) */
    strncpy(pairs[2].rejected, "14 15", sizeof(pairs[2].rejected) - 1);
    pairs[2].rejected_len = strlen(pairs[2].rejected);

    double chosen[3] = {0.0, 0.0, 0.0};
    double rejected[3] = {0.0, 0.0, 0.0};
    HU_ASSERT_EQ(rm.vtable->score_batch(rm.ctx, &alloc, pairs, 3,
                                         chosen, rejected),
                 HU_OK);

    HU_ASSERT_TRUE(isfinite(chosen[0]));
    HU_ASSERT_TRUE(isfinite(rejected[0]));

    HU_ASSERT_TRUE(isfinite(chosen[1]));
    HU_ASSERT_TRUE(isnan(rejected[1]));

    HU_ASSERT_TRUE(isnan(chosen[2]));
    HU_ASSERT_TRUE(isfinite(rejected[2]));

    rm.vtable->deinit(rm.ctx, &alloc);
}

void run_reward_model_train_tests(void) {
    HU_TEST_SUITE("reward_model");
    HU_RUN_TEST(test_reward_model_huml_smoke_score_returns_finite_double);
    HU_RUN_TEST(test_reward_model_huml_score_batch_one_sided_writes_nan);
    /* Task 3 will append more HU_RUN_TEST calls here (Bradley-Terry
     * convergence on a 3-pair fixture; finite-diff grad check on the
     * value head against the BT loss). */
}
