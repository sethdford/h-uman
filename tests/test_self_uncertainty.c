/* Tests for calibrated self-uncertainty (src/agent/self_uncertainty.c).
 * The ECE-direction surrogate: recent confidence must map to a hedge decision in
 * the correct direction (low -> hedge, high -> not), monotonically. */
#include "human/agent/self_uncertainty.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <math.h>
#include <string.h>

static bool hedges(float c) {
    hu_self_uncertainty_t a;
    hu_self_uncertainty_assess(c, &a);
    return a.hedge;
}

static void su_low_confidence_hedges(void) {
    HU_ASSERT_TRUE(hedges(0.1f));
    hu_self_uncertainty_t a;
    hu_self_uncertainty_assess(0.1f, &a);
    HU_ASSERT_TRUE(a.hedge);

    hu_allocator_t alloc = hu_system_allocator();
    char *dir = NULL;
    size_t dir_len = 0;
    HU_ASSERT_EQ((int)hu_self_uncertainty_build_directive(&alloc, &a, &dir, &dir_len), (int)HU_OK);
    HU_ASSERT_NOT_NULL(dir);
    HU_ASSERT_TRUE(dir_len > 0);
    HU_ASSERT_STR_CONTAINS(dir, "self-awareness");
    HU_ASSERT_STR_CONTAINS(dir, "not sure");
    alloc.free(alloc.ctx, dir, dir_len + 1);
}

static void su_high_confidence_no_hedge(void) {
    HU_ASSERT_FALSE(hedges(0.9f));
    hu_self_uncertainty_t a;
    hu_self_uncertainty_assess(0.9f, &a);
    HU_ASSERT_FALSE(a.hedge);

    hu_allocator_t alloc = hu_system_allocator();
    char *dir = (char *)0x1;
    size_t dir_len = 99;
    HU_ASSERT_EQ((int)hu_self_uncertainty_build_directive(&alloc, &a, &dir, &dir_len), (int)HU_OK);
    HU_ASSERT_NULL(dir); /* confident -> inject nothing */
    HU_ASSERT_EQ((int)dir_len, 0);
}

/* ECE-direction surrogate: across the [0,1] sweep, hedge is true iff confidence
 * is below threshold — a single monotonic transition, never inverted. */
static void su_calibration_monotonic_surrogate(void) {
    bool prev = true; /* at c=0 we hedge */
    for (int i = 0; i <= 100; i++) {
        float c = (float)i / 100.0f;
        bool h = hedges(c);
        HU_ASSERT_EQ((int)h, (int)(c < HU_SELF_UNCERTAINTY_THRESHOLD));
        /* monotonic: once we stop hedging we never resume as confidence rises */
        if (!prev) {
            HU_ASSERT_FALSE(h);
        }
        prev = h;
    }
}

static void su_threshold_boundary(void) {
    HU_ASSERT_FALSE(hedges(HU_SELF_UNCERTAINTY_THRESHOLD)); /* 0.5 is NOT < 0.5 */
    HU_ASSERT_TRUE(hedges(HU_SELF_UNCERTAINTY_THRESHOLD - 0.01f));
}

static void su_clamps_out_of_range(void) {
    hu_self_uncertainty_t a;
    hu_self_uncertainty_assess(-1.0f, &a);
    HU_ASSERT_FLOAT_EQ(a.confidence, 0.0f, 1e-6);
    HU_ASSERT_TRUE(a.hedge);
    hu_self_uncertainty_assess(2.0f, &a);
    HU_ASSERT_FLOAT_EQ(a.confidence, 1.0f, 1e-6);
    HU_ASSERT_FALSE(a.hedge);
    hu_self_uncertainty_assess(NAN, &a);
    HU_ASSERT_FLOAT_EQ(a.confidence, 0.0f, 1e-6);
    HU_ASSERT_TRUE(a.hedge);
}

static void su_null_safe(void) {
    hu_self_uncertainty_assess(0.3f, NULL); /* must not crash */
    hu_allocator_t alloc = hu_system_allocator();
    char *dir = NULL;
    size_t dir_len = 0;
    HU_ASSERT_EQ((int)hu_self_uncertainty_build_directive(NULL, NULL, &dir, &dir_len),
                 (int)HU_ERR_INVALID_ARGUMENT);
}

void run_self_uncertainty_tests(void) {
    HU_TEST_SUITE("self_uncertainty");
    HU_RUN_TEST(su_low_confidence_hedges);
    HU_RUN_TEST(su_high_confidence_no_hedge);
    HU_RUN_TEST(su_calibration_monotonic_surrogate);
    HU_RUN_TEST(su_threshold_boundary);
    HU_RUN_TEST(su_clamps_out_of_range);
    HU_RUN_TEST(su_null_safe);
}
