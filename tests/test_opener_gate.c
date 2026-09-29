/* voiceai 2026-09-27 opener gate: a reaction word now and then is human, on
 * every memo it is a tic. Keep one at most every HU_OPENER_EVERY openers per
 * recipient; otherwise drop it before synthesis. */
#include "human/tts/opener_gate.h"
#include "test_framework.h"

#include <string.h>

static const char *strip(const char *in) {
    static char out[256];
    size_t n = hu_opener_strip(in, strlen(in), out, sizeof(out));
    return n > 0 ? out : NULL;
}

static void test_opener_strip_drops_stacked_reaction_words(void) {
    HU_ASSERT_STR_EQ(strip("Ha! Oh, of course he did."), "Of course he did.");
    HU_ASSERT_STR_EQ(strip("yeah, sounds good"), "Sounds good");
    HU_ASSERT_STR_EQ(strip("Hmmm... maybe later."), "Maybe later.");
}

static void test_opener_strip_keeps_leading_tags(void) {
    HU_ASSERT_STR_EQ(strip("<emotion value=\"content\"/>Oh, love that."),
                     "<emotion value=\"content\"/>Love that.");
    HU_ASSERT_STR_EQ(strip("[laughter] Ha, you're ridiculous."), "[laughter] You're ridiculous.");
}

static void test_opener_strip_leaves_real_words_alone(void) {
    HU_ASSERT_NULL(strip("Hey Mindy, love you."));     /* his opener, not a tic */
    HU_ASSERT_NULL(strip("Well done, proud of you.")); /* "well" opens content */
    HU_ASSERT_NULL(strip("Oho is a word."));           /* word boundary */
    HU_ASSERT_NULL(strip("Hahn called."));
    HU_ASSERT_NULL(strip("Oh?")); /* nothing would be left to say */
}

static void test_opener_gate_keeps_one_in_three_per_recipient(void) {
    hu_opener_gate_t g;
    hu_opener_gate_init(&g, HU_OPENER_EVERY);
    HU_ASSERT_TRUE(hu_opener_gate_keep(&g, "mindy", 5)); /* first may keep one */
    HU_ASSERT_FALSE(hu_opener_gate_keep(&g, "mindy", 5));
    HU_ASSERT_FALSE(hu_opener_gate_keep(&g, "mindy", 5));
    HU_ASSERT_FALSE(hu_opener_gate_keep(&g, "mindy", 5));
    HU_ASSERT_TRUE(hu_opener_gate_keep(&g, "mindy", 5)); /* three dropped, one back */
    HU_ASSERT_TRUE(hu_opener_gate_keep(&g, "mom", 3));   /* others are independent */
    HU_ASSERT_FALSE(hu_opener_gate_keep(&g, "mom", 3));
}

static void test_opener_gate_survives_more_recipients_than_slots(void) {
    hu_opener_gate_t g;
    hu_opener_gate_init(&g, HU_OPENER_EVERY);
    char key[16];
    for (int i = 0; i < HU_OPENER_GATE_SLOTS + 5; i++) {
        int n = snprintf(key, sizeof(key), "c%d", i);
        HU_ASSERT_TRUE(hu_opener_gate_keep(&g, key, (size_t)n));
    }
    /* the most recent recipient is still remembered */
    int n = snprintf(key, sizeof(key), "c%d", HU_OPENER_GATE_SLOTS + 4);
    HU_ASSERT_FALSE(hu_opener_gate_keep(&g, key, (size_t)n));
}

void run_opener_gate_tests(void) {
    HU_TEST_SUITE("opener gate (voiceai port)");
    HU_RUN_TEST(test_opener_strip_drops_stacked_reaction_words);
    HU_RUN_TEST(test_opener_strip_keeps_leading_tags);
    HU_RUN_TEST(test_opener_strip_leaves_real_words_alone);
    HU_RUN_TEST(test_opener_gate_keeps_one_in_three_per_recipient);
    HU_RUN_TEST(test_opener_gate_survives_more_recipients_than_slots);
}
