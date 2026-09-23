/*
 * Tests for voice decision (when to use TTS vs text).
 * Only compiled when HU_ENABLE_CARTESIA=ON.
 */
#include "human/context/voice_decision.h"
#include "human/persona.h"
#include "test_framework.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#if HU_ENABLE_CARTESIA

static hu_voice_messages_config_t config_enabled_frequent(void) {
    hu_voice_messages_config_t c = {0};
    c.enabled = true;
    (void)snprintf(c.frequency, sizeof(c.frequency), "%.15s", "frequent");
    (void)snprintf(c.prefer_for[0], sizeof(c.prefer_for[0]), "%.31s", "emotional");
    (void)snprintf(c.prefer_for[1], sizeof(c.prefer_for[1]), "%.31s", "comfort");
    (void)snprintf(c.prefer_for[2], sizeof(c.prefer_for[2]), "%.31s", "long_response");
    c.prefer_for_count = 3;
    c.max_duration_sec = 30;
    return c;
}

static hu_voice_messages_config_t config_enabled_rare(void) {
    hu_voice_messages_config_t c = {0};
    c.enabled = true;
    (void)snprintf(c.frequency, sizeof(c.frequency), "%.15s", "rare");
    (void)snprintf(c.prefer_for[0], sizeof(c.prefer_for[0]), "%.31s", "late_night");
    c.prefer_for_count = 1;
    c.max_duration_sec = 30;
    return c;
}

static void test_voice_decision_ex_reports_gate_reasons(void) {
    hu_voice_messages_config_t cfg = config_enabled_frequent();
    const char *why = NULL;
    HU_ASSERT_EQ(hu_voice_decision_classify_ex("Here is my detailed answer to your question.", 42,
                                               "Did it go ok?", 13, &cfg, true, 14, 0, &why),
                 HU_VOICE_SEND_TEXT);
    HU_ASSERT_STR_EQ(why, "incoming_question");
    HU_ASSERT_EQ(hu_voice_decision_classify_ex("Here is my detailed answer to your question.", 42,
                                               "what time is the meeting", 24, &cfg, true, 14, 0,
                                               &why),
                 HU_VOICE_SEND_TEXT);
    HU_ASSERT_STR_EQ(why, "logistics");
    HU_ASSERT_EQ(hu_voice_decision_classify_ex("ok sure", 7, "hey", 3, &cfg, true, 14, 0, &why),
                 HU_VOICE_SEND_TEXT);
    HU_ASSERT_STR_EQ(why, "response_short");
    HU_ASSERT_EQ(hu_voice_decision_classify_ex("Here is my detailed answer for you today.", 40,
                                               "hey", 3, &cfg, false, 14, 0, &why),
                 HU_VOICE_SEND_TEXT);
    HU_ASSERT_STR_EQ(why, "no_voice_id");
    hu_voice_messages_config_t off = cfg;
    off.enabled = false;
    HU_ASSERT_EQ(hu_voice_decision_classify_ex("Here is my detailed answer for you today.", 40,
                                               "hey", 3, &off, true, 14, 0, &why),
                 HU_VOICE_SEND_TEXT);
    HU_ASSERT_STR_EQ(why, "disabled");
}

static void test_voice_decision_ex_voice_and_roll_miss_reasons(void) {
    hu_voice_messages_config_t cfg = config_enabled_frequent(); /* 30% roll */
    static const char resp[] = "I love you and I'm so proud of you, honestly this whole year.";
    const char *why = NULL;
    /* seed 0 -> roll 0 -> voice; "love" satisfies the emotional boost */
    HU_ASSERT_EQ(
        hu_voice_decision_classify_ex(resp, sizeof(resp) - 1, "hey", 3, &cfg, true, 14, 0, &why),
        HU_VOICE_SEND_VOICE);
    HU_ASSERT_STR_EQ(why, "voice");
    /* seed 99 -> roll 99 -> text, and the reason says it was the roll, not a gate */
    HU_ASSERT_EQ(
        hu_voice_decision_classify_ex(resp, sizeof(resp) - 1, "hey", 3, &cfg, true, 14, 99, &why),
        HU_VOICE_SEND_TEXT);
    HU_ASSERT_STR_EQ(why, "roll_miss");
    /* a boring reply never reaches the roll */
    static const char plain[] = "yeah the meeting got moved to thursday afternoon i think.";
    HU_ASSERT_EQ(
        hu_voice_decision_classify_ex(plain, sizeof(plain) - 1, "hey", 3, &cfg, true, 14, 0, &why),
        HU_VOICE_SEND_TEXT);
    HU_ASSERT_STR_EQ(why, "no_prefer_boost");
    /* the plain wrapper still agrees with _ex */
    HU_ASSERT_EQ(hu_voice_decision_classify(resp, sizeof(resp) - 1, "hey", 3, &cfg, true, 14, 0),
                 HU_VOICE_SEND_VOICE);
}

static void test_voice_decision_question_returns_text(void) {
    hu_voice_messages_config_t cfg = config_enabled_frequent();
    hu_voice_decision_t r =
        hu_voice_decision_classify("Here is my detailed answer to your question.", 42,
                                   "What time is the meeting?", 22, &cfg, true, 14, 0);
    HU_ASSERT_EQ(r, HU_VOICE_SEND_TEXT);
}

static void test_voice_decision_short_ok_returns_text(void) {
    hu_voice_messages_config_t cfg = config_enabled_frequent();
    hu_voice_decision_t r =
        hu_voice_decision_classify("ok", 2, "Can you do that?", 16, &cfg, true, 14, 0);
    HU_ASSERT_EQ(r, HU_VOICE_SEND_TEXT);
}

static void test_voice_decision_disabled_returns_text(void) {
    hu_voice_messages_config_t cfg = config_enabled_frequent();
    cfg.enabled = false;
    hu_voice_decision_t r = hu_voice_decision_classify(
        "This is a long response that would otherwise qualify for voice.", 60,
        "How are you feeling?", 19, &cfg, true, 23, 0);
    HU_ASSERT_EQ(r, HU_VOICE_SEND_TEXT);
}

static void test_voice_decision_no_voice_id_returns_text(void) {
    hu_voice_messages_config_t cfg = config_enabled_frequent();
    hu_voice_decision_t r =
        hu_voice_decision_classify("I'm so sorry you're going through this. I'm here for you.", 55,
                                   "I'm really upset", 15, &cfg, false, 23, 0);
    HU_ASSERT_EQ(r, HU_VOICE_SEND_TEXT);
}

static void test_voice_decision_long_emotional_frequent_returns_voice(void) {
    /* Response must fit max_duration_sec (30): ~5 chars/sec → ≤150 chars.
     * Contains "sorry" and "I'm here" for emotional/comfort boost. Seed 0 passes 30% roll. */
    const char *response = "I'm so sorry you're going through this. I'm here for you. "
                           "Sometimes when we feel overwhelmed it helps to take a breath.";
    const char *incoming = "I'm really upset and scared right now";
    hu_voice_messages_config_t cfg = config_enabled_frequent();
    hu_voice_decision_t r =
        hu_voice_decision_classify(response, 105, incoming, 36, &cfg, true, 23, 0);
    HU_ASSERT_EQ(r, HU_VOICE_SEND_VOICE);
}

static void test_voice_decision_late_night_long_may_be_voice(void) {
    const char *response = "Sure, I'd love to catch up soon. Let me know when works for you.";
    hu_voice_messages_config_t cfg = config_enabled_rare();
    hu_voice_decision_t r =
        hu_voice_decision_classify(response, 60, "Hey how are you?", 15, &cfg, true, 23, 0);
    HU_ASSERT_EQ(r, HU_VOICE_SEND_VOICE);
}

static void test_voice_decision_logistics_returns_text(void) {
    hu_voice_messages_config_t cfg = config_enabled_frequent();
    hu_voice_decision_t r =
        hu_voice_decision_classify("The meeting is at 3pm in the main conference room.", 50,
                                   "What time is the meeting?", 24, &cfg, true, 14, 0);
    HU_ASSERT_EQ(r, HU_VOICE_SEND_TEXT);
}

static void test_voice_decision_where_returns_text(void) {
    hu_voice_messages_config_t cfg = config_enabled_frequent();
    hu_voice_decision_t r = hu_voice_decision_classify(
        "It's on the second floor.", 25, "Where is the office?", 18, &cfg, true, 14, 0);
    HU_ASSERT_EQ(r, HU_VOICE_SEND_TEXT);
}

static void test_voice_decision_max_duration_exceeded_returns_text(void) {
    hu_voice_messages_config_t cfg = config_enabled_frequent();
    cfg.max_duration_sec = 5;
    /* 38 chars → est 7 sec > 5 → TEXT (checked before prefer_for boost) */
    const char *response = "I'm so sorry you're going through this.";
    const char *incoming = "I'm really upset";
    hu_voice_decision_t r =
        hu_voice_decision_classify(response, 38, incoming, 15, &cfg, true, 23, 0);
    HU_ASSERT_EQ(r, HU_VOICE_SEND_TEXT);
}

void run_voice_decision_tests(void) {
    HU_TEST_SUITE("Voice decision");
    HU_RUN_TEST(test_voice_decision_ex_reports_gate_reasons);
    HU_RUN_TEST(test_voice_decision_ex_voice_and_roll_miss_reasons);
    HU_RUN_TEST(test_voice_decision_question_returns_text);
    HU_RUN_TEST(test_voice_decision_short_ok_returns_text);
    HU_RUN_TEST(test_voice_decision_disabled_returns_text);
    HU_RUN_TEST(test_voice_decision_no_voice_id_returns_text);
    HU_RUN_TEST(test_voice_decision_long_emotional_frequent_returns_voice);
    HU_RUN_TEST(test_voice_decision_late_night_long_may_be_voice);
    HU_RUN_TEST(test_voice_decision_logistics_returns_text);
    HU_RUN_TEST(test_voice_decision_where_returns_text);
    HU_RUN_TEST(test_voice_decision_max_duration_exceeded_returns_text);
}

#else

void run_voice_decision_tests(void) {
    (void)0; /* No-op when Cartesia disabled */
}

#endif /* HU_ENABLE_CARTESIA */
