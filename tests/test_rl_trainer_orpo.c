/* US-11.5 — ORPO loss head tests.
 *
 * AC-11.5.2: golden loss within 1e-4 of analytical (load-bearing).
 * AC-11.5.3: OR penalty diminishes as log_pi(chosen) → 0.
 * AC-11.5.6: compiles -Wall -Wextra -Wpedantic -Werror, zero ASan.
 *
 * The golden fixture (mirrored from `tests/fixtures/orpo_golden.json`)
 * is hard-coded here so the test never depends on cwd or filesystem
 * I/O. The JSON file remains the human-readable reference and includes
 * the Python derivation in its `_comment` field for reviewer audit. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/rl_trainer.h"

#include "test_framework.h"

#include <math.h>
#include <stddef.h>

/* Mirror of tests/fixtures/orpo_golden.json. */
static const double GOLDEN_CHOSEN_LOGPROB_SUM = -1.5;
static const size_t GOLDEN_CHOSEN_TOKEN_COUNT = 3;
static const double GOLDEN_REJECTED_LOGPROB_SUM = -4.5;
static const size_t GOLDEN_REJECTED_TOKEN_COUNT = 3;
static const float GOLDEN_LAMBDA = 0.1f;
/* See orpo_golden.json `_comment` for the derivation. */
static const double GOLDEN_EXPECTED_LOSS = 0.5170859217146478;

/* Factory creates a trainer with type tag and all three vtable pointers
 * non-NULL. Mirrors SimPO AC-7.10.1. */
static void test_orpo_factory_creates_valid_trainer(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_orpo_config_t cfg = {
        .lambda = GOLDEN_LAMBDA,
        .model = NULL,
    };
    hu_rl_trainer_t trainer = {0};
    HU_ASSERT_EQ(hu_rl_trainer_orpo_create(&alloc, &cfg, &trainer), HU_OK);
    HU_ASSERT_NOT_NULL(trainer.ctx);
    HU_ASSERT_NOT_NULL((void *)trainer.vtable);
    HU_ASSERT_NOT_NULL((void *)trainer.vtable->compute_loss);
    HU_ASSERT_NOT_NULL((void *)trainer.vtable->train_step);
    HU_ASSERT_NOT_NULL((void *)trainer.vtable->deinit);
    HU_ASSERT_EQ((int)trainer.type, (int)HU_RL_TRAINER_ORPO);
    hu_rl_trainer_deinit(&trainer);
}

/* AC-11.5.2 — load-bearing golden test. Calls compute_loss with the
 * injected logprobs from the fixture and asserts |loss - expected| <
 * 1e-4. Pure-double precision throughout — no model forward. */
static void test_orpo_loss_golden(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_orpo_config_t cfg = {
        .lambda = GOLDEN_LAMBDA,
        .model = NULL,
    };
    hu_rl_trainer_t trainer = {0};
    HU_ASSERT_EQ(hu_rl_trainer_orpo_create(&alloc, &cfg, &trainer), HU_OK);

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

/* AC-11.5.3 — OR penalty shrinks as log_pi(chosen) approaches 0 (model
 * is highly confident on the chosen completion). Two regimes at the
 * same chosen/rejected margin (1.0) — high-confidence chosen produces
 * a smaller OR term. Computed offline in Python: low-conf or_p≈0.2887,
 * high-conf or_p≈0.0057 (~50x smaller). */
static void test_orpo_or_penalty_diminishes_at_high_log_prob(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_orpo_config_t cfg = {
        .lambda = 1.0f, /* λ=1 isolates the OR term: loss == nll + or_penalty. */
        .model = NULL,
    };
    hu_rl_trainer_t trainer = {0};
    HU_ASSERT_EQ(hu_rl_trainer_orpo_create(&alloc, &cfg, &trainer), HU_OK);

    /* Low-confidence regime: chosen=-2.0, rejected=-3.0 (margin = 1.0). */
    hu_pref_pair_logprobs_t low = {
        .chosen_logprob_sum = -2.0,
        .chosen_token_count = 1,
        .rejected_logprob_sum = -3.0,
        .rejected_token_count = 1,
    };
    double loss_low = 0.0;
    HU_ASSERT_EQ(trainer.vtable->compute_loss(trainer.ctx, &low, &loss_low), HU_OK);
    /* nll_low = 2.0; or_p_low ≈ 0.2887 → loss_low ≈ 2.2887. */
    double or_low = loss_low - 2.0; /* with lambda=1, this isolates the OR term. */

    /* High-confidence regime: chosen=-0.01, rejected=-1.01 (margin = 1.0). */
    hu_pref_pair_logprobs_t high = {
        .chosen_logprob_sum = -0.01,
        .chosen_token_count = 1,
        .rejected_logprob_sum = -1.01,
        .rejected_token_count = 1,
    };
    double loss_high = 0.0;
    HU_ASSERT_EQ(trainer.vtable->compute_loss(trainer.ctx, &high, &loss_high), HU_OK);
    double or_high = loss_high - 0.01;

    /* The well-fit regime must have a strictly smaller OR penalty. */
    HU_ASSERT(or_high < or_low);
    /* And it must be substantially smaller (at least 10x). */
    HU_ASSERT(or_high * 10.0 < or_low);
    /* And it must remain non-negative (-log σ ≥ 0). */
    HU_ASSERT(or_high >= 0.0);

    hu_rl_trainer_deinit(&trainer);
}

/* AC-11.5.2 fallback path: with rejected_token_count == 0, the OR term
 * is zeroed and the loss reduces to pure NLL on chosen (single-stage
 * SFT-only mode; see design §9 OQ-3). */
static void test_orpo_sft_only_when_no_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_orpo_config_t cfg = {
        .lambda = 0.5f,
        .model = NULL,
    };
    hu_rl_trainer_t trainer = {0};
    HU_ASSERT_EQ(hu_rl_trainer_orpo_create(&alloc, &cfg, &trainer), HU_OK);

    hu_pref_pair_logprobs_t lp = {
        .chosen_logprob_sum = -6.0,
        .chosen_token_count = 3,
        .rejected_logprob_sum = 0.0,
        .rejected_token_count = 0, /* no rejected → SFT-only */
    };
    double loss = 0.0;
    HU_ASSERT_EQ(trainer.vtable->compute_loss(trainer.ctx, &lp, &loss), HU_OK);
    /* nll = -(-6/3) = 2.0; or term zeroed → loss == 2.0. */
    HU_ASSERT_FLOAT_EQ(loss, 2.0, 1e-12);

    hu_rl_trainer_deinit(&trainer);
}

/* Numerical guard: chosen with logprob very close to 0 (model nearly
 * certain) must not produce NaN/Inf via the log1mexp clamp. */
static void test_orpo_loss_floor_at_high_confidence(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_orpo_config_t cfg = {.lambda = 0.1f, .model = NULL};
    hu_rl_trainer_t trainer = {0};
    HU_ASSERT_EQ(hu_rl_trainer_orpo_create(&alloc, &cfg, &trainer), HU_OK);

    /* logp_chosen ≈ 0 (model nearly certain). Without the clamp,
     * log(1 - exp(0)) = log(0) = -inf and the loss explodes. With the
     * clamp the loss is finite. */
    hu_pref_pair_logprobs_t lp = {
        .chosen_logprob_sum = -1e-15, /* effectively 0 in float64 */
        .chosen_token_count = 1,
        .rejected_logprob_sum = -5.0,
        .rejected_token_count = 1,
    };
    double loss = 0.0;
    HU_ASSERT_EQ(trainer.vtable->compute_loss(trainer.ctx, &lp, &loss), HU_OK);
    HU_ASSERT(isfinite(loss));

    hu_rl_trainer_deinit(&trainer);
}

/* hu_rl_trainer_deinit must be safe to call twice. Mirrors SimPO test. */
static void test_orpo_deinit_idempotent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_orpo_config_t cfg = {.lambda = GOLDEN_LAMBDA, .model = NULL};
    hu_rl_trainer_t trainer = {0};
    HU_ASSERT_EQ(hu_rl_trainer_orpo_create(&alloc, &cfg, &trainer), HU_OK);
    hu_rl_trainer_deinit(&trainer);
    HU_ASSERT_NULL((void *)trainer.vtable);
    HU_ASSERT_NULL(trainer.ctx);
    /* Second call must be a no-op (no double-free, no crash). */
    hu_rl_trainer_deinit(&trainer);
    HU_ASSERT_NULL((void *)trainer.vtable);
}

/* Factory rejects invalid configurations. */
static void test_orpo_factory_invalid_lambda_rejects(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_t trainer = {0};

    /* NULL config. */
    HU_ASSERT_EQ(hu_rl_trainer_orpo_create(&alloc, NULL, &trainer), HU_ERR_INVALID_ARGUMENT);
    /* NULL out. */
    hu_orpo_config_t cfg = {.lambda = 0.1f, .model = NULL};
    HU_ASSERT_EQ(hu_rl_trainer_orpo_create(&alloc, &cfg, NULL), HU_ERR_INVALID_ARGUMENT);
    /* lambda <= 0. */
    cfg.lambda = 0.0f;
    HU_ASSERT_EQ(hu_rl_trainer_orpo_create(&alloc, &cfg, &trainer), HU_ERR_INVALID_ARGUMENT);
    cfg.lambda = -0.1f;
    HU_ASSERT_EQ(hu_rl_trainer_orpo_create(&alloc, &cfg, &trainer), HU_ERR_INVALID_ARGUMENT);
}

/* compute_loss rejects null arguments and zero-length chosen sequence
 * (zero-length rejected is the SFT-only fallback, NOT an error). */
static void test_orpo_compute_loss_input_validation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_orpo_config_t cfg = {.lambda = 0.1f, .model = NULL};
    hu_rl_trainer_t trainer = {0};
    HU_ASSERT_EQ(hu_rl_trainer_orpo_create(&alloc, &cfg, &trainer), HU_OK);

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

    /* Zero-length chosen sequence → divide-by-zero hazard → reject. */
    hu_pref_pair_logprobs_t bad = lp;
    bad.chosen_token_count = 0;
    HU_ASSERT_EQ(trainer.vtable->compute_loss(trainer.ctx, &bad, &loss), HU_ERR_INVALID_ARGUMENT);

    hu_rl_trainer_deinit(&trainer);
}

void run_rl_trainer_orpo_tests(void) {
    HU_TEST_SUITE("RlTrainerOrpo");
    HU_RUN_TEST(test_orpo_factory_creates_valid_trainer);
    HU_RUN_TEST(test_orpo_loss_golden);
    HU_RUN_TEST(test_orpo_or_penalty_diminishes_at_high_log_prob);
    HU_RUN_TEST(test_orpo_sft_only_when_no_rejected);
    HU_RUN_TEST(test_orpo_loss_floor_at_high_confidence);
    HU_RUN_TEST(test_orpo_deinit_idempotent);
    HU_RUN_TEST(test_orpo_factory_invalid_lambda_rejects);
    HU_RUN_TEST(test_orpo_compute_loss_input_validation);
}
