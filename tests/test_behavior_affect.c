#include "human/behavior/affect.h"
#include "test_framework.h"

#include <stdint.h>
#include <string.h>

static void affect_estimate_audio_stub_is_neutral_audio_modality(void) {
    int16_t samples[4] = {100, -100, 50, -50};
    hu_affect_state_t s;
    HU_ASSERT_EQ(hu_affect_estimate_audio(samples, 4, 16000, &s), HU_OK);
    HU_ASSERT_EQ((int)s.modality, (int)HU_AFFECT_AUDIO);
    HU_ASSERT_TRUE(s.uncertainty >= 0.9f);
}

static void affect_init_neutral_with_high_uncertainty(void) {
    hu_affect_state_t s;
    hu_affect_init(&s);
    HU_ASSERT_FLOAT_EQ(s.valence, 0.f, 0.001f);
    HU_ASSERT_FLOAT_EQ(s.arousal, 0.f, 0.001f);
    HU_ASSERT_FLOAT_EQ(s.dominance, 0.5f, 0.001f);
    HU_ASSERT_FLOAT_EQ(s.uncertainty, 1.f, 0.001f);
}

static void affect_estimate_positive_text_pushes_valence_positive(void) {
    hu_affect_state_t s;
    const char *t = "I am happy and grateful today";
    hu_error_t e = hu_affect_estimate_text(t, strlen(t), &s);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_TRUE(s.valence > 0.3f);
    HU_ASSERT_TRUE(s.uncertainty < 0.7f);
}

static void affect_estimate_negative_text_pushes_valence_negative(void) {
    hu_affect_state_t s;
    const char *t = "I feel hopeless and exhausted";
    hu_error_t e = hu_affect_estimate_text(t, strlen(t), &s);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_TRUE(s.valence < -0.3f);
}

static void affect_estimate_distress_text_is_distress(void) {
    hu_affect_state_t s;
    const char *t = "I am overwhelmed and scared right now!";
    hu_error_t e = hu_affect_estimate_text(t, strlen(t), &s);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_TRUE(hu_affect_is_distress(&s));
}

static void affect_estimate_negation_flips_valence(void) {
    hu_affect_state_t s_pos;
    const char *pos = "I am happy";
    HU_ASSERT_EQ(hu_affect_estimate_text(pos, strlen(pos), &s_pos), HU_OK);

    hu_affect_state_t s_neg;
    const char *neg = "I am not happy";
    HU_ASSERT_EQ(hu_affect_estimate_text(neg, strlen(neg), &s_neg), HU_OK);

    HU_ASSERT_TRUE(s_neg.valence < s_pos.valence);
}

static void affect_estimate_caps_increases_arousal(void) {
    hu_affect_state_t s_lower;
    hu_affect_state_t s_upper;
    const char *lower = "this is great";
    const char *upper = "THIS IS GREAT";
    HU_ASSERT_EQ(hu_affect_estimate_text(lower, strlen(lower), &s_lower), HU_OK);
    HU_ASSERT_EQ(hu_affect_estimate_text(upper, strlen(upper), &s_upper), HU_OK);
    HU_ASSERT_TRUE(s_upper.arousal >= s_lower.arousal);
}

static void affect_decay_brings_arousal_toward_zero(void) {
    hu_affect_state_t s;
    hu_affect_init(&s);
    s.valence = 0.6f;
    s.arousal = 0.8f;
    s.dominance = 0.7f;
    s.uncertainty = 0.2f;
    s.ts = 1000;
    HU_ASSERT_EQ(hu_affect_decay(&s, 1010, 5.f), HU_OK);
    HU_ASSERT_TRUE(s.arousal < 0.4f);
    HU_ASSERT_TRUE(s.valence < 0.4f);
}

static void affect_fuse_low_uncertainty_dominates(void) {
    hu_affect_state_t prior = {0};
    hu_affect_state_t update = {0};
    hu_affect_state_t out = {0};

    prior.valence = 0.6f;
    prior.arousal = 0.4f;
    prior.dominance = 0.5f;
    prior.uncertainty = 0.9f;

    update.valence = -0.6f;
    update.arousal = 0.7f;
    update.dominance = 0.4f;
    update.uncertainty = 0.1f;

    HU_ASSERT_EQ(hu_affect_fuse(&prior, &update, &out), HU_OK);
    HU_ASSERT_TRUE(out.valence < 0.f);
    HU_ASSERT_EQ(out.modality, HU_AFFECT_FUSED);
}

static void affect_route_score_higher_for_distress(void) {
    hu_affect_state_t neutral;
    hu_affect_init(&neutral);
    neutral.uncertainty = 0.3f;

    hu_affect_state_t distress;
    hu_affect_init(&distress);
    distress.valence = -0.7f;
    distress.arousal = 0.85f;
    distress.dominance = 0.2f;
    distress.uncertainty = 0.1f;

    int s_neutral = hu_affect_route_tier_score(&neutral);
    int s_distress = hu_affect_route_tier_score(&distress);
    HU_ASSERT_TRUE(s_distress > s_neutral);
    HU_ASSERT_TRUE(s_distress >= 4);
}

static void affect_route_score_zero_when_uncertain(void) {
    hu_affect_state_t s;
    hu_affect_init(&s);
    s.valence = -0.9f;
    s.arousal = 0.9f;
    s.uncertainty = 0.95f;
    HU_ASSERT_EQ(hu_affect_route_tier_score(&s), 0);
}

void run_behavior_affect_tests(void);

void run_behavior_affect_tests(void) {
    HU_TEST_SUITE("behavior_affect");
    HU_RUN_TEST(affect_estimate_audio_stub_is_neutral_audio_modality);
    HU_RUN_TEST(affect_init_neutral_with_high_uncertainty);
    HU_RUN_TEST(affect_estimate_positive_text_pushes_valence_positive);
    HU_RUN_TEST(affect_estimate_negative_text_pushes_valence_negative);
    HU_RUN_TEST(affect_estimate_distress_text_is_distress);
    HU_RUN_TEST(affect_estimate_negation_flips_valence);
    HU_RUN_TEST(affect_estimate_caps_increases_arousal);
    HU_RUN_TEST(affect_decay_brings_arousal_toward_zero);
    HU_RUN_TEST(affect_fuse_low_uncertainty_dominates);
    HU_RUN_TEST(affect_route_score_higher_for_distress);
    HU_RUN_TEST(affect_route_score_zero_when_uncertain);
}
