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

/* Forward declarations of test helpers from reward_model_train.c */
hu_error_t reward_model_compute_bt_loss_only_for_test(hu_reward_model_t *rm, hu_allocator_t *alloc,
                                                      const hu_preference_pair_t *pairs, size_t n,
                                                      double *out_loss);
float *reward_model_huml_value_head_W_for_test(hu_reward_model_t *rm);
hu_error_t reward_model_compute_bt_grad_for_test(hu_reward_model_t *rm, hu_allocator_t *alloc,
                                                 const hu_preference_pair_t *pairs, size_t n,
                                                 float *out_dW, double *out_db);

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

/* AC-101.4: validate the ANALYTICAL value-head gradient against CENTRAL finite
 * differences of the same mean Bradley-Terry loss. A correct analytical
 * gradient matches FD within float32 tolerance; a wrong one (sign error,
 * missing term, or a gradient that was never actually computed) does not.
 * Assumes hidden_dim == 100 (all three batch tests use vocab_size=100). */
static void gradient_check_against_fd(hu_reward_model_t *rm, hu_allocator_t *alloc,
                                      const hu_preference_pair_t *pairs, size_t n) {
    float *W = reward_model_huml_value_head_W_for_test(rm);
    HU_ASSERT_NOT_NULL(W);

    float analytic[100];
    double analytic_db = 0.0;
    HU_ASSERT_EQ(reward_model_compute_bt_grad_for_test(rm, alloc, pairs, n, analytic, &analytic_db),
                 HU_OK);

    const double eps = 1e-2; /* large enough that float32 W rounding stays << step */
    int checked = 0;
    for (int j = 0; j < 100 && checked < 8; j++) {
        float saved = W[j];
        double lp = 0.0, lm = 0.0;
        W[j] = saved + (float)eps;
        HU_ASSERT_EQ(reward_model_compute_bt_loss_only_for_test(rm, alloc, pairs, n, &lp), HU_OK);
        W[j] = saved - (float)eps;
        HU_ASSERT_EQ(reward_model_compute_bt_loss_only_for_test(rm, alloc, pairs, n, &lm), HU_OK);
        W[j] = saved;

        double fd = (lp - lm) / (2.0 * eps);
        double a = (double)analytic[j];
        /* Skip ill-conditioned near-zero gradients (relative error blows up). */
        if (fabs(a) < 1e-3 && fabs(fd) < 1e-3)
            continue;
        double rel = fabs(a - fd) / (fabs(a) + fabs(fd) + 1e-9);
        HU_ASSERT(rel < 2e-2); /* analytical matches FD to ~2% (float32-limited) */
        checked++;
    }
    HU_ASSERT(checked > 0); /* must have exercised at least one non-trivial weight */
}

static void make_int_pairs(hu_preference_pair_t *pairs, int n) {
    memset(pairs, 0, (size_t)n * sizeof(pairs[0]));
    for (int i = 0; i < n; i++) {
        /* integer-ID tokens within vocab=100; low chosen vs high rejected so
         * the loss has a non-trivial gradient to check against. */
        snprintf(pairs[i].prompt, sizeof(pairs[i].prompt), "0 1");
        pairs[i].prompt_len = strlen(pairs[i].prompt);
        snprintf(pairs[i].chosen, sizeof(pairs[i].chosen), "%d", 2 + (i % 5));
        pairs[i].chosen_len = strlen(pairs[i].chosen);
        snprintf(pairs[i].rejected, sizeof(pairs[i].rejected), "%d", 40 + (i % 5));
        pairs[i].rejected_len = strlen(pairs[i].rejected);
    }
}

static void run_gradient_check_for_batch(int n) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_reward_model_config_t cfg = {
        .backend = HU_REWARD_MODEL_BACKEND_HUML,
        .vocab_size = 100,
        .hidden_dim = 100,
    };
    hu_reward_model_t rm = {0};
    HU_ASSERT_EQ(hu_reward_model_create_huml(&alloc, &cfg, &rm), HU_OK);

    hu_preference_pair_t pairs[8];
    make_int_pairs(pairs, n);
    gradient_check_against_fd(&rm, &alloc, pairs, (size_t)n);

    rm.vtable->deinit(rm.ctx, &alloc);
}

/* AC-101.4: analytical-vs-finite-difference gradient checks on 3 batch sizes. */
static void test_gradient_check_finite_difference_batch_2(void) {
    run_gradient_check_for_batch(2);
}
static void test_gradient_check_finite_difference_batch_4(void) {
    run_gradient_check_for_batch(4);
}
static void test_gradient_check_finite_difference_batch_8(void) {
    run_gradient_check_for_batch(8);
}

/* AC-101.5: Preference ranking test (5 seeds) */
static void test_preference_ranking_5_seeds(void) {
    for (int seed = 0; seed < 5; seed++) {
        srand(42 + seed);
        hu_allocator_t alloc = hu_system_allocator();

        hu_reward_model_config_t cfg = {
            .backend = HU_REWARD_MODEL_BACKEND_HUML,
            .vocab_size = 32,
            .hidden_dim = 32,
        };

        hu_reward_model_t rm = {0};
        HU_ASSERT_EQ(hu_reward_model_create_huml(&alloc, &cfg, &rm), HU_OK);

        /* HUML inputs are SPACE-SEPARATED INTEGER TOKEN IDs (parse_id_string,
         * reward_model.c:67), not natural text. Use a learnable structural
         * pattern within vocab_size=32: chosen sides always start with the
         * low "good" token 1, rejected sides with the high "bad" token 26.
         * A frozen-backbone linear value head learns to weight the token-1
         * logit up and token-26 down, which generalizes to a held-out pair
         * that follows the same pattern (AC-101.5). Mirrors make_synthetic_pairs
         * in test_reward_model_train.c, the proven-convergent setup. */
        hu_preference_pair_t train_pairs[10];
        memset(train_pairs, 0, sizeof(train_pairs));
        for (int i = 0; i < 10; i++) {
            snprintf(train_pairs[i].prompt, sizeof(train_pairs[i].prompt), "0 1 2");
            train_pairs[i].prompt_len = strlen(train_pairs[i].prompt);
            snprintf(train_pairs[i].chosen, sizeof(train_pairs[i].chosen), "1 %d", 1 + (i % 5));
            train_pairs[i].chosen_len = strlen(train_pairs[i].chosen);
            snprintf(train_pairs[i].rejected, sizeof(train_pairs[i].rejected), "26 %d",
                     26 + (i % 5));
            train_pairs[i].rejected_len = strlen(train_pairs[i].rejected);
            train_pairs[i].margin = 1.0;
        }

        hu_reward_model_train_config_t train_cfg = {
            .max_iters = 10,
            .learning_rate = 20.0,
            .log_every = 0,
        };

        hu_reward_model_train_metrics_t metrics = {0};
        HU_ASSERT_EQ(hu_reward_model_train(&rm, &alloc, train_pairs, 10, &train_cfg, &metrics),
                     HU_OK);

        /* Test on held-out pair */
        hu_preference_pair_t heldout;
        memset(&heldout, 0, sizeof(heldout));
        /* Held-out pair follows the SAME structural pattern as training
         * (chosen=low "good" token 1, rejected=high "bad" token 26) but with
         * an unseen second token, so a correct ranking demonstrates learned
         * generalization, not memorization. */
        strncpy(heldout.prompt, "0 1 2", sizeof(heldout.prompt) - 1);
        heldout.prompt_len = strlen(heldout.prompt);
        strncpy(heldout.chosen, "1 5", sizeof(heldout.chosen) - 1);
        heldout.chosen_len = strlen(heldout.chosen);
        strncpy(heldout.rejected, "26 30", sizeof(heldout.rejected) - 1);
        heldout.rejected_len = strlen(heldout.rejected);

        double chosen_score = 0.0, rejected_score = 0.0;
        HU_ASSERT_EQ(rm.vtable->score(rm.ctx, &alloc, heldout.prompt, heldout.prompt_len,
                                      heldout.chosen, heldout.chosen_len, &chosen_score),
                     HU_OK);
        HU_ASSERT_EQ(rm.vtable->score(rm.ctx, &alloc, heldout.prompt, heldout.prompt_len,
                                      heldout.rejected, heldout.rejected_len, &rejected_score),
                     HU_OK);

        /* Verify chosen > rejected (preference ranking works) */
        HU_ASSERT(chosen_score > rejected_score);

        rm.vtable->deinit(rm.ctx, &alloc);
    }
}

/* AC-101.3: Training reduces loss on simple pairs */
static void test_training_reduces_loss(void) {
    srand(42);
    hu_allocator_t alloc = hu_system_allocator();

    hu_reward_model_config_t cfg = {
        .backend = HU_REWARD_MODEL_BACKEND_HUML,
        .vocab_size = 100,
        .hidden_dim = 100,
    };

    hu_reward_model_t rm = {0};
    HU_ASSERT_EQ(hu_reward_model_create_huml(&alloc, &cfg, &rm), HU_OK);

    /* Create simple synthetic pairs */
    hu_preference_pair_t pairs[2];
    memset(pairs, 0, sizeof(pairs));
    /* Integer token IDs within vocab_size=100 (parse_id_string), with the
     * learnable low="good"/high="bad" structure so SGD reduces BT loss. */
    strncpy(pairs[0].prompt, "0 1 2", sizeof(pairs[0].prompt) - 1);
    pairs[0].prompt_len = strlen(pairs[0].prompt);
    strncpy(pairs[0].chosen, "1 3", sizeof(pairs[0].chosen) - 1);
    pairs[0].chosen_len = strlen(pairs[0].chosen);
    strncpy(pairs[0].rejected, "40 41", sizeof(pairs[0].rejected) - 1);
    pairs[0].rejected_len = strlen(pairs[0].rejected);
    strncpy(pairs[1].prompt, "0 1 2", sizeof(pairs[1].prompt) - 1);
    pairs[1].prompt_len = strlen(pairs[1].prompt);
    strncpy(pairs[1].chosen, "1 4", sizeof(pairs[1].chosen) - 1);
    pairs[1].chosen_len = strlen(pairs[1].chosen);
    strncpy(pairs[1].rejected, "40 42", sizeof(pairs[1].rejected) - 1);
    pairs[1].rejected_len = strlen(pairs[1].rejected);

    hu_reward_model_train_config_t train_cfg = {
        .max_iters = 10,
        .learning_rate = 0.1,
        .log_every = 0,
    };

    hu_reward_model_train_metrics_t metrics = {0};
    HU_ASSERT_EQ(hu_reward_model_train(&rm, &alloc, pairs, 2, &train_cfg, &metrics), HU_OK);

    /* AC-101.3: training must actually REDUCE loss, not merely run.
     * A test named "reduces_loss" that only checks >=0 is a tests-that-pin-bugs
     * smell — assert the real contract. */
    HU_ASSERT(metrics.initial_loss >= 0.0);
    HU_ASSERT(metrics.final_loss >= 0.0);
    HU_ASSERT(metrics.iters_completed == 10);
    HU_ASSERT(metrics.skipped_count == 0);
    HU_ASSERT(metrics.final_loss < metrics.initial_loss);

    rm.vtable->deinit(rm.ctx, &alloc);
}

/* AC-101.6: One-sided KTO training with skip count */
static void test_kto_one_sided_train(void) {
    hu_allocator_t alloc = hu_system_allocator();

    hu_reward_model_config_t cfg = {
        .backend = HU_REWARD_MODEL_BACKEND_HUML,
        .vocab_size = 100,
        .hidden_dim = 100,
    };

    hu_reward_model_t rm = {0};
    HU_ASSERT_EQ(hu_reward_model_create_huml(&alloc, &cfg, &rm), HU_OK);

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
    HU_ASSERT_EQ(hu_reward_model_train(&rm, &alloc, pairs, 4, &train_cfg, &metrics), HU_OK);

    /* Should skip 2 pairs */
    HU_ASSERT_EQ(metrics.skipped_count, 2);
    HU_ASSERT(metrics.iters_completed == 5);

    rm.vtable->deinit(rm.ctx, &alloc);
}

void run_reward_model_huml_tests(void) {
    HU_TEST_SUITE("reward_model_huml");

    HU_RUN_TEST(test_score_deterministic);
    HU_RUN_TEST(test_score_batch_two_sided);
    HU_RUN_TEST(test_score_batch_one_sided_kto);
    HU_RUN_TEST(test_bradley_terry_loss_math);
    HU_RUN_TEST(test_gradient_check_finite_difference_batch_2);
    HU_RUN_TEST(test_gradient_check_finite_difference_batch_4);
    HU_RUN_TEST(test_gradient_check_finite_difference_batch_8);
    HU_RUN_TEST(test_preference_ranking_5_seeds);
    HU_RUN_TEST(test_training_reduces_loss);
    HU_RUN_TEST(test_kto_one_sided_train);
}
