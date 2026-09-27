/* D2: a directed line is spoken only if every tag is in Cartesia's palette,
 * values are in range and the budgets hold. */
#include "human/tts/speech_direction.h"
#include "test_framework.h"

#include <string.h>

static hu_direction_verdict_t parse(const char *s, hu_direction_t *d) {
    return hu_direction_parse(s, strlen(s), NULL, d);
}

static void test_direction_parses_a_valid_line(void) {
    static hu_direction_t d;
    HU_ASSERT_EQ(parse("<emotion value=\"excited\"/>Wait, that's amazing! <break time=\"250ms\"/>"
                       "<emotion value=\"proud\"/>I'm so proud of you.",
                       &d),
                 HU_DIRECTION_OK);
    HU_ASSERT_EQ(d.count, 2);
    HU_ASSERT_STR_EQ(d.seg[0].emotion, "excited");
    HU_ASSERT_STR_EQ(d.seg[1].emotion, "proud");
    HU_ASSERT_EQ(d.seg[1].break_ms, 250);
    HU_ASSERT_STR_EQ(d.words, "Wait, that's amazing! I'm so proud of you.");
    HU_ASSERT_EQ(d.sentences, 2);
    HU_ASSERT_STR_EQ(hu_direction_first_emotion(&d), "excited");
}

static void test_direction_rejects_tags_outside_the_palette(void) {
    static hu_direction_t d;
    HU_ASSERT_EQ(parse("<prosody rate=\"slow\">hey</prosody>", &d), HU_DIRECTION_BAD_TAG);
    HU_ASSERT_EQ(parse("<emotion value=\"joyful\"/>hey there", &d), HU_DIRECTION_BAD_EMOTION);
    HU_ASSERT_EQ(parse("<speed ratio=\"fast\"/>hey", &d), HU_DIRECTION_BAD_TAG);
}

static void test_direction_rejects_unclosed_tag(void) {
    static hu_direction_t d;
    HU_ASSERT_EQ(parse("<emotion value=\"sad\" hey there", &d), HU_DIRECTION_BAD_TAG);
    HU_ASSERT_EQ(parse("hey the<break time=\"200ms\"re", &d), HU_DIRECTION_BAD_TAG);
}

static void test_direction_rejects_stage_directions_and_emoji(void) {
    static hu_direction_t d;
    HU_ASSERT_EQ(parse("*laughs* that's great", &d), HU_DIRECTION_STAGE_DIRECTION);
    HU_ASSERT_EQ(parse("(sighs) fine", &d), HU_DIRECTION_STAGE_DIRECTION);
    HU_ASSERT_EQ(parse("[pause] sure", &d), HU_DIRECTION_STAGE_DIRECTION);
    HU_ASSERT_EQ(parse("love you \xF0\x9F\x98\x8D", &d), HU_DIRECTION_EMOJI);
}

static void test_direction_clamps_values(void) {
    static hu_direction_t d;
    HU_ASSERT_EQ(
        parse("<speed ratio=\"2.0\"/><volume ratio=\"0.2\"/><break time=\"3s\"/>ok then", &d),
        HU_DIRECTION_OK);
    HU_ASSERT_TRUE(d.seg[0].speed > 1.09f && d.seg[0].speed < 1.11f);
    HU_ASSERT_TRUE(d.seg[0].volume > 0.84f && d.seg[0].volume < 0.86f);
    HU_ASSERT_EQ(d.seg[0].break_ms, 800);
}

static void test_direction_enforces_budgets(void) {
    static hu_direction_t d;
    HU_ASSERT_EQ(parse("[laughter] that's hilarious. [laughter] stop it.", &d),
                 HU_DIRECTION_OVER_BUDGET);
    HU_ASSERT_EQ(parse("<emotion value=\"sad\"/>oh no. <emotion value=\"excited\"/>wait "
                       "<emotion value=\"calm\"/>what!",
                       &d),
                 HU_DIRECTION_OVER_BUDGET); /* 2 changes in 2 sentences > max(1, 1) */
    HU_ASSERT_EQ(parse("<speed ratio=\"0.9\"/>slow. <speed ratio=\"1.05\"/>fast.", &d),
                 HU_DIRECTION_OVER_BUDGET);
    HU_ASSERT_EQ(parse("[laughter] that's hilarious, you're ridiculous.", &d), HU_DIRECTION_OK);
}

static void test_direction_empty_and_tags_only(void) {
    static hu_direction_t d;
    HU_ASSERT_EQ(parse("", &d), HU_DIRECTION_EMPTY);
    HU_ASSERT_EQ(parse("<emotion value=\"calm\"/><break time=\"200ms\"/>", &d), HU_DIRECTION_EMPTY);
}

static void test_direction_emotion_list_is_cartesias(void) {
    HU_ASSERT_EQ(hu_direction_emotion_count(), 58); /* docs list 58 names */
    HU_ASSERT_TRUE(hu_direction_emotion_valid("affectionate", 12));
    HU_ASSERT_TRUE(hu_direction_emotion_valid("Excited", 7));
    HU_ASSERT_FALSE(hu_direction_emotion_valid("joyful", 6));
    HU_ASSERT_FALSE(hu_direction_emotion_valid("sad", 2)); /* exact length, no prefixes */
}

static void test_direction_verdict_names_are_distinct(void) {
    HU_ASSERT_STR_EQ(hu_direction_verdict_name(HU_DIRECTION_OK), "ok");
    HU_ASSERT_STR_EQ(hu_direction_verdict_name(HU_DIRECTION_BAD_TAG), "bad_tag");
    HU_ASSERT_STR_EQ(hu_direction_verdict_name(HU_DIRECTION_OVER_BUDGET), "over_budget");
}

void run_speech_direction_tests(void) {
    HU_TEST_SUITE("speech direction (D2)");
    HU_RUN_TEST(test_direction_parses_a_valid_line);
    HU_RUN_TEST(test_direction_rejects_tags_outside_the_palette);
    HU_RUN_TEST(test_direction_rejects_unclosed_tag);
    HU_RUN_TEST(test_direction_rejects_stage_directions_and_emoji);
    HU_RUN_TEST(test_direction_clamps_values);
    HU_RUN_TEST(test_direction_enforces_budgets);
    HU_RUN_TEST(test_direction_empty_and_tags_only);
    HU_RUN_TEST(test_direction_emotion_list_is_cartesias);
    HU_RUN_TEST(test_direction_verdict_names_are_distinct);
}
