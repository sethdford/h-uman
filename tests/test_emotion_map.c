/*
 * Tests for emotion mapping (conversation context → Cartesia emotion strings).
 * Only compiled when HU_ENABLE_CARTESIA=ON.
 */
#include "human/tts/emotion_map.h"
#include "test_framework.h"
#include <stddef.h>
#include <string.h>

#if HU_ENABLE_CARTESIA

static void test_emotion_map_sad_incoming_returns_sympathetic(void) {
    const char *incoming = "I'm so sad today";
    const char *response = "I understand";
    const char *r = hu_cartesia_emotion_from_context(
        incoming, strlen(incoming), response, strlen(response), 14);
    HU_ASSERT_STR_EQ(r, "sympathetic");
}

static void test_emotion_map_congrats_in_response_returns_excited(void) {
    const char *incoming = "I got the job!";
    const char *response = "Congrats! That's awesome news!";
    const char *r = hu_cartesia_emotion_from_context(
        incoming, strlen(incoming), response, strlen(response), 14);
    HU_ASSERT_STR_EQ(r, "excited");
}

static void test_emotion_map_default_returns_content(void) {
    const char *incoming = "What's for dinner?";
    const char *response = "Pizza sounds good.";
    const char *r = hu_cartesia_emotion_from_context(
        incoming, strlen(incoming), response, strlen(response), 14);
    HU_ASSERT_STR_EQ(r, "content");
}

static void test_emotion_map_null_incoming_response_returns_content(void) {
    const char *r = hu_cartesia_emotion_from_context(NULL, 0, NULL, 0, 14);
    HU_ASSERT_STR_EQ(r, "content");
}

static void test_emotion_map_late_night_returns_calm(void) {
    const char *incoming = "Hey";
    const char *response = "Hi there";
    const char *r = hu_cartesia_emotion_from_context(
        incoming, strlen(incoming), response, strlen(response), 23);
    HU_ASSERT_STR_EQ(r, "calm");
}

static void test_emotion_map_early_morning_returns_calm(void) {
    const char *incoming = "Can't sleep";
    const char *response = "I'm here";
    const char *r = hu_cartesia_emotion_from_context(
        incoming, strlen(incoming), response, strlen(response), 3);
    HU_ASSERT_STR_EQ(r, "calm");
}

static void test_emotion_map_playful_lol_returns_joking_comedic(void) {
    const char *incoming = "lol that was funny";
    const char *response = "Glad you liked it";
    const char *r = hu_cartesia_emotion_from_context(
        incoming, strlen(incoming), response, strlen(response), 14);
    HU_ASSERT_STR_EQ(r, "joking/comedic");
}

static void test_emotion_map_serious_death_returns_contemplative(void) {
    const char *incoming = "My grandfather passed away";
    const char *response = "I'm so sorry for your loss.";
    const char *r = hu_cartesia_emotion_from_context(
        incoming, strlen(incoming), response, strlen(response), 14);
    HU_ASSERT_STR_EQ(r, "contemplative");
}

static void test_emotion_map_anxious_incoming_returns_calm(void) {
    const char *incoming = "I'm really anxious about the exam";
    const char *response = "You'll do great.";
    const char *r = hu_cartesia_emotion_from_context(
        incoming, strlen(incoming), response, strlen(response), 14);
    HU_ASSERT_STR_EQ(r, "calm");
}

static void test_emotion_map_upset_incoming_returns_sympathetic(void) {
    const char *incoming = "I'm upset about what happened";
    const char *response = "I hear you.";
    const char *r = hu_cartesia_emotion_from_context(
        incoming, strlen(incoming), response, strlen(response), 14);
    HU_ASSERT_STR_EQ(r, "sympathetic");
}

void run_emotion_map_tests(void) {
    HU_TEST_SUITE("Emotion map");
    HU_RUN_TEST(test_emotion_map_sad_incoming_returns_sympathetic);
    HU_RUN_TEST(test_emotion_map_congrats_in_response_returns_excited);
    HU_RUN_TEST(test_emotion_map_default_returns_content);
    HU_RUN_TEST(test_emotion_map_null_incoming_response_returns_content);
    HU_RUN_TEST(test_emotion_map_late_night_returns_calm);
    HU_RUN_TEST(test_emotion_map_early_morning_returns_calm);
    HU_RUN_TEST(test_emotion_map_playful_lol_returns_joking_comedic);
    HU_RUN_TEST(test_emotion_map_serious_death_returns_contemplative);
    HU_RUN_TEST(test_emotion_map_anxious_incoming_returns_calm);
    HU_RUN_TEST(test_emotion_map_upset_incoming_returns_sympathetic);
}

#else

void run_emotion_map_tests(void) {
    (void)0; /* No-op when Cartesia disabled */
}

#endif /* HU_ENABLE_CARTESIA */
