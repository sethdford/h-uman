#include "human/cognition/emotional.h"
#include "human/core/error.h"
#include "human/memory/stm.h"
#include "test_framework.h"
#include <math.h>
#include <string.h>

/* Sprint 6 US-17: unit tests for hu_emotional_apply_contagion */

static void contagion_null_safe(void) {
    hu_error_t err = hu_emotional_apply_contagion(NULL, HU_EMOTION_JOY, 0.8f, 0.3f);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void contagion_partner_neutral_no_change(void) {
    hu_emotional_cognition_t ec;
    hu_emotional_cognition_init(&ec);
    float initial_valence = ec.state.valence;
    float initial_intensity = ec.state.intensity;

    hu_error_t err = hu_emotional_apply_contagion(&ec, HU_EMOTION_NEUTRAL, 0.5f, 0.3f);
    HU_ASSERT_EQ(err, HU_OK);

    /* Neutral emotion maps to valence 0, so no valence change */
    HU_ASSERT_FLOAT_EQ(ec.state.valence, initial_valence, 0.001f);
    /* Arousal still rises slightly from partner intensity even on neutral */
    HU_ASSERT_TRUE(ec.state.intensity >= initial_intensity);
}

static void contagion_partner_sadness_drops_valence(void) {
    hu_emotional_cognition_t ec;
    hu_emotional_cognition_init(&ec);
    ec.state.valence = 0.0f;

    /* sadness maps to -0.7; delta = -0.7 * 0.8 * 0.3 = -0.168 */
    hu_error_t err = hu_emotional_apply_contagion(&ec, HU_EMOTION_SADNESS, 0.8f, 0.3f);
    HU_ASSERT_EQ(err, HU_OK);

    float expected = -0.168f;
    HU_ASSERT_TRUE(ec.state.valence < 0.0f);
    HU_ASSERT_FLOAT_EQ(ec.state.valence, expected, 0.05f);
}

static void contagion_partner_joy_lifts_valence(void) {
    hu_emotional_cognition_t ec;
    hu_emotional_cognition_init(&ec);
    ec.state.valence = 0.0f;

    /* joy maps to +0.7; delta = +0.7 * 0.6 * 0.3 = +0.126 */
    hu_error_t err = hu_emotional_apply_contagion(&ec, HU_EMOTION_JOY, 0.6f, 0.3f);
    HU_ASSERT_EQ(err, HU_OK);

    float expected = 0.126f;
    HU_ASSERT_TRUE(ec.state.valence > 0.0f);
    HU_ASSERT_FLOAT_EQ(ec.state.valence, expected, 0.05f);
}

static void contagion_valence_clamped_negative(void) {
    hu_emotional_cognition_t ec;
    hu_emotional_cognition_init(&ec);
    ec.state.valence = -0.95f;

    /* Large sadness push should not go below -1.0 */
    (void)hu_emotional_apply_contagion(&ec, HU_EMOTION_SADNESS, 1.0f, 1.0f);
    HU_ASSERT_TRUE(ec.state.valence >= -1.0f);
}

static void contagion_valence_clamped_positive(void) {
    hu_emotional_cognition_t ec;
    hu_emotional_cognition_init(&ec);
    ec.state.valence = 0.95f;

    /* Large joy push should not exceed +1.0 */
    (void)hu_emotional_apply_contagion(&ec, HU_EMOTION_JOY, 1.0f, 1.0f);
    HU_ASSERT_TRUE(ec.state.valence <= 1.0f);
}

static void contagion_default_fraction_when_zero(void) {
    hu_emotional_cognition_t ec;
    hu_emotional_cognition_init(&ec);
    ec.state.valence = 0.0f;

    /* Passing 0 fraction should use default 0.3; joy 0.6 -> +0.126 */
    hu_error_t err = hu_emotional_apply_contagion(&ec, HU_EMOTION_JOY, 0.6f, 0.0f);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(ec.state.valence > 0.0f);
}

void run_emotional_contagion_tests(void) {
    HU_TEST_SUITE("EmotionalContagion");
    HU_RUN_TEST(contagion_null_safe);
    HU_RUN_TEST(contagion_partner_neutral_no_change);
    HU_RUN_TEST(contagion_partner_sadness_drops_valence);
    HU_RUN_TEST(contagion_partner_joy_lifts_valence);
    HU_RUN_TEST(contagion_valence_clamped_negative);
    HU_RUN_TEST(contagion_valence_clamped_positive);
    HU_RUN_TEST(contagion_default_fraction_when_zero);
}
