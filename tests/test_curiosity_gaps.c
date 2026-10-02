/* Gap-driven curiosity (src/agent/turn/curiosity_gaps.c). */
#include "human/agent/curiosity_gaps.h"
#include "test_framework.h"

#include <string.h>

static hu_curiosity_topic_t pick(const char *s, hu_curiosity_topic_t after) {
    return hu_curiosity_gap_pick(s, s ? strlen(s) : 0, after);
}

static const char k_plans_work[] = "trip to Denver next weekend\nnew job at the clinic";

static void gap_nothing_known_starts_with_plans(void) {
    HU_ASSERT_EQ((int)pick("", HU_CURIOSITY_NONE), (int)HU_CURIOSITY_PLANS);
    HU_ASSERT_EQ((int)pick(NULL, HU_CURIOSITY_NONE), (int)HU_CURIOSITY_PLANS);
}

static void gap_skips_covered_topics(void) {
    HU_ASSERT_EQ((int)pick("trip to Denver next weekend", HU_CURIOSITY_NONE),
                 (int)HU_CURIOSITY_WORK);
    HU_ASSERT_EQ((int)pick(k_plans_work, HU_CURIOSITY_NONE), (int)HU_CURIOSITY_PEOPLE);
    HU_ASSERT_EQ((int)pick("trip to Denver next weekend\nnew job at the clinic\n"
                           "her sister is staying over\nreally into pottery lately",
                           HU_CURIOSITY_NONE),
                 (int)HU_CURIOSITY_NONE);
}

/* Always offering the first gap would ask "anything coming up?" every 72 hours
 * for anyone whose answer never became a note. */
static void gap_rotates_past_the_last_topic_offered(void) {
    HU_ASSERT_EQ((int)pick("", HU_CURIOSITY_PLANS), (int)HU_CURIOSITY_WORK);
    HU_ASSERT_EQ((int)pick("", HU_CURIOSITY_INTERESTS), (int)HU_CURIOSITY_PLANS);
    HU_ASSERT_EQ((int)pick(k_plans_work, HU_CURIOSITY_PEOPLE), (int)HU_CURIOSITY_INTERESTS);
    HU_ASSERT_EQ((int)pick(k_plans_work, HU_CURIOSITY_INTERESTS), (int)HU_CURIOSITY_PEOPLE);
}

static void gap_matches_whole_words_only(void) {
    /* "networking" is not "work", "planet" is not "plan", "sonar" not "son" */
    HU_ASSERT_EQ(
        (int)pick("went to a networking thing\nsaw a planet show\nsonar app", HU_CURIOSITY_NONE),
        (int)HU_CURIOSITY_PLANS);
    HU_ASSERT_EQ((int)pick("plans to visit grandma\nshift ran late\nkids are sick\nloves hiking",
                           HU_CURIOSITY_NONE),
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

static hu_curiosity_topic_t offer(const char *who, const char *inbound, int64_t now, bool commit) {
    return hu_curiosity_gap_offer(who, strlen(who), inbound, strlen(inbound), "", 0, now, commit);
}

static void gap_offer_waits_out_a_question_and_the_cooldown(void) {
    hu_curiosity_gaps_reset_for_test();
    const int64_t t0 = 1790000000;
    const int64_t cool = (int64_t)HU_CURIOSITY_COOLDOWN_HOURS * 3600;
    /* they asked us something: answer it, don't pivot to our question */
    HU_ASSERT_EQ((int)offer("+15550001111", "u free friday?", t0, true), (int)HU_CURIOSITY_NONE);
    HU_ASSERT_EQ((int)offer("+15550001111", "ugh long day", t0, true), (int)HU_CURIOSITY_PLANS);
    HU_ASSERT_EQ((int)offer("+15550001111", "ugh long day", t0 + 3600, true),
                 (int)HU_CURIOSITY_NONE);
    HU_ASSERT_EQ((int)offer("+15550002222", "hey", t0 + 3600, true), (int)HU_CURIOSITY_PLANS);
    /* after the cooldown, the next topic, not the same one again */
    HU_ASSERT_EQ((int)offer("+15550001111", "hey", t0 + cool, true), (int)HU_CURIOSITY_WORK);
    HU_ASSERT_EQ((int)hu_curiosity_gap_offer(NULL, 0, "hey", 3, "", 0, t0, true),
                 (int)HU_CURIOSITY_NONE);
}

/* SHADOW measures how often a gap would be offered; it must not spend the
 * budget, or the measured rate is capped at one per 72 hours. */
static void gap_offer_without_commit_leaves_the_budget(void) {
    hu_curiosity_gaps_reset_for_test();
    const int64_t t0 = 1790000000;
    HU_ASSERT_EQ((int)offer("+15550003333", "hey", t0, false), (int)HU_CURIOSITY_PLANS);
    HU_ASSERT_EQ((int)offer("+15550003333", "hey", t0 + 60, false), (int)HU_CURIOSITY_PLANS);
    HU_ASSERT_EQ((int)offer("+15550003333", "hey", t0 + 120, true), (int)HU_CURIOSITY_PLANS);
    HU_ASSERT_EQ((int)offer("+15550003333", "hey", t0 + 180, false), (int)HU_CURIOSITY_NONE);
}

void run_curiosity_gaps_tests(void) {
    HU_TEST_SUITE("curiosity gaps");
    HU_RUN_TEST(gap_nothing_known_starts_with_plans);
    HU_RUN_TEST(gap_skips_covered_topics);
    HU_RUN_TEST(gap_rotates_past_the_last_topic_offered);
    HU_RUN_TEST(gap_matches_whole_words_only);
    HU_RUN_TEST(gap_lines_name_the_topic_and_stay_soft);
    HU_RUN_TEST(gap_offer_waits_out_a_question_and_the_cooldown);
    HU_RUN_TEST(gap_offer_without_commit_leaves_the_budget);
}
