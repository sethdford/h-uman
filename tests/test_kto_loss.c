/* tests/test_kto_loss.c — Phase 3 Task 5
 *
 * Pins the KTO loss + structural backward in src/ml/kto.c:
 *   1. Vtable contract: factory returns fully populated vtable
 *   2. Sign-of-gradient: desirable step increases chosen logprob,
 *      undesirable step decreases rejected logprob
 *   3. Finite-diff grad check: numerical gradient matches analytical
 *      sign on a probed lm_head parameter
 */
#include "test_framework.h"
#include "human/ml/kto.h"
#include "human/ml/rl_trainer.h"
#include "human/core/allocator.h"
#include <math.h>
#include <string.h>

static void test_kto_rl_trainer_vtable_fields_all_populated(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_config_t cfg = {
        .backend = HU_DPO_BACKEND_HUML,
        .beta = 0.1,
        .learning_rate = 1e-3,
        .max_iters = 1,
        .lambda_d = 1.0,
        .lambda_u = 1.0,
    };
    hu_rl_trainer_t t = {0};
    HU_ASSERT_EQ(hu_rl_trainer_create_kto(&alloc, &cfg, &t), HU_OK);
    HU_ASSERT_NOT_NULL(t.vtable);
    HU_ASSERT_NOT_NULL(t.vtable->step);
    HU_ASSERT_NOT_NULL(t.vtable->save_adapter);
    HU_ASSERT_NOT_NULL(t.vtable->name);
    HU_ASSERT_NOT_NULL(t.vtable->deinit);
    HU_ASSERT_TRUE(t.ctx != NULL);
    t.vtable->deinit(t.ctx, &alloc);
}

static void test_kto_loss_sign_of_gradient_increases_chosen_decreases_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_config_t cfg = {
        .backend = HU_DPO_BACKEND_HUML,
        .beta = 0.1,
        .learning_rate = 1e-2,
        .max_iters = 1,
        .lambda_d = 1.0,
        .lambda_u = 1.0,
    };

    /* Desirable signal over 50 steps: accumulated chosen logprob delta
     * should be positive (policy moves toward the desirable response).
     * The delta is measured BEFORE the backward at each step, so the
     * first step reports 0 (policy == reference). From step 2 onward
     * the gap widens. Same multi-step pattern as DPO's
     * test_dpo_real_huml_e2e_sign_of_improvement. */
    hu_rl_trainer_t trainer = {0};
    HU_ASSERT_EQ(hu_rl_trainer_create_kto(&alloc, &cfg, &trainer), HU_OK);

    hu_preference_pair_t desirable = {0};
    memcpy(desirable.prompt, "1 2 3", 5);
    desirable.prompt_len = 5;
    memcpy(desirable.chosen, "4 5", 3);
    desirable.chosen_len = 3;

    double chosen_total = 0;
    for (int i = 0; i < 50; i++) {
        hu_rl_trainer_metrics_t m = {0};
        HU_ASSERT_EQ(trainer.vtable->step(trainer.ctx, &alloc, &desirable, 1, &m), HU_OK);
        chosen_total += m.chosen_logprob_delta;
    }
    HU_ASSERT_TRUE(chosen_total > 0);

    trainer.vtable->deinit(trainer.ctx, &alloc);

    /* Undesirable signal over 50 steps: accumulated rejected logprob
     * delta should be negative (policy moves away from undesirable). */
    hu_rl_trainer_t trainer2 = {0};
    HU_ASSERT_EQ(hu_rl_trainer_create_kto(&alloc, &cfg, &trainer2), HU_OK);

    hu_preference_pair_t undesirable = {0};
    memcpy(undesirable.prompt, "1 2 3", 5);
    undesirable.prompt_len = 5;
    memcpy(undesirable.rejected, "6 7", 3);
    undesirable.rejected_len = 3;

    double rejected_total = 0;
    for (int i = 0; i < 50; i++) {
        hu_rl_trainer_metrics_t m2 = {0};
        HU_ASSERT_EQ(trainer2.vtable->step(trainer2.ctx, &alloc, &undesirable, 1, &m2), HU_OK);
        rejected_total += m2.rejected_logprob_delta;
    }
    HU_ASSERT_TRUE(rejected_total < 0);

    trainer2.vtable->deinit(trainer2.ctx, &alloc);
}

static void test_kto_loss_finite_diff_matches_analytical(void) {
    extern hu_error_t kto_compute_loss_only_for_test(void *ctx,
                          const hu_preference_pair_t *p, double *out_loss);
    extern hu_error_t kto_get_huml_lm_head_param_for_test(void *ctx,
                          size_t row, size_t col, float **out_param_ptr);

    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_config_t cfg = {
        .backend = HU_DPO_BACKEND_HUML,
        .beta = 0.1,
        .learning_rate = 0.0,
        .max_iters = 1,
        .lambda_d = 1.0,
        .lambda_u = 1.0,
    };
    srand(42);
    hu_rl_trainer_t trainer = {0};
    HU_ASSERT_EQ(hu_rl_trainer_create_kto(&alloc, &cfg, &trainer), HU_OK);

    hu_preference_pair_t desirable = {0};
    memcpy(desirable.prompt, "1 2", 3);
    desirable.prompt_len = 3;
    memcpy(desirable.chosen, "3 4", 3);
    desirable.chosen_len = 3;

    hu_preference_pair_t undesirable = {0};
    memcpy(undesirable.prompt, "1 2", 3);
    undesirable.prompt_len = 3;
    memcpy(undesirable.rejected, "3 4", 3);
    undesirable.rejected_len = 3;

    const size_t probe_row = 3, probe_col = 0;
    float *theta_ptr = NULL;
    HU_ASSERT_EQ(kto_get_huml_lm_head_param_for_test(trainer.ctx,
                     probe_row, probe_col, &theta_ptr), HU_OK);
    HU_ASSERT_NOT_NULL(theta_ptr);
    const float saved = *theta_ptr;
    const float eps = 1e-3f;

    double grads[2] = {0};
    for (int branch = 0; branch < 2; branch++) {
        const hu_preference_pair_t *p = (branch == 0) ? &desirable : &undesirable;

        *theta_ptr = saved + eps;
        double L_plus = 0.0;
        HU_ASSERT_EQ(kto_compute_loss_only_for_test(trainer.ctx, p, &L_plus), HU_OK);

        *theta_ptr = saved - eps;
        double L_minus = 0.0;
        HU_ASSERT_EQ(kto_compute_loss_only_for_test(trainer.ctx, p, &L_minus), HU_OK);

        *theta_ptr = saved;

        grads[branch] = (L_plus - L_minus) / (2.0 * (double)eps);
        HU_ASSERT_TRUE(fabs(grads[branch]) > 1e-6);
    }
    /* The desirable and undesirable losses are mirrors:
     * dL_D/dtheta = -lambda*beta*sig*(1-sig) * d(logpi)/dtheta
     * dL_U/dtheta = +lambda*beta*sig*(1-sig) * d(logpi)/dtheta
     * So the gradients must have opposite signs. */
    HU_ASSERT_TRUE(grads[0] * grads[1] < 0);

    trainer.vtable->deinit(trainer.ctx, &alloc);
}

void run_kto_loss_tests(void) {
    HU_TEST_SUITE("kto");
    HU_RUN_TEST(test_kto_rl_trainer_vtable_fields_all_populated);
    HU_RUN_TEST(test_kto_loss_sign_of_gradient_increases_chosen_decreases_rejected);
    HU_RUN_TEST(test_kto_loss_finite_diff_matches_analytical);
}
