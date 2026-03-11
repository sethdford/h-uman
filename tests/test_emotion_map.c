/*
 * Tests for emotion mapping (conversation emotion → Cartesia emotion strings).
 * Only compiled when HU_ENABLE_CARTESIA=ON.
 */
#include "human/tts/cartesia.h"
#include "test_framework.h"
#include <stddef.h>

#if HU_ENABLE_CARTESIA

static void test_emotion_map_excited_returns_excited(void) {
    const char *r = hu_cartesia_emotion_from_context("excited", 1);
    HU_ASSERT_STR_EQ(r, "excited");
}

static void test_emotion_map_happy_returns_excited(void) {
    const char *r = hu_cartesia_emotion_from_context("happy", 1);
    HU_ASSERT_STR_EQ(r, "excited");
}

static void test_emotion_map_sad_returns_sympathetic(void) {
    const char *r = hu_cartesia_emotion_from_context("sad", 1);
    HU_ASSERT_STR_EQ(r, "sympathetic");
}

static void test_emotion_map_null_returns_content(void) {
    const char *r = hu_cartesia_emotion_from_context(NULL, 1);
    HU_ASSERT_STR_EQ(r, "content");
}

static void test_emotion_map_empty_returns_content(void) {
    const char *r = hu_cartesia_emotion_from_context("", 1);
    HU_ASSERT_STR_EQ(r, "content");
}

static void test_emotion_map_energy_high_returns_excited(void) {
    const char *r = hu_cartesia_emotion_from_context("unknown_emotion", 2);
    HU_ASSERT_STR_EQ(r, "excited");
}

static void test_emotion_map_energy_low_returns_calm(void) {
    const char *r = hu_cartesia_emotion_from_context("unknown_emotion", 0);
    HU_ASSERT_STR_EQ(r, "calm");
}

static void test_emotion_map_melancholic_returns_sympathetic(void) {
    const char *r = hu_cartesia_emotion_from_context("melancholic", 1);
    HU_ASSERT_STR_EQ(r, "sympathetic");
}

static void test_emotion_map_stressed_returns_calm(void) {
    const char *r = hu_cartesia_emotion_from_context("stressed", 1);
    HU_ASSERT_STR_EQ(r, "calm");
}

static void test_emotion_map_playful_returns_joking_comedic(void) {
    const char *r = hu_cartesia_emotion_from_context("playful", 1);
    HU_ASSERT_STR_EQ(r, "joking/comedic");
}

void run_emotion_map_tests(void) {
    HU_TEST_SUITE("Emotion map");
    HU_RUN_TEST(test_emotion_map_excited_returns_excited);
    HU_RUN_TEST(test_emotion_map_happy_returns_excited);
    HU_RUN_TEST(test_emotion_map_sad_returns_sympathetic);
    HU_RUN_TEST(test_emotion_map_null_returns_content);
    HU_RUN_TEST(test_emotion_map_empty_returns_content);
    HU_RUN_TEST(test_emotion_map_energy_high_returns_excited);
    HU_RUN_TEST(test_emotion_map_energy_low_returns_calm);
    HU_RUN_TEST(test_emotion_map_melancholic_returns_sympathetic);
    HU_RUN_TEST(test_emotion_map_stressed_returns_calm);
    HU_RUN_TEST(test_emotion_map_playful_returns_joking_comedic);
}

#else

void run_emotion_map_tests(void) {
    (void)0; /* No-op when Cartesia disabled */
}

#endif /* HU_ENABLE_CARTESIA */
