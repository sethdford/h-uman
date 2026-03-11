/*
 * Tests for voice decision (when to use TTS vs text).
 * Only compiled when HU_ENABLE_CARTESIA=ON.
 */
#include "human/context/voice_decision.h"
#include "test_framework.h"
#include <stddef.h>

#if HU_ENABLE_CARTESIA

static void test_voice_decision_question_returns_text(void) {
    hu_voice_decision_t r = hu_voice_decision_classify(
        "Here is my detailed answer to your question.", 42,
        "What time is the meeting?", 22,
        true, "frequent", 14, 0);
    HU_ASSERT_EQ(r, HU_VOICE_SEND_TEXT);
}

static void test_voice_decision_short_ok_returns_text(void) {
    hu_voice_decision_t r = hu_voice_decision_classify(
        "ok", 2,
        "Can you do that?", 16,
        true, "frequent", 14, 0);
    HU_ASSERT_EQ(r, HU_VOICE_SEND_TEXT);
}

static void test_voice_decision_disabled_returns_text(void) {
    hu_voice_decision_t r = hu_voice_decision_classify(
        "This is a long response that would otherwise qualify for voice.", 60,
        "How are you feeling?", 19,
        false, "frequent", 23, 0);
    HU_ASSERT_EQ(r, HU_VOICE_SEND_TEXT);
}

static void test_voice_decision_long_emotional_frequent_returns_voice(void) {
    /* Long response (160 chars) + emotional incoming + frequent + late night → high probability */
    const char *response = "I'm so sorry you're going through this. I'm here for you. "
                           "Sometimes when we feel overwhelmed it helps to take a breath "
                           "and remember you're not alone. Want to talk more?";
    const char *incoming = "I'm really upset and scared right now";
    hu_voice_decision_t r = hu_voice_decision_classify(
        response, 160,
        incoming, 36,
        true, "frequent", 23, 0);
    HU_ASSERT_EQ(r, HU_VOICE_SEND_VOICE);
}

static void test_voice_decision_late_night_long_may_be_voice(void) {
    /* Late night (23) + response > 50, seed 0 → roll 0 < probability → VOICE */
    const char *response = "Sure, I'd love to catch up soon. Let me know when works for you.";
    hu_voice_decision_t r = hu_voice_decision_classify(
        response, 60,
        "Hey how are you?", 15,
        true, "rare", 23, 0);
    HU_ASSERT_EQ(r, HU_VOICE_SEND_VOICE);
}

static void test_voice_decision_logistics_returns_text(void) {
    hu_voice_decision_t r = hu_voice_decision_classify(
        "The meeting is at 3pm in the main conference room.", 50,
        "What time is the meeting?", 24,
        true, "frequent", 14, 0);
    HU_ASSERT_EQ(r, HU_VOICE_SEND_TEXT);
}

static void test_voice_decision_where_returns_text(void) {
    hu_voice_decision_t r = hu_voice_decision_classify(
        "It's on the second floor.", 25,
        "Where is the office?", 18,
        true, "frequent", 14, 0);
    HU_ASSERT_EQ(r, HU_VOICE_SEND_TEXT);
}

void run_voice_decision_tests(void) {
    HU_TEST_SUITE("Voice decision");
    HU_RUN_TEST(test_voice_decision_question_returns_text);
    HU_RUN_TEST(test_voice_decision_short_ok_returns_text);
    HU_RUN_TEST(test_voice_decision_disabled_returns_text);
    HU_RUN_TEST(test_voice_decision_long_emotional_frequent_returns_voice);
    HU_RUN_TEST(test_voice_decision_late_night_long_may_be_voice);
    HU_RUN_TEST(test_voice_decision_logistics_returns_text);
    HU_RUN_TEST(test_voice_decision_where_returns_text);
}

#else

void run_voice_decision_tests(void) {
    (void)0; /* No-op when Cartesia disabled */
}

#endif /* HU_ENABLE_CARTESIA */
