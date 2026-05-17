/* tests/test_value_head.c — Phase 3 Task 1
 *
 * Pins the hu_value_head_t linear projection used as the trainable
 * surface in Phase 3's reward-model composition (hu_reward_model_t,
 * Task 2) and Bradley-Terry RM training (Task 3).
 *
 * Three pinned invariants:
 *   1. Forward: score = W . h + b matches a hand-computed dot product.
 *   2. Backward: dW = h * dL_dscore, db = dL_dscore, dh = W * dL_dscore;
 *      analytical dW is also pinned against a finite-difference numerical
 *      gradient (eps = 1e-3, tolerance 1e-3).
 *   3. Save/load: "VHED" magic round-trips W and b byte-identically.
 */
#include "test_framework.h"

#include "human/core/allocator.h"
#include "human/ml/value_head.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static void test_value_head_forward_matches_hand_computed_dot_product(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_value_head_t vh = {0};
    HU_ASSERT_EQ(hu_value_head_create(&alloc, /*hidden_dim=*/4, &vh), HU_OK);

    /* Manually set known weights for a deterministic forward:
     *   W = [0.5, -0.25, 0.1, -0.1]
     *   b = 0.3
     *   h = [1.0, 2.0, 3.0, 4.0]
     * expected = 0.5*1 + (-0.25)*2 + 0.1*3 + (-0.1)*4 + 0.3
     *          = 0.5  - 0.5      + 0.3   - 0.4      + 0.3
     *          = 0.2 */
    vh.W[0] = 0.5f;
    vh.W[1] = -0.25f;
    vh.W[2] = 0.1f;
    vh.W[3] = -0.1f;
    vh.b = 0.3f;
    float h[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    double score = 0.0;
    HU_ASSERT_EQ(hu_value_head_forward(&vh, h, &score), HU_OK);
    HU_ASSERT_TRUE(fabs(score - 0.2) < 1e-5);

    hu_value_head_deinit(&vh, &alloc);
}

static void test_value_head_backward_grad_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_value_head_t vh = {0};
    HU_ASSERT_EQ(hu_value_head_create(&alloc, /*hidden_dim=*/4, &vh), HU_OK);
    vh.W[0] = 0.5f;
    vh.W[1] = -0.25f;
    vh.W[2] = 0.1f;
    vh.W[3] = -0.1f;
    vh.b = 0.3f;
    float h[4] = {1.0f, 2.0f, 3.0f, 4.0f};

    /* Analytical grads for L = score (so dL_dscore = 1):
     *   dW[i] = h[i]
     *   db    = 1
     *   dh[i] = W[i] */
    float dW[4] = {0};
    float dh[4] = {0};
    float db = 0.0f;
    HU_ASSERT_EQ(hu_value_head_backward(&vh, h, /*dL_dscore=*/1.0, dW, &db, dh),
                 HU_OK);
    for (int i = 0; i < 4; i++) {
        HU_ASSERT_TRUE(fabs((double)dW[i] - (double)h[i]) < 1e-5);
    }
    HU_ASSERT_TRUE(fabs((double)db - 1.0) < 1e-5);
    for (int i = 0; i < 4; i++) {
        HU_ASSERT_TRUE(fabs((double)dh[i] - (double)vh.W[i]) < 1e-5);
    }

    /* Finite-difference check on each W[i] (tol 1e-3): perturb by ±eps,
     * recompute score, compare numerical gradient to analytical dW. */
    const float eps = 1e-3f;
    for (int i = 0; i < 4; i++) {
        float saved = vh.W[i];
        vh.W[i] = saved + eps;
        double sp = 0.0;
        HU_ASSERT_EQ(hu_value_head_forward(&vh, h, &sp), HU_OK);
        vh.W[i] = saved - eps;
        double sm = 0.0;
        HU_ASSERT_EQ(hu_value_head_forward(&vh, h, &sm), HU_OK);
        vh.W[i] = saved;
        double numerical = (sp - sm) / (2.0 * (double)eps);
        HU_ASSERT_TRUE(fabs(numerical - (double)dW[i]) < 1e-3);
    }

    hu_value_head_deinit(&vh, &alloc);
}

static void test_value_head_save_load_round_trips(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_value_head_t vh1 = {0};
    HU_ASSERT_EQ(hu_value_head_create(&alloc, /*hidden_dim=*/4, &vh1), HU_OK);
    vh1.W[0] = 0.5f;
    vh1.W[1] = -0.25f;
    vh1.W[2] = 0.1f;
    vh1.W[3] = -0.1f;
    vh1.b = 0.3f;

    const char *path = "/tmp/hu_vh_round_trip.vh";
    HU_ASSERT_EQ(hu_value_head_save(&vh1, path), HU_OK);

    hu_value_head_t vh2 = {0};
    HU_ASSERT_EQ(hu_value_head_load(&alloc, path, &vh2), HU_OK);
    HU_ASSERT_EQ(vh2.hidden_dim, vh1.hidden_dim);
    for (size_t i = 0; i < vh1.hidden_dim; i++) {
        HU_ASSERT_TRUE(fabs((double)vh1.W[i] - (double)vh2.W[i]) < 1e-9);
    }
    HU_ASSERT_TRUE(fabs((double)vh1.b - (double)vh2.b) < 1e-9);

    hu_value_head_deinit(&vh1, &alloc);
    hu_value_head_deinit(&vh2, &alloc);
    remove(path);
}

void run_value_head_tests(void) {
    HU_TEST_SUITE("value_head");
    HU_RUN_TEST(test_value_head_forward_matches_hand_computed_dot_product);
    HU_RUN_TEST(test_value_head_backward_grad_check);
    HU_RUN_TEST(test_value_head_save_load_round_trips);
}
