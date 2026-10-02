/* Gap-driven curiosity (src/agent/turn/curiosity_gaps.c). */
#include "human/agent/curiosity_gaps.h"
#include "test_framework.h"

#include <string.h>

static hu_curiosity_topic_t pick(const char *s) {
    return hu_curiosity_gap_pick(s, s ? strlen(s) : 0);
}

static void gap_nothing_known_asks_about_plans_first(void) {
    HU_ASSERT_EQ((int)pick(""), (int)HU_CURIOSITY_PLANS);
    HU_ASSERT_EQ((int)pick(NULL), (int)HU_CURIOSITY_PLANS);
}

static void gap_walks_the_topics_in_order(void) {
    HU_ASSERT_EQ((int)pick("trip to Denver next weekend"), (int)HU_CURIOSITY_WORK);
    HU_ASSERT_EQ((int)pick("trip to Denver next weekend\nnew job at the clinic"),
                 (int)HU_CURIOSITY_PEOPLE);
    HU_ASSERT_EQ((int)pick("trip to Denver next weekend\nnew job at the clinic\n"
                           "her sister is staying over"),
                 (int)HU_CURIOSITY_INTERESTS);
    HU_ASSERT_EQ((int)pick("trip to Denver next weekend\nnew job at the clinic\n"
                           "her sister is staying over\nreally into pottery lately"),
                 (int)HU_CURIOSITY_NONE);
}

static void gap_matches_whole_words_only(void) {
    /* "networking" is not "work", "planet" is not "plan", "sonar" not "son" */
    HU_ASSERT_EQ((int)pick("went to a networking thing\nsaw a planet show\nsonar app"),
                 (int)HU_CURIOSITY_PLANS);
    HU_ASSERT_EQ((int)pick("plans to visit grandma\nshift ran late\nkids are sick\nloves hiking"),
                 (int)HU_CURIOSITY_NONE);
}

static void gap_lines_name_the_topic_and_stay_soft(void) {
    const char *w = hu_curiosity_gap_line(HU_CURIOSITY_WORK);
    HU_ASSERT_NOT_NULL(w);
    HU_ASSERT_NOT_NULL(strstr(w, "work"));
    HU_ASSERT_NOT_NULL(strstr(w, "natural"));
    HU_ASSERT_NULL(hu_curiosity_gap_line(HU_CURIOSITY_NONE));
    for (int t = HU_CURIOSITY_PLANS; t <= HU_CURIOSITY_INTERESTS; t++)
        HU_ASSERT_TRUE(hu_curiosity_gap_line((hu_curiosity_topic_t)t)[0] == '-');
}

static void gap_offer_waits_out_a_question_and_the_cooldown(void) {
    hu_curiosity_gaps_reset_for_test();
    const int64_t t0 = 1790000000;
    /* they asked us something: answer it, don't pivot to our question */
    HU_ASSERT_FALSE(hu_curiosity_gap_offer_now("+15550001111", 12, "u free friday?", 14, t0));
    HU_ASSERT_TRUE(hu_curiosity_gap_offer_now("+15550001111", 12, "ugh long day", 12, t0));
    HU_ASSERT_FALSE(hu_curiosity_gap_offer_now("+15550001111", 12, "ugh long day", 12, t0 + 3600));
    HU_ASSERT_TRUE(hu_curiosity_gap_offer_now("+15550002222", 12, "hey", 3, t0 + 3600));
    HU_ASSERT_TRUE(hu_curiosity_gap_offer_now("+15550001111", 12, "hey", 3,
                                              t0 + HU_CURIOSITY_COOLDOWN_HOURS * 3600));
    HU_ASSERT_FALSE(hu_curiosity_gap_offer_now(NULL, 0, "hey", 3, t0));
}

void run_curiosity_gaps_tests(void) {
    HU_TEST_SUITE("curiosity gaps");
    HU_RUN_TEST(gap_nothing_known_asks_about_plans_first);
    HU_RUN_TEST(gap_walks_the_topics_in_order);
    HU_RUN_TEST(gap_matches_whole_words_only);
    HU_RUN_TEST(gap_lines_name_the_topic_and_stay_soft);
    HU_RUN_TEST(gap_offer_waits_out_a_question_and_the_cooldown);
}
