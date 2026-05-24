/* tests/test_audio_emotion.c
 *
 * Sprint B Story 5 — voice-tone heuristic stub.
 * Contracts (10 tests):
 *   classify:
 *     1. zero/negative duration → UNKNOWN
 *     2. zero word_count → UNKNOWN
 *     3. ~140 wpm → NEUTRAL
 *     4. ~220 wpm → ENERGETIC
 *     5. ~70 wpm with normal duration → DELIBERATE
 *     6. long duration + very low rate → HESITANT (overrides DELIBERATE)
 *     7. boundary: 100 wpm → DELIBERATE; 100.001 wpm → NEUTRAL
 *   label:
 *     8. UNKNOWN → "unknown"; ENERGETIC → "energetic"
 *   render:
 *     9. UNKNOWN → empty; populated → "VOICE TONE: …"
 *    10. NULL/empty args → 0
 */

#include "human/memory/audio_emotion.h"
#include "test_framework.h"

#include <string.h>

static void test_zero_duration_unknown(void) {
    HU_ASSERT_EQ((int)hu_audio_tone_classify(0.0, 10), (int)HU_AUDIO_TONE_UNKNOWN);
    HU_ASSERT_EQ((int)hu_audio_tone_classify(-3.0, 10), (int)HU_AUDIO_TONE_UNKNOWN);
}

static void test_zero_word_count_unknown(void) {
    HU_ASSERT_EQ((int)hu_audio_tone_classify(5.0, 0), (int)HU_AUDIO_TONE_UNKNOWN);
    HU_ASSERT_EQ((int)hu_audio_tone_classify(5.0, -1), (int)HU_AUDIO_TONE_UNKNOWN);
}

static void test_normal_pace_neutral(void) {
    /* 14 words / 6 sec → 140 wpm — comfortably neutral. */
    HU_ASSERT_EQ((int)hu_audio_tone_classify(6.0, 14), (int)HU_AUDIO_TONE_NEUTRAL);
}

static void test_fast_pace_energetic(void) {
    /* 22 words / 6 sec → 220 wpm. */
    HU_ASSERT_EQ((int)hu_audio_tone_classify(6.0, 22), (int)HU_AUDIO_TONE_ENERGETIC);
}

static void test_slow_pace_deliberate(void) {
    /* 7 words / 6 sec → 70 wpm — below SLOW threshold, NOT long
     * enough to be HESITANT. */
    HU_ASSERT_EQ((int)hu_audio_tone_classify(6.0, 7), (int)HU_AUDIO_TONE_DELIBERATE);
}

static void test_long_low_rate_hesitant_overrides_deliberate(void) {
    /* 3 words / 10 sec → 18 wpm, duration >= 5s → HESITANT. */
    HU_ASSERT_EQ((int)hu_audio_tone_classify(10.0, 3), (int)HU_AUDIO_TONE_HESITANT);
}

static void test_boundary_around_slow_threshold(void) {
    /* Exactly 100 wpm (10 words / 6 sec) — predicate is `< 100`, so
     * 100 should fall into NEUTRAL, not DELIBERATE. */
    /* 10 words / 6 sec = 100 wpm exactly. */
    HU_ASSERT_EQ((int)hu_audio_tone_classify(6.0, 10), (int)HU_AUDIO_TONE_NEUTRAL);
    /* 9 words / 6 sec = 90 wpm → DELIBERATE. */
    HU_ASSERT_EQ((int)hu_audio_tone_classify(6.0, 9), (int)HU_AUDIO_TONE_DELIBERATE);
}

static void test_label_lookup(void) {
    HU_ASSERT_STR_EQ(hu_audio_tone_label(HU_AUDIO_TONE_UNKNOWN), "unknown");
    HU_ASSERT_STR_EQ(hu_audio_tone_label(HU_AUDIO_TONE_ENERGETIC), "energetic");
    HU_ASSERT_STR_EQ(hu_audio_tone_label(HU_AUDIO_TONE_DELIBERATE), "deliberate");
    HU_ASSERT_STR_EQ(hu_audio_tone_label(HU_AUDIO_TONE_HESITANT), "hesitant");
    HU_ASSERT_STR_EQ(hu_audio_tone_label(HU_AUDIO_TONE_NEUTRAL), "neutral");
}

static void test_render_unknown_writes_nothing_populated_renders(void) {
    char buf[256] = {0};
    HU_ASSERT_EQ((int)hu_audio_tone_render("alice", HU_AUDIO_TONE_UNKNOWN, buf, sizeof(buf)), 0);

    size_t n = hu_audio_tone_render("alice", HU_AUDIO_TONE_ENERGETIC, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "VOICE TONE:") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "alice") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "energetic") != NULL);
}

static void test_render_null_or_empty_returns_zero(void) {
    char buf[256] = {0};
    HU_ASSERT_EQ((int)hu_audio_tone_render(NULL, HU_AUDIO_TONE_NEUTRAL, buf, sizeof(buf)), 0);
    HU_ASSERT_EQ((int)hu_audio_tone_render("", HU_AUDIO_TONE_NEUTRAL, buf, sizeof(buf)), 0);
    HU_ASSERT_EQ((int)hu_audio_tone_render("alice", HU_AUDIO_TONE_NEUTRAL, NULL, sizeof(buf)), 0);
    HU_ASSERT_EQ((int)hu_audio_tone_render("alice", HU_AUDIO_TONE_NEUTRAL, buf, 0), 0);
}

void run_audio_emotion_tests(void) {
    HU_TEST_SUITE("audio_emotion");
    HU_RUN_TEST(test_zero_duration_unknown);
    HU_RUN_TEST(test_zero_word_count_unknown);
    HU_RUN_TEST(test_normal_pace_neutral);
    HU_RUN_TEST(test_fast_pace_energetic);
    HU_RUN_TEST(test_slow_pace_deliberate);
    HU_RUN_TEST(test_long_low_rate_hesitant_overrides_deliberate);
    HU_RUN_TEST(test_boundary_around_slow_threshold);
    HU_RUN_TEST(test_label_lookup);
    HU_RUN_TEST(test_render_unknown_writes_nothing_populated_renders);
    HU_RUN_TEST(test_render_null_or_empty_returns_zero);
}
