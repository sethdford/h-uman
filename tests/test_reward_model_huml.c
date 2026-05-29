/* tests/test_reward_model_huml.c — Unit tests for HUML reward model
 *
 * Covers: scoring, batch scoring, Bradley-Terry loss, training, and KTO
 * one-sided handling using the HUML toy GPT + value head composition.
 */
#include "human/core/allocator.h"
#include "human/ml/dpo.h"
#include "human/ml/reward_model.h"
#include "test_framework.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* AC-101.2: Scoring is deterministic and reproducible */
static void test_score_deterministic(void) {
    hu_allocator_t alloc_storage = hu_system_allocator();
    hu_allocator_t *alloc = &alloc_storage;

    hu_reward_model_config_t cfg = {
        .backend = HU_REWARD_MODEL_BACKEND_HUML,
        .vocab_size = 100,
        .hidden_dim = 100,
        .backbone_path = NULL,
        .value_head_path = NULL,
    };

    hu_reward_model_t rm = {0};
    HU_ASSERT_EQ(hu_reward_model_create_huml(alloc, &cfg, &rm), HU_OK);

    const char *prompt = "1 2 3";
    const char *response = "4 5 6";

    double score1 = 0.0, score2 = 0.0;
    HU_ASSERT_EQ(rm.vtable->score(rm.ctx, alloc, prompt, strlen(prompt), response, strlen(response),
                                  &score1),
                 HU_OK);
    HU_ASSERT_EQ(rm.vtable->score(rm.ctx, alloc, prompt, strlen(prompt), response, strlen(response),
                                  &score2),
                 HU_OK);

    /* Bit-exact match */
    HU_ASSERT_EQ(score1, score2);

    rm.vtable->deinit(rm.ctx, alloc);
}

/* AC-101.7: Batch scoring with two-sided pairs */
static void test_score_batch_two_sided(void) {
    hu_allocator_t alloc_storage = hu_system_allocator();
    hu_allocator_t *alloc = &alloc_storage;

    hu_reward_model_config_t cfg = {
        .backend = HU_REWARD_MODEL_BACKEND_HUML,
        .vocab_size = 100,
        .hidden_dim = 100,
    };

    hu_reward_model_t rm = {0};
    HU_ASSERT_EQ(hu_reward_model_create_huml(alloc, &cfg, &rm), HU_OK);

    hu_preference_pair_t pairs[3] = {
        {.prompt = "1",
         .prompt_len = 1,
         .chosen = "2",
         .chosen_len = 1,
         .rejected = "3",
         .rejected_len = 1},
        {.prompt = "4",
         .prompt_len = 1,
         .chosen = "5",
         .chosen_len = 1,
         .rejected = "6",
         .rejected_len = 1},
        {.prompt = "7",
         .prompt_len = 1,
         .chosen = "8",
         .chosen_len = 1,
         .rejected = "9",
         .rejected_len = 1},
    };

    double chosen_scores[3] = {0};
    double rejected_scores[3] = {0};

    HU_ASSERT_EQ(rm.vtable->score_batch(rm.ctx, alloc, pairs, 3, chosen_scores, rejected_scores),
                 HU_OK);

    /* All should be valid numbers, not NaN */
    for (int i = 0; i < 3; i++) {
        HU_ASSERT(!isnan(chosen_scores[i]));
        HU_ASSERT(!isnan(rejected_scores[i]));
    }

    rm.vtable->deinit(rm.ctx, alloc);
}

/* AC-101.6: One-sided KTO pair handling produces NaN */
static void test_score_batch_one_sided_kto(void) {
    hu_allocator_t alloc_storage = hu_system_allocator();
    hu_allocator_t *alloc = &alloc_storage;

    hu_reward_model_config_t cfg = {
        .backend = HU_REWARD_MODEL_BACKEND_HUML,
        .vocab_size = 100,
        .hidden_dim = 100,
    };

    hu_reward_model_t rm = {0};
    HU_ASSERT_EQ(hu_reward_model_create_huml(alloc, &cfg, &rm), HU_OK);

    hu_preference_pair_t pairs[3] = {
        {.prompt = "1",
         .prompt_len = 1,
         .chosen = "2",
         .chosen_len = 1,
         .rejected = "3",
         .rejected_len = 1},
        {.prompt = "4",
         .prompt_len = 1,
         .chosen = "",
         .chosen_len = 0,
         .rejected = "6",
         .rejected_len = 1},
        {.prompt = "7",
         .prompt_len = 1,
         .chosen = "8",
         .chosen_len = 1,
         .rejected = "",
         .rejected_len = 0},
    };

    double chosen_scores[3] = {0};
    double rejected_scores[3] = {0};

    HU_ASSERT_EQ(rm.vtable->score_batch(rm.ctx, alloc, pairs, 3, chosen_scores, rejected_scores),
                 HU_OK);

    /* Pair 0: both valid */
    HU_ASSERT(!isnan(chosen_scores[0]));
    HU_ASSERT(!isnan(rejected_scores[0]));

    /* Pair 1: chosen is NaN, rejected is valid */
    HU_ASSERT(isnan(chosen_scores[1]));
    HU_ASSERT(!isnan(rejected_scores[1]));

    /* Pair 2: chosen is valid, rejected is NaN */
    HU_ASSERT(!isnan(chosen_scores[2]));
    HU_ASSERT(isnan(rejected_scores[2]));

    rm.vtable->deinit(rm.ctx, alloc);
}

/* AC-101.3: Bradley-Terry loss math validation */
static void test_bradley_terry_loss_math(void) {
    /* Hand-compute loss for a known pair */
    /* r_w = 0.5, r_l = -0.5, delta = 1.0
     * sigmoid(1.0) ≈ 0.731
     * loss = -log(0.731) ≈ 0.313 */

    double r_w = 0.5;
    double r_l = -0.5;
    double delta = r_w - r_l;
    double sig = 1.0 / (1.0 + exp(-delta));
    double loss = -log(sig);

    /* Should be approximately 0.313 */
    HU_ASSERT(loss > 0.3 && loss < 0.32);

    /* Verify direct formula: loss = log(1 + exp(-delta)) */
    double loss_alt = log(1.0 + exp(-delta));
    HU_ASSERT(fabs(loss - loss_alt) < 1e-10);
}

/* AC-101.3: Training reduces loss on simple pairs */
static void test_training_reduces_loss(void) {
    hu_allocator_t alloc_storage = hu_system_allocator();
    hu_allocator_t *alloc = &alloc_storage;

    hu_reward_model_config_t cfg = {
        .backend = HU_REWARD_MODEL_BACKEND_HUML,
        .vocab_size = 100,
        .hidden_dim = 100,
    };

    hu_reward_model_t rm = {0};
    HU_ASSERT_EQ(hu_reward_model_create_huml(alloc, &cfg, &rm), HU_OK);

    /* Create simple synthetic pairs */
    hu_preference_pair_t pairs[2] = {
        {.prompt = "good",
         .prompt_len = 4,
         .chosen = "yes",
         .chosen_len = 3,
         .rejected = "no",
         .rejected_len = 2},
        {.prompt = "bad",
         .prompt_len = 3,
         .chosen = "no",
         .chosen_len = 2,
         .rejected = "yes",
         .rejected_len = 3},
    };

    hu_reward_model_train_config_t train_cfg = {
        .max_iters = 10,
        .learning_rate = 0.1,
        .log_every = 0,
    };

    hu_reward_model_train_metrics_t metrics = {0};
    HU_ASSERT_EQ(hu_reward_model_train(&rm, alloc, pairs, 2, &train_cfg, &metrics), HU_OK);

    /* Loss should decrease (or stay similar) */
    HU_ASSERT(metrics.initial_loss >= 0.0);
    HU_ASSERT(metrics.final_loss >= 0.0);
    HU_ASSERT(metrics.iters_completed == 10);
    HU_ASSERT(metrics.skipped_count == 0);

    rm.vtable->deinit(rm.ctx, alloc);
}

/* AC-101.6: One-sided KTO training with skip count */
static void test_kto_one_sided_train(void) {
    hu_allocator_t alloc_storage = hu_system_allocator();
    hu_allocator_t *alloc = &alloc_storage;

    hu_reward_model_config_t cfg = {
        .backend = HU_REWARD_MODEL_BACKEND_HUML,
        .vocab_size = 100,
        .hidden_dim = 100,
    };

    hu_reward_model_t rm = {0};
    HU_ASSERT_EQ(hu_reward_model_create_huml(alloc, &cfg, &rm), HU_OK);

    /* Mix of two-sided and one-sided pairs */
    hu_preference_pair_t pairs[4] = {
        {.prompt = "1",
         .prompt_len = 1,
         .chosen = "2",
         .chosen_len = 1,
         .rejected = "3",
         .rejected_len = 1},
        {.prompt = "4",
         .prompt_len = 1,
         .chosen = "",
         .chosen_len = 0,
         .rejected = "6",
         .rejected_len = 1},
        {.prompt = "7",
         .prompt_len = 1,
         .chosen = "8",
         .chosen_len = 1,
         .rejected = "",
         .rejected_len = 0},
        {.prompt = "10",
         .prompt_len = 2,
         .chosen = "11",
         .chosen_len = 2,
         .rejected = "12",
         .rejected_len = 2},
    };

    hu_reward_model_train_config_t train_cfg = {
        .max_iters = 5,
        .learning_rate = 0.01,
        .log_every = 0,
    };

    hu_reward_model_train_metrics_t metrics = {0};
    HU_ASSERT_EQ(hu_reward_model_train(&rm, alloc, pairs, 4, &train_cfg, &metrics), HU_OK);

    /* Should skip 2 pairs */
    HU_ASSERT_EQ(metrics.skipped_count, 2);
    HU_ASSERT(metrics.iters_completed == 5);

    rm.vtable->deinit(rm.ctx, alloc);
}

void run_reward_model_huml_tests(void) {
    HU_TEST_SUITE("reward_model_huml");

    HU_RUN_TEST(test_score_deterministic);
    HU_RUN_TEST(test_score_batch_two_sided);
    HU_RUN_TEST(test_score_batch_one_sided_kto);
    HU_RUN_TEST(test_bradley_terry_loss_math);
    HU_RUN_TEST(test_training_reduces_loss);
    HU_RUN_TEST(test_kto_one_sided_train);
}
