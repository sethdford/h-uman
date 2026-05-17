/* US-7.10 — SimPO loss head tests.
 *
 * AC-7.10.1: factory + vtable shape (compute_loss, train_step, deinit).
 * AC-7.10.2: golden loss within 1e-4 of analytical (load-bearing).
 * AC-7.10.6: compiles -Wall -Wextra -Wpedantic -Werror, zero ASan.
 *
 * The golden fixture (mirrored from `tests/fixtures/simpo_golden.json`)
 * is hard-coded here so the test never depends on cwd or filesystem
 * I/O. The JSON file remains the human-readable reference and includes
 * the Python derivation in its `_comment` field for reviewer audit. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/rl_trainer.h"

#include "test_framework.h"

#include <math.h>
#include <stddef.h>

/* Mirror of tests/fixtures/simpo_golden.json. */
static const double GOLDEN_CHOSEN_LOGPROB_SUM = -10.0;
static const size_t GOLDEN_CHOSEN_TOKEN_COUNT = 5;
static const double GOLDEN_REJECTED_LOGPROB_SUM = -30.0;
static const size_t GOLDEN_REJECTED_TOKEN_COUNT = 10;
static const float GOLDEN_BETA = 0.1f;
static const float GOLDEN_GAMMA = 0.5f;
/* Computed offline: sig = 1/(1+exp(0.4)); loss = -log(sig). */
static const double GOLDEN_EXPECTED_LOSS = 0.9130152523999526;

/* AC-7.10.1: factory creates a trainer with type tag and all three
 * vtable pointers non-NULL. Header parse is implicit via #include. */
static void test_simpo_factory_creates_valid_trainer(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_simpo_config_t cfg = {
        .beta = GOLDEN_BETA,
        .gamma = GOLDEN_GAMMA,
        .model = NULL,
    };
    hu_rl_trainer_t trainer = {0};
    HU_ASSERT_EQ(hu_rl_trainer_simpo_create(&alloc, &cfg, &trainer), HU_OK);
    HU_ASSERT_NOT_NULL(trainer.ctx);
    HU_ASSERT_NOT_NULL((void *)trainer.vtable);
    HU_ASSERT_NOT_NULL((void *)trainer.vtable->compute_loss);
    HU_ASSERT_NOT_NULL((void *)trainer.vtable->train_step);
    HU_ASSERT_NOT_NULL((void *)trainer.vtable->deinit);
    HU_ASSERT_EQ((int)trainer.type, (int)HU_RL_TRAINER_SIMPO);
    hu_rl_trainer_deinit(&trainer);
}

/* AC-7.10.2 — load-bearing golden test. Calls compute_loss with the
 * injected logprobs from the fixture and asserts |loss - expected| <
 * 1e-4. Pure-double precision throughout — no model forward. */
static void test_simpo_loss_golden(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_simpo_config_t cfg = {
        .beta = GOLDEN_BETA,
        .gamma = GOLDEN_GAMMA,
        .model = NULL,
    };
    hu_rl_trainer_t trainer = {0};
    HU_ASSERT_EQ(hu_rl_trainer_simpo_create(&alloc, &cfg, &trainer), HU_OK);

    hu_pref_pair_logprobs_t lp = {
        .chosen_logprob_sum = GOLDEN_CHOSEN_LOGPROB_SUM,
        .chosen_token_count = GOLDEN_CHOSEN_TOKEN_COUNT,
        .rejected_logprob_sum = GOLDEN_REJECTED_LOGPROB_SUM,
        .rejected_token_count = GOLDEN_REJECTED_TOKEN_COUNT,
    };
    double loss = 0.0;
    HU_ASSERT_EQ(trainer.vtable->compute_loss(trainer.ctx, &lp, &loss), HU_OK);
    HU_ASSERT_FLOAT_EQ(loss, GOLDEN_EXPECTED_LOSS, 1e-4);

    hu_rl_trainer_deinit(&trainer);
}

/* Defensive: large negative logits drive sigmoid below the 1e-10 floor.
 * Loss must remain finite (no NaN/Inf). Mirrors the DPO guard at
 * src/ml/dpo.c:506. */
static void test_simpo_loss_floor(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_simpo_config_t cfg = {
        .beta = 1.0f, /* magnify the gap */
        .gamma = 0.0f,
        .model = NULL,
    };
    hu_rl_trainer_t trainer = {0};
    HU_ASSERT_EQ(hu_rl_trainer_simpo_create(&alloc, &cfg, &trainer), HU_OK);

    /* avg_chosen = -1000, avg_rejected = +1000 → diff = -2000 →
     * logits = -2000 → sigmoid ≈ 0 → -log(floor) ≈ 23.02. */
    hu_pref_pair_logprobs_t lp = {
        .chosen_logprob_sum = -1000.0,
        .chosen_token_count = 1,
        .rejected_logprob_sum = 1000.0,
        .rejected_token_count = 1,
    };
    double loss = 0.0;
    HU_ASSERT_EQ(trainer.vtable->compute_loss(trainer.ctx, &lp, &loss), HU_OK);
    HU_ASSERT(isfinite(loss));
    /* -log(1e-10) ≈ 23.025850929940457. */
    HU_ASSERT_FLOAT_EQ(loss, 23.025850929940457, 1e-9);

    hu_rl_trainer_deinit(&trainer);
}

/* hu_rl_trainer_deinit must be safe to call twice. */
static void test_simpo_deinit_idempotent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_simpo_config_t cfg = {
        .beta = GOLDEN_BETA,
        .gamma = GOLDEN_GAMMA,
        .model = NULL,
    };
    hu_rl_trainer_t trainer = {0};
    HU_ASSERT_EQ(hu_rl_trainer_simpo_create(&alloc, &cfg, &trainer), HU_OK);
    hu_rl_trainer_deinit(&trainer);
    HU_ASSERT_NULL((void *)trainer.vtable);
    HU_ASSERT_NULL(trainer.ctx);
    /* Second call must be a no-op (no double-free, no crash). */
    hu_rl_trainer_deinit(&trainer);
    HU_ASSERT_NULL((void *)trainer.vtable);
}

/* Factory rejects invalid configurations. */
static void test_simpo_factory_invalid_config_rejects(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_t trainer = {0};

    /* NULL config. */
    HU_ASSERT_EQ(hu_rl_trainer_simpo_create(&alloc, NULL, &trainer), HU_ERR_INVALID_ARGUMENT);
    /* NULL out. */
    hu_simpo_config_t cfg = {.beta = 0.1f, .gamma = 0.5f, .model = NULL};
    HU_ASSERT_EQ(hu_rl_trainer_simpo_create(&alloc, &cfg, NULL), HU_ERR_INVALID_ARGUMENT);
    /* beta <= 0. */
    cfg.beta = 0.0f;
    HU_ASSERT_EQ(hu_rl_trainer_simpo_create(&alloc, &cfg, &trainer), HU_ERR_INVALID_ARGUMENT);
    cfg.beta = -0.1f;
    HU_ASSERT_EQ(hu_rl_trainer_simpo_create(&alloc, &cfg, &trainer), HU_ERR_INVALID_ARGUMENT);
    /* gamma < 0. */
    cfg.beta = 0.1f;
    cfg.gamma = -0.01f;
    HU_ASSERT_EQ(hu_rl_trainer_simpo_create(&alloc, &cfg, &trainer), HU_ERR_INVALID_ARGUMENT);
}

/* compute_loss rejects null arguments and zero-length sequences. */
static void test_simpo_compute_loss_input_validation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_simpo_config_t cfg = {.beta = 0.1f, .gamma = 0.5f, .model = NULL};
    hu_rl_trainer_t trainer = {0};
    HU_ASSERT_EQ(hu_rl_trainer_simpo_create(&alloc, &cfg, &trainer), HU_OK);

    hu_pref_pair_logprobs_t lp = {
        .chosen_logprob_sum = -1.0,
        .chosen_token_count = 1,
        .rejected_logprob_sum = -2.0,
        .rejected_token_count = 1,
    };
    double loss = 0.0;

    HU_ASSERT_EQ(trainer.vtable->compute_loss(NULL, &lp, &loss), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(trainer.vtable->compute_loss(trainer.ctx, NULL, &loss), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(trainer.vtable->compute_loss(trainer.ctx, &lp, NULL), HU_ERR_INVALID_ARGUMENT);

    /* Zero-length chosen sequence → divide-by-zero hazard. */
    hu_pref_pair_logprobs_t bad = lp;
    bad.chosen_token_count = 0;
    HU_ASSERT_EQ(trainer.vtable->compute_loss(trainer.ctx, &bad, &loss), HU_ERR_INVALID_ARGUMENT);
    bad = lp;
    bad.rejected_token_count = 0;
    HU_ASSERT_EQ(trainer.vtable->compute_loss(trainer.ctx, &bad, &loss), HU_ERR_INVALID_ARGUMENT);

    hu_rl_trainer_deinit(&trainer);
}

/* hu_rl_trainer_type_name returns the expected strings. */
static void test_rl_trainer_type_name(void) {
    HU_ASSERT_STR_EQ(hu_rl_trainer_type_name(HU_RL_TRAINER_DPO), "dpo");
    HU_ASSERT_STR_EQ(hu_rl_trainer_type_name(HU_RL_TRAINER_SIMPO), "simpo");
    HU_ASSERT_STR_EQ(hu_rl_trainer_type_name(HU_RL_TRAINER_ORPO), "orpo");
    HU_ASSERT_STR_EQ(hu_rl_trainer_type_name(HU_RL_TRAINER_GRPO2), "grpo2");
}

void run_rl_trainer_simpo_tests(void) {
    HU_TEST_SUITE("RlTrainerSimpo");
    HU_RUN_TEST(test_simpo_factory_creates_valid_trainer);
    HU_RUN_TEST(test_simpo_loss_golden);
    HU_RUN_TEST(test_simpo_loss_floor);
    HU_RUN_TEST(test_simpo_deinit_idempotent);
    HU_RUN_TEST(test_simpo_factory_invalid_config_rejects);
    HU_RUN_TEST(test_simpo_compute_loss_input_validation);
    HU_RUN_TEST(test_rl_trainer_type_name);
}
