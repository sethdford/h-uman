#include "human/core/allocator.h"
#include "human/intelligence/trust.h"
#include "test_framework.h"
#include <string.h>

static void trust_init_sets_defaults(void) {
    hu_trust_state_t state;
    hu_trust_init(&state);
    HU_ASSERT_TRUE(state.trust_level >= 0.49 && state.trust_level <= 0.51);
    HU_ASSERT_EQ(state.corrections_count, 0u);
    HU_ASSERT_EQ(state.fabrications_count, 0u);
    HU_ASSERT_EQ(state.accurate_recalls, 0u);
}

static void trust_accurate_recall_increases(void) {
    hu_trust_state_t state;
    hu_trust_init(&state);
    double before = state.trust_level;
    hu_error_t err = hu_trust_update(&state, HU_TRUST_ACCURATE_RECALL, 1000);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(state.trust_level > before);
    HU_ASSERT_EQ(state.accurate_recalls, 1u);
}

static void trust_fabrication_decreases(void) {
    hu_trust_state_t state;
    hu_trust_init(&state);
    double before = state.trust_level;
    hu_error_t err = hu_trust_update(&state, HU_TRUST_FABRICATION, 1000);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(state.trust_level < before);
    HU_ASSERT_EQ(state.fabrications_count, 1u);
}

static void trust_correction_decreases(void) {
    hu_trust_state_t state;
    hu_trust_init(&state);
    double before = state.trust_level;
    hu_error_t err = hu_trust_update(&state, HU_TRUST_CORRECTION, 1000);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(state.trust_level < before);
    HU_ASSERT_EQ(state.corrections_count, 1u);
    HU_ASSERT_TRUE(state.last_error_at == 1000);
}

static void trust_clamps_to_zero(void) {
    hu_trust_state_t state;
    hu_trust_init(&state);
    /* Hammer with fabrications until trust bottoms out */
    for (int i = 0; i < 20; i++)
        hu_trust_update(&state, HU_TRUST_FABRICATION, 1000 + i);
    HU_ASSERT_TRUE(state.trust_level >= 0.0);
    HU_ASSERT_TRUE(state.trust_level <= 0.01);
}

static void trust_clamps_to_one(void) {
    hu_trust_state_t state;
    hu_trust_init(&state);
    /* Lots of positive events */
    for (int i = 0; i < 30; i++)
        hu_trust_update(&state, HU_TRUST_VERIFIED_CLAIM, 1000 + i);
    HU_ASSERT_TRUE(state.trust_level <= 1.0);
    HU_ASSERT_TRUE(state.trust_level >= 0.99);
}

static void trust_erosion_detected_when_low(void) {
    hu_trust_state_t state;
    hu_trust_init(&state);
    /* Drop trust below threshold */
    for (int i = 0; i < 5; i++)
        hu_trust_update(&state, HU_TRUST_FABRICATION, 1000 + i);
    HU_ASSERT_TRUE(hu_trust_detect_erosion(&state));
}

static void trust_no_erosion_at_default(void) {
    hu_trust_state_t state;
    hu_trust_init(&state);
    HU_ASSERT_TRUE(!hu_trust_detect_erosion(&state));
}

static void trust_null_returns_error(void) {
    hu_error_t err = hu_trust_update(NULL, HU_TRUST_CORRECTION, 1000);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void calibrate_high_trust_high_confidence_certain(void) {
    hu_trust_state_t state;
    hu_trust_init(&state);
    /* Boost trust high */
    for (int i = 0; i < 15; i++)
        hu_trust_update(&state, HU_TRUST_VERIFIED_CLAIM, 1000 + i);
    hu_uncertainty_level_t level = hu_trust_calibrate_language(&state, 0.98);
    HU_ASSERT_EQ(level, HU_UNCERTAINTY_CERTAIN);
}

static void calibrate_low_trust_downgrades(void) {
    hu_trust_state_t state;
    hu_trust_init(&state);
    /* Drop trust low */
    for (int i = 0; i < 5; i++)
        hu_trust_update(&state, HU_TRUST_FABRICATION, 1000 + i);
    /* Even with high internal confidence, low trust should downgrade */
    hu_uncertainty_level_t level = hu_trust_calibrate_language(&state, 0.90);
    HU_ASSERT_TRUE(level > HU_UNCERTAINTY_CERTAIN);
}

static void calibrate_low_confidence_speculating(void) {
    hu_trust_state_t state;
    hu_trust_init(&state);
    hu_uncertainty_level_t level = hu_trust_calibrate_language(&state, 0.10);
    HU_ASSERT_TRUE(level >= HU_UNCERTAINTY_NOT_SURE);
}

static void uncertainty_prefix_returns_valid(void) {
    const char *p = hu_uncertainty_prefix(HU_UNCERTAINTY_THINK);
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_STR_CONTAINS(p, "think");
}

static void uncertainty_prefix_certain(void) {
    const char *p = hu_uncertainty_prefix(HU_UNCERTAINTY_CERTAIN);
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_STR_CONTAINS(p, "know");
}

static void uncertainty_prefix_speculating(void) {
    const char *p = hu_uncertainty_prefix(HU_UNCERTAINTY_SPECULATING);
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_STR_CONTAINS(p, "wrong");
}

static void trust_directive_low_trust_emits(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_trust_state_t state;
    hu_trust_init(&state);
    for (int i = 0; i < 5; i++)
        hu_trust_update(&state, HU_TRUST_FABRICATION, 1000 + i);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_trust_state_build_directive(&alloc, &state, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_STR_CONTAINS(out, "TRUST");
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void trust_directive_high_trust_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_trust_state_t state;
    hu_trust_init(&state);
    for (int i = 0; i < 5; i++)
        hu_trust_update(&state, HU_TRUST_VERIFIED_CLAIM, 1000 + i);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_trust_state_build_directive(&alloc, &state, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    /* High trust: no directive needed */
    HU_ASSERT_TRUE(out == NULL);
    HU_ASSERT_EQ(out_len, (size_t)0);
}

static void trust_directive_null_returns_error(void) {
    hu_error_t err = hu_trust_state_build_directive(NULL, NULL, NULL, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void trust_helpful_action_increases(void) {
    hu_trust_state_t state;
    hu_trust_init(&state);
    double before = state.trust_level;
    hu_trust_update(&state, HU_TRUST_HELPFUL_ACTION, 1000);
    HU_ASSERT_TRUE(state.trust_level > before);
    HU_ASSERT_EQ(state.helpful_actions, 1u);
}

static void trust_error_decreases(void) {
    hu_trust_state_t state;
    hu_trust_init(&state);
    double before = state.trust_level;
    hu_trust_update(&state, HU_TRUST_ERROR, 1000);
    HU_ASSERT_TRUE(state.trust_level < before);
    HU_ASSERT_TRUE(state.last_error_at == 1000);
}

void run_trust_tests(void) {
    HU_TEST_SUITE("trust");
    HU_RUN_TEST(trust_init_sets_defaults);
    HU_RUN_TEST(trust_accurate_recall_increases);
    HU_RUN_TEST(trust_fabrication_decreases);
    HU_RUN_TEST(trust_correction_decreases);
    HU_RUN_TEST(trust_clamps_to_zero);
    HU_RUN_TEST(trust_clamps_to_one);
    HU_RUN_TEST(trust_erosion_detected_when_low);
    HU_RUN_TEST(trust_no_erosion_at_default);
    HU_RUN_TEST(trust_null_returns_error);
    HU_RUN_TEST(calibrate_high_trust_high_confidence_certain);
    HU_RUN_TEST(calibrate_low_trust_downgrades);
    HU_RUN_TEST(calibrate_low_confidence_speculating);
    HU_RUN_TEST(uncertainty_prefix_returns_valid);
    HU_RUN_TEST(uncertainty_prefix_certain);
    HU_RUN_TEST(uncertainty_prefix_speculating);
    HU_RUN_TEST(trust_directive_low_trust_emits);
    HU_RUN_TEST(trust_directive_high_trust_empty);
    HU_RUN_TEST(trust_directive_null_returns_error);
    HU_RUN_TEST(trust_helpful_action_increases);
    HU_RUN_TEST(trust_error_decreases);
}
