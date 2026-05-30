/* tests/test_register.c
 *
 * Unit tests for src/eval/register.c — the A4 relationship-calibration axis
 * of the humanness north-star metric
 * (docs/plans/2026-05-29-humanness-north-star-metric/). Pins AC-6: a
 * too-formal reply to a warm contact scores low; a calibrated reply scores
 * high. Each estimator is a pure predicate, so we can assert the contract
 * directly without a full nightly run.
 *
 * 2026-05-29.
 */

#include "human/eval/register.h"
#include "test_framework.h"
#include <string.h>

/* ── formality estimator ───────────────────────────────────────────────── */

static void test_register_formality_casual_text_low(void) {
    /* lowercase textisms — unambiguously casual */
    const char *casual[] = {
        "yeah lol i'll be there rn",
        "u up? wanna grab food",
        "haha omg that's wild, idk tbh",
    };
    for (size_t i = 0; i < sizeof(casual) / sizeof(casual[0]); i++) {
        double f = hu_register_formality_estimate(casual[i], strlen(casual[i]));
        HU_ASSERT_TRUE(f >= 0.0 && f <= 1.0);
        HU_ASSERT_TRUE(f < 0.4);
    }
}

static void test_register_formality_formal_text_high(void) {
    /* salutation + full punctuation + no contractions — unambiguously formal */
    const char *formal[] = {
        "Dear Mr. Smith, I would like to confirm our appointment. Sincerely, Seth.",
        "Please find the requested report attached. Kindly review it at your convenience.",
        "Thank you for your message. I will respond to your inquiry shortly.",
    };
    for (size_t i = 0; i < sizeof(formal) / sizeof(formal[0]); i++) {
        double f = hu_register_formality_estimate(formal[i], strlen(formal[i]));
        HU_ASSERT_TRUE(f >= 0.0 && f <= 1.0);
        HU_ASSERT_TRUE(f > 0.6);
    }
}

static void test_register_formality_null_and_empty_neutral(void) {
    HU_ASSERT_TRUE(hu_register_formality_estimate(NULL, 0) == 0.5);
    HU_ASSERT_TRUE(hu_register_formality_estimate("", 0) == 0.5);
    HU_ASSERT_TRUE(hu_register_formality_estimate("   \t\n ", 5) == 0.5);
}

/* ── warmth estimator ──────────────────────────────────────────────────── */

static void test_register_warmth_warm_text_high(void) {
    const char *warm[] = {
        "hey love! miss you so much, can't wait to see you xo",
        "morning buddy! hope you slept well, thinking of you today",
        "yesss so happy for you!! that's amazing, love it",
    };
    for (size_t i = 0; i < sizeof(warm) / sizeof(warm[0]); i++) {
        double w = hu_register_warmth_estimate(warm[i], strlen(warm[i]));
        HU_ASSERT_TRUE(w >= 0.0 && w <= 1.0);
        HU_ASSERT_TRUE(w > 0.6);
    }
}

static void test_register_warmth_distant_text_low(void) {
    const char *distant[] = {
        "Received. Will process per the agreement.",
        "Noted. As discussed, see the attached.",
        "Confirmed. The transaction has been recorded.",
    };
    for (size_t i = 0; i < sizeof(distant) / sizeof(distant[0]); i++) {
        double w = hu_register_warmth_estimate(distant[i], strlen(distant[i]));
        HU_ASSERT_TRUE(w >= 0.0 && w <= 1.0);
        HU_ASSERT_TRUE(w < 0.4);
    }
}

static void test_register_warmth_null_neutral(void) {
    HU_ASSERT_TRUE(hu_register_warmth_estimate(NULL, 0) == 0.5);
    HU_ASSERT_TRUE(hu_register_warmth_estimate("", 0) == 0.5);
}

/* ── A4 relationship-calibration score (the headline AC-6 contract) ──────── */

static void test_relationship_too_formal_to_warm_contact_scores_low(void) {
    /* Target: a partner — very casual, very warm. */
    const double target_formality = 0.1, target_warmth = 0.9;
    /* Reply that is stiff and cold — wrong register for this contact. */
    const char *stiff = "Received. I will revert to you regarding this matter in due course.";
    double s = hu_relationship_axis_score(stiff, strlen(stiff), target_formality, target_warmth);
    HU_ASSERT_TRUE(s >= 0.0 && s <= 1.0);
    HU_ASSERT_TRUE(s < 0.5);
}

static void test_relationship_calibrated_to_warm_contact_scores_high(void) {
    const double target_formality = 0.1, target_warmth = 0.9;
    /* Casual + warm reply — right register for a partner. */
    const char *good = "hey love! yeah i'm around tonight, can't wait to see you xo";
    double s = hu_relationship_axis_score(good, strlen(good), target_formality, target_warmth);
    HU_ASSERT_TRUE(s >= 0.0 && s <= 1.0);
    HU_ASSERT_TRUE(s > 0.6);
}

static void test_relationship_calibrated_to_distant_contact_scores_high(void) {
    /* Target: a stranger / formal contact — formal, low warmth. */
    const double target_formality = 0.8, target_warmth = 0.2;
    const char *good = "Thank you. I will confirm the details and follow up shortly.";
    double s = hu_relationship_axis_score(good, strlen(good), target_formality, target_warmth);
    HU_ASSERT_TRUE(s > 0.55);
}

static void test_relationship_wrong_direction_to_distant_contact_scores_low(void) {
    /* Overly chummy reply to a formal/distant contact — miscalibrated. */
    const double target_formality = 0.8, target_warmth = 0.2;
    const char *chummy = "heyyy buddy!! omg lol miss u so much xo can't wait";
    double s = hu_relationship_axis_score(chummy, strlen(chummy), target_formality, target_warmth);
    HU_ASSERT_TRUE(s < 0.5);
}

static void test_relationship_target_out_of_range_clamped(void) {
    /* Out-of-range targets must not produce out-of-range scores. */
    const char *txt = "hey, sounds good";
    double s = hu_relationship_axis_score(txt, strlen(txt), 2.0, -1.0);
    HU_ASSERT_TRUE(s >= 0.0 && s <= 1.0);
}

void run_register_tests(void) {
    HU_TEST_SUITE("eval register / relationship axis");
    HU_RUN_TEST(test_register_formality_casual_text_low);
    HU_RUN_TEST(test_register_formality_formal_text_high);
    HU_RUN_TEST(test_register_formality_null_and_empty_neutral);
    HU_RUN_TEST(test_register_warmth_warm_text_high);
    HU_RUN_TEST(test_register_warmth_distant_text_low);
    HU_RUN_TEST(test_register_warmth_null_neutral);
    HU_RUN_TEST(test_relationship_too_formal_to_warm_contact_scores_low);
    HU_RUN_TEST(test_relationship_calibrated_to_warm_contact_scores_high);
    HU_RUN_TEST(test_relationship_calibrated_to_distant_contact_scores_high);
    HU_RUN_TEST(test_relationship_wrong_direction_to_distant_contact_scores_low);
    HU_RUN_TEST(test_relationship_target_out_of_range_clamped);
}
