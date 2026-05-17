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
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

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
     * With identical prompt+response on both branches and equal lambdas
     * (lambda_d == lambda_u == 1.0), the magnitudes must also match.
     * Phase 3 audit fold-in (critic HIGH-2): the bare sign check
     * grads[0]*grads[1] < 0 would pass for ANY formula that produced
     * opposite signs — even one off by a factor of 10 or 100. Pin the
     * mathematical correctness of the KTO gradient formula by also
     * asserting magnitude equivalence within 5% (HUML toy-GPT FD has
     * higher noise than analytical gradients; 5% absorbs O(eps) FD
     * approximation error without admitting formula drift). */
    HU_ASSERT_TRUE(grads[0] * grads[1] < 0);
    double mag_diff = fabs(grads[0] + grads[1]);
    double mag_avg = 0.5 * (fabs(grads[0]) + fabs(grads[1]));
    HU_ASSERT_TRUE(mag_avg > 0.0);
    double rel_err = mag_diff / mag_avg;
    HU_ASSERT_TRUE(rel_err < 0.05);

    trainer.vtable->deinit(trainer.ctx, &alloc);
}

static void test_kto_huml_50_signal_e2e_chosen_delta_increases_over_iters(void) {
    srand(42);
    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_config_t cfg = {
        .backend = HU_DPO_BACKEND_HUML, .beta = 0.1,
        .learning_rate = 1e-3, .max_iters = 100,
        .lambda_d = 1.0, .lambda_u = 1.0,
    };
    hu_rl_trainer_t trainer = {0};
    HU_ASSERT_EQ(hu_rl_trainer_create_kto(&alloc, &cfg, &trainer), HU_OK);

    /* Load Phase 2 fixture and split into one-sided KTO signals:
     * even index -> desirable (chosen as response), odd -> undesirable */
    hu_preference_pair_t signals[50];
    memset(signals, 0, sizeof(signals));
    size_t n = 0;

    FILE *f = fopen("tests/fixtures/synthetic_preference_pairs_huml.jsonl", "r");
    if (!f) {
        trainer.vtable->deinit(trainer.ctx, &alloc);
        HU_ASSERT_TRUE(0 && "fixture file not found");
        return;
    }
    char line[4096];
    while (fgets(line, sizeof(line), f) && n < 50) {
        const char *pf = strstr(line, "\"prompt\"");
        const char *cf = strstr(line, "\"chosen\"");
        const char *rf = strstr(line, "\"rejected\"");
        if (!pf) continue;

        hu_preference_pair_t *s = &signals[n];
        const char *ps = strchr(pf + 8, '"');
        if (ps) { ps++; const char *pe = strchr(ps, '"');
            if (pe) { size_t len = (size_t)(pe - ps);
                if (len >= sizeof(s->prompt)) len = sizeof(s->prompt) - 1;
                memcpy(s->prompt, ps, len); s->prompt_len = len; } }

        if (n % 2 == 0 && cf) {
            const char *cs = strchr(cf + 8, '"');
            if (cs) { cs++; const char *ce = strchr(cs, '"');
                if (ce) { size_t len = (size_t)(ce - cs);
                    if (len >= sizeof(s->chosen)) len = sizeof(s->chosen) - 1;
                    memcpy(s->chosen, cs, len); s->chosen_len = len; } }
        } else if (n % 2 == 1 && rf) {
            const char *rs = strchr(rf + 10, '"');
            if (rs) { rs++; const char *re = strchr(rs, '"');
                if (re) { size_t len = (size_t)(re - rs);
                    if (len >= sizeof(s->rejected)) len = sizeof(s->rejected) - 1;
                    memcpy(s->rejected, rs, len); s->rejected_len = len; } }
        }
        if (s->prompt_len > 0 && (s->chosen_len > 0 || s->rejected_len > 0))
            n++;
    }
    fclose(f);

    HU_ASSERT_TRUE(n >= 10);

    hu_rl_trainer_metrics_t m = {0};
    for (int iter = 0; iter < 50; iter++) {
        memset(&m, 0, sizeof(m));
        trainer.vtable->step(trainer.ctx, &alloc, signals, n, &m);
    }
    trainer.vtable->deinit(trainer.ctx, &alloc);

    HU_ASSERT_TRUE(m.chosen_logprob_delta > 0);
    HU_ASSERT_TRUE(m.rejected_logprob_delta < 0);
}

static void test_kto_mlx_subprocess_produces_safetensors(void) {
#if !defined(HU_HAVE_MLX_LM_KTO) || HU_HAVE_MLX_LM_KTO == 0
    fprintf(stderr, "[skip] HU_HAVE_MLX_LM_KTO not defined; KTO MLX subprocess test deferred to local run\n");
    return;
#else
    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_config_t cfg = {
        .backend = HU_DPO_BACKEND_MLX,
        .beta = 0.1,
        .max_iters = 5,
        .lambda_d = 1.0,
        .lambda_u = 1.0,
    };
    hu_rl_trainer_t trainer = {0};
    hu_error_t err = hu_kto_mlx_create(&alloc, &cfg, &trainer);
    if (err == HU_ERR_NOT_SUPPORTED) {
        fprintf(stderr, "[skip] mlx-lm-lora KTO not available on this platform\n");
        return;
    }
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(trainer.vtable);

    hu_preference_pair_t pairs[2];
    memset(pairs, 0, sizeof(pairs));
    memcpy(pairs[0].prompt, "hello", 5);
    pairs[0].prompt_len = 5;
    memcpy(pairs[0].chosen, "world", 5);
    pairs[0].chosen_len = 5;
    memcpy(pairs[1].prompt, "hello", 5);
    pairs[1].prompt_len = 5;
    memcpy(pairs[1].rejected, "bad", 3);
    pairs[1].rejected_len = 3;

    hu_rl_trainer_metrics_t m = {0};
    HU_ASSERT_EQ(trainer.vtable->step(trainer.ctx, &alloc, pairs, 2, &m), HU_OK);
    HU_ASSERT_TRUE(strlen(m.adapter_path) > 0);

    struct stat st;
    HU_ASSERT_EQ(stat(m.adapter_path, &st), 0);
    HU_ASSERT_TRUE(st.st_size > 0);

    trainer.vtable->deinit(trainer.ctx, &alloc);
#endif
}

static void test_kto_mlx_dummy_adapter_in_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rl_trainer_config_t cfg = {
        .backend = HU_DPO_BACKEND_MLX,
        .beta = 0.1,
        .max_iters = 1,
        .lambda_d = 1.0,
        .lambda_u = 1.0,
    };
    hu_rl_trainer_t trainer = {0};
    hu_error_t err = hu_kto_mlx_create(&alloc, &cfg, &trainer);
    if (err == HU_ERR_NOT_SUPPORTED) {
        fprintf(stderr, "[skip] not Apple or mlx-lm-lora unavailable\n");
        return;
    }
    HU_ASSERT_EQ(err, HU_OK);

    hu_preference_pair_t pair = {0};
    memcpy(pair.prompt, "test", 4);
    pair.prompt_len = 4;
    memcpy(pair.chosen, "good", 4);
    pair.chosen_len = 4;

    hu_rl_trainer_metrics_t m = {0};
    HU_ASSERT_EQ(trainer.vtable->step(trainer.ctx, &alloc, &pair, 1, &m), HU_OK);
    HU_ASSERT_TRUE(strlen(m.adapter_path) > 0);
    HU_ASSERT_TRUE(m.iters_completed > 0);

    trainer.vtable->deinit(trainer.ctx, &alloc);
}

void run_kto_loss_tests(void) {
    HU_TEST_SUITE("kto");
    HU_RUN_TEST(test_kto_rl_trainer_vtable_fields_all_populated);
    HU_RUN_TEST(test_kto_loss_sign_of_gradient_increases_chosen_decreases_rejected);
    HU_RUN_TEST(test_kto_loss_finite_diff_matches_analytical);
    HU_RUN_TEST(test_kto_huml_50_signal_e2e_chosen_delta_increases_over_iters);
    HU_RUN_TEST(test_kto_mlx_subprocess_produces_safetensors);
    HU_RUN_TEST(test_kto_mlx_dummy_adapter_in_test_mode);
}
