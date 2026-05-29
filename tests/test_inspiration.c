#include "human/inspiration.h"
#include "test_framework.h"
#include <string.h>

static void test_inspiration_voice_hint_includes_formality_and_traits(void) {
    char buf[256];
    size_t n = hu_inspiration_build_voice_hint("casual", "dry humor,tech-nerd", buf, sizeof(buf));
    HU_ASSERT(n > 0);
    HU_ASSERT(strstr(buf, "casual") != NULL);
    HU_ASSERT(strstr(buf, "dry humor") != NULL);
}

static void test_inspiration_voice_hint_neutral_when_absent(void) {
    char buf[256];
    size_t n = hu_inspiration_build_voice_hint(NULL, NULL, buf, sizeof(buf));
    HU_ASSERT_EQ((int)n, 0);
}

static void test_inspiration_system_prompt_differs_per_medium(void) {
    const char *m = hu_inspiration_system_prompt(HU_INSPIRATION_MUSIC);
    const char *y = hu_inspiration_system_prompt(HU_INSPIRATION_YOUTUBE);
    const char *t = hu_inspiration_system_prompt(HU_INSPIRATION_TIKTOK);
    HU_ASSERT(m && y && t);
    HU_ASSERT(strcmp(m, y) != 0 && strcmp(y, t) != 0);
}

static void test_inspiration_tiktok_tag_url_basic(void) {
    char buf[128];
    size_t n = hu_tiktok_tag_url("#latteart", 9, buf, sizeof(buf));
    HU_ASSERT(n > 0);
    HU_ASSERT_STR_EQ(buf, "https://www.tiktok.com/tag/latteart");
}

static void test_inspiration_tiktok_tag_url_multiword_collapses(void) {
    char buf[128];
    size_t n = hu_tiktok_tag_url("latte art", 9, buf, sizeof(buf));
    HU_ASSERT(n > 0);
    HU_ASSERT_STR_EQ(buf, "https://www.tiktok.com/tag/latteart");
}

static void test_inspiration_pick_medium_routes_and_falls_back(void) {
    HU_ASSERT_EQ((int)hu_inspiration_pick_medium("send me a tiktok", 16, true),
                 (int)HU_INSPIRATION_TIKTOK);
    HU_ASSERT_EQ((int)hu_inspiration_pick_medium("got a funny video?", 18, true),
                 (int)HU_INSPIRATION_YOUTUBE);
    HU_ASSERT_EQ((int)hu_inspiration_pick_medium("got a funny video?", 18, false),
                 (int)HU_INSPIRATION_MUSIC);
    HU_ASSERT_EQ((int)hu_inspiration_pick_medium("what a day", 10, true),
                 (int)HU_INSPIRATION_MUSIC);
}

static void test_inspiration_pick_medium_word_boundary_safety(void) {
    /* "trendy" contains "trend" (a tiktok cue) as a substring but is a different word.
       Word-boundary match must NOT route it to TIKTOK on the basis of substring overlap. */
    HU_ASSERT_EQ((int)hu_inspiration_pick_medium("feeling trendy today", 20, true),
                 (int)HU_INSPIRATION_MUSIC);
}

void run_inspiration_tests(void) {
    HU_TEST_SUITE("inspiration");
    HU_RUN_TEST(test_inspiration_voice_hint_includes_formality_and_traits);
    HU_RUN_TEST(test_inspiration_voice_hint_neutral_when_absent);
    HU_RUN_TEST(test_inspiration_system_prompt_differs_per_medium);
    HU_RUN_TEST(test_inspiration_tiktok_tag_url_basic);
    HU_RUN_TEST(test_inspiration_tiktok_tag_url_multiword_collapses);
    HU_RUN_TEST(test_inspiration_pick_medium_routes_and_falls_back);
    HU_RUN_TEST(test_inspiration_pick_medium_word_boundary_safety);
}
