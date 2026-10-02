/* Voice triggers v2 (HU_VOICE_TRIGGERS_V2): four more moments a person would
 * talk. Each predicate: positive, negative and boundary. */
#include "human/context/voice_triggers.h"
#include "test_framework.h"

#include <string.h>

#define S(x) (x), strlen(x)

static void test_voice_v2_story_long_message(void) {
    /* 140 chars exactly is a story; 139 is not (no narrative marker). */
    char at[HU_VOICE_V2_STORY_LONG_CHARS + 1];
    memset(at, 'a', sizeof(at) - 1);
    at[sizeof(at) - 1] = '\0';
    for (size_t i = 5; i < sizeof(at) - 1; i += 6)
        at[i] = ' ';
    HU_ASSERT_TRUE(hu_voice_v2_story_inbound(at, HU_VOICE_V2_STORY_LONG_CHARS));
    HU_ASSERT_FALSE(hu_voice_v2_story_inbound(at, HU_VOICE_V2_STORY_LONG_CHARS - 1));
    /* A pasted link is not a story, however long. */
    const char *link =
        "check this out https://example.com/a/very/long/path/that/goes/on/and/on/"
        "and/on/for/a/while/until/it/passes/the/one/hundred/forty/character/line/mark";
    HU_ASSERT_TRUE(strlen(link) >= HU_VOICE_V2_STORY_LONG_CHARS);
    HU_ASSERT_FALSE(hu_voice_v2_story_inbound(S(link)));
}

static void test_voice_v2_story_narrative(void) {
    /* Two sentences, a past-tense marker, >= 80 chars. */
    const char *story = "We went to the lake this morning with the kids. "
                        "Ella caught her first fish and screamed.";
    HU_ASSERT_TRUE(strlen(story) >= HU_VOICE_V2_STORY_NARRATIVE_CHARS);
    HU_ASSERT_TRUE(hu_voice_v2_story_inbound(S(story)));
    /* A feeling word counts as a marker too. */
    HU_ASSERT_TRUE(hu_voice_v2_story_inbound(
        S("The interview is tomorrow at nine downtown. Honestly I am so nervous about it all.")));
    /* One sentence, even with a marker: not a story. */
    HU_ASSERT_FALSE(hu_voice_v2_story_inbound(S("we went to the store and got all the stuff for "
                                                "the party on saturday night so we are set")));
    /* Two sentences, no marker: plans, not a story. */
    HU_ASSERT_FALSE(hu_voice_v2_story_inbound(
        S("Can you bring the folding chairs on Saturday. Also the big cooler from the garage.")));
    /* Two short sentences with a marker but under 80 chars. */
    HU_ASSERT_FALSE(hu_voice_v2_story_inbound(S("It was fine. We went home after.")));
    /* Word boundary: "washed" is not "was", "wanted" is not "went". */
    HU_ASSERT_FALSE(hu_voice_v2_story_inbound(
        S("Please get the car washed before noon. And grab coffee beans for the morning pot.")));
    HU_ASSERT_FALSE(hu_voice_v2_story_inbound(NULL, 0));
}

static void test_voice_v2_memo_length_reply(void) {
    HU_ASSERT_TRUE(hu_voice_v2_memo_length_reply(HU_VOICE_V2_MEMO_PLANNED_CHARS));
    HU_ASSERT_TRUE(hu_voice_v2_memo_length_reply(640));
    HU_ASSERT_FALSE(hu_voice_v2_memo_length_reply(HU_VOICE_V2_MEMO_PLANNED_CHARS - 1));
    HU_ASSERT_FALSE(hu_voice_v2_memo_length_reply(0)); /* unknown */
}

static void test_voice_v2_late_evening_warmth(void) {
    const char *msg = "just got the kids down finally";
    HU_ASSERT_TRUE(hu_voice_v2_late_evening_warmth(HU_VOICE_V2_EVENING_START_MIN, true, S(msg)));
    HU_ASSERT_TRUE(hu_voice_v2_late_evening_warmth(HU_VOICE_V2_EVENING_END_MIN, true, S(msg)));
    HU_ASSERT_TRUE(hu_voice_v2_late_evening_warmth(21 * 60 + 15, true, S(msg)));
    HU_ASSERT_FALSE(
        hu_voice_v2_late_evening_warmth(HU_VOICE_V2_EVENING_START_MIN - 1, true, S(msg)));
    HU_ASSERT_FALSE(hu_voice_v2_late_evening_warmth(HU_VOICE_V2_EVENING_END_MIN + 1, true, S(msg)));
    HU_ASSERT_FALSE(hu_voice_v2_late_evening_warmth(21 * 60, false, S(msg)));       /* not close */
    HU_ASSERT_FALSE(hu_voice_v2_late_evening_warmth(-1, true, S(msg)));             /* unknown */
    HU_ASSERT_FALSE(hu_voice_v2_late_evening_warmth(21 * 60, true, S("ok night"))); /* 2 words */
    HU_ASSERT_TRUE(hu_voice_v2_late_evening_warmth(21 * 60, true, S("long day for me too")));
    /* Logistics is never warmth. */
    HU_ASSERT_FALSE(
        hu_voice_v2_late_evening_warmth(21 * 60, true, S("what time is pickup tomorrow?")));
}

static void test_voice_v2_long_gap_reconnect(void) {
    HU_ASSERT_TRUE(hu_voice_v2_long_gap_reconnect(HU_VOICE_V2_RECONNECT_GAP_SEC, true));
    HU_ASSERT_TRUE(hu_voice_v2_long_gap_reconnect(10 * 86400, true));
    HU_ASSERT_FALSE(hu_voice_v2_long_gap_reconnect(HU_VOICE_V2_RECONNECT_GAP_SEC - 1, true));
    HU_ASSERT_FALSE(hu_voice_v2_long_gap_reconnect(10 * 86400, false)); /* not close */
    HU_ASSERT_FALSE(hu_voice_v2_long_gap_reconnect(-1, true));          /* unknown */
}

static void test_voice_v2_close_contact(void) {
    hu_contact_profile_t cp;
    memset(&cp, 0, sizeof(cp));
    HU_ASSERT_FALSE(hu_voice_v2_close_contact(NULL));
    HU_ASSERT_FALSE(hu_voice_v2_close_contact(&cp));
    cp.dunbar_layer = "intimate";
    HU_ASSERT_TRUE(hu_voice_v2_close_contact(&cp));
    cp.dunbar_layer = "Close";
    HU_ASSERT_TRUE(hu_voice_v2_close_contact(&cp));
    cp.dunbar_layer = "active";
    HU_ASSERT_FALSE(hu_voice_v2_close_contact(&cp));
    cp.dunbar_layer = "closed-off"; /* whole value, not a substring */
    HU_ASSERT_FALSE(hu_voice_v2_close_contact(&cp));
    cp.dunbar_layer = NULL;
    cp.relationship_type = "family";
    HU_ASSERT_TRUE(hu_voice_v2_close_contact(&cp));
    cp.relationship_type = "romantic";
    HU_ASSERT_TRUE(hu_voice_v2_close_contact(&cp));
    cp.relationship_type = "coworker";
    HU_ASSERT_FALSE(hu_voice_v2_close_contact(&cp));
}

static void test_voice_v2_weekly_cap_parse(void) {
    HU_ASSERT_EQ(hu_voice_v2_parse_weekly_cap(NULL), HU_VOICE_V2_WEEKLY_CAP_DEFAULT);
    HU_ASSERT_EQ(hu_voice_v2_parse_weekly_cap(""), HU_VOICE_V2_WEEKLY_CAP_DEFAULT);
    HU_ASSERT_EQ(hu_voice_v2_parse_weekly_cap("lots"), HU_VOICE_V2_WEEKLY_CAP_DEFAULT);
    HU_ASSERT_EQ(hu_voice_v2_parse_weekly_cap("-1"), HU_VOICE_V2_WEEKLY_CAP_DEFAULT);
    HU_ASSERT_EQ(hu_voice_v2_parse_weekly_cap("15"), HU_VOICE_V2_WEEKLY_CAP_DEFAULT);
    HU_ASSERT_EQ(hu_voice_v2_parse_weekly_cap("0"), 0u);
    HU_ASSERT_EQ(hu_voice_v2_parse_weekly_cap("3"), 3u);
}

static hu_voice_v2_facts_t facts(const char *inbound) {
    hu_voice_v2_facts_t f = {.inbound = inbound,
                             .inbound_len = inbound ? strlen(inbound) : 0,
                             .planned_reply_chars = 120,
                             .local_minute = 12 * 60,
                             .close_contact = true,
                             .secs_since_owner_reply = 3600,
                             .v2_memos_this_week = 0,
                             .weekly_cap = HU_VOICE_V2_WEEKLY_CAP_DEFAULT};
    return f;
}

static void test_voice_v2_decide_picks_each_reason(void) {
    const char *why = NULL;
    hu_voice_v2_facts_t f = facts("sounds good");
    HU_ASSERT_FALSE(hu_voice_v2_decide(&f, &why));
    HU_ASSERT_STR_EQ(why, "none");

    f.planned_reply_chars = HU_VOICE_V2_MEMO_PLANNED_CHARS;
    HU_ASSERT_TRUE(hu_voice_v2_decide(&f, &why));
    HU_ASSERT_STR_EQ(why, "memo_length_reply");

    f = facts("long day for me too");
    f.local_minute = 21 * 60;
    HU_ASSERT_TRUE(hu_voice_v2_decide(&f, &why));
    HU_ASSERT_STR_EQ(why, "late_evening_warmth");

    f = facts("hey stranger");
    f.secs_since_owner_reply = 4 * 86400;
    HU_ASSERT_TRUE(hu_voice_v2_decide(&f, &why));
    HU_ASSERT_STR_EQ(why, "long_gap_reconnect");

    f = facts("We went to the lake this morning with the kids. Ella caught her first fish "
              "and screamed.");
    HU_ASSERT_TRUE(hu_voice_v2_decide(&f, &why));
    HU_ASSERT_STR_EQ(why, "story_inbound");
}

static void test_voice_v2_decide_weekly_cap(void) {
    const char *why = NULL;
    hu_voice_v2_facts_t f = facts("hey stranger");
    f.secs_since_owner_reply = 4 * 86400;
    f.v2_memos_this_week = HU_VOICE_V2_WEEKLY_CAP_DEFAULT - 1;
    HU_ASSERT_TRUE(hu_voice_v2_decide(&f, &why));
    f.v2_memos_this_week = HU_VOICE_V2_WEEKLY_CAP_DEFAULT;
    HU_ASSERT_FALSE(hu_voice_v2_decide(&f, &why));
    HU_ASSERT_STR_EQ(why, "weekly_cap");
    f.v2_memos_this_week = 0;
    f.weekly_cap = 0; /* operator turned the v2 reasons off */
    HU_ASSERT_FALSE(hu_voice_v2_decide(&f, &why));
    HU_ASSERT_STR_EQ(why, "weekly_cap");
    HU_ASSERT_FALSE(hu_voice_v2_decide(NULL, &why));
    HU_ASSERT_STR_EQ(why, "none");
}

void run_voice_triggers_tests(void) {
    HU_TEST_SUITE("voice triggers v2");
    HU_RUN_TEST(test_voice_v2_story_long_message);
    HU_RUN_TEST(test_voice_v2_story_narrative);
    HU_RUN_TEST(test_voice_v2_memo_length_reply);
    HU_RUN_TEST(test_voice_v2_late_evening_warmth);
    HU_RUN_TEST(test_voice_v2_long_gap_reconnect);
    HU_RUN_TEST(test_voice_v2_close_contact);
    HU_RUN_TEST(test_voice_v2_weekly_cap_parse);
    HU_RUN_TEST(test_voice_v2_decide_picks_each_reason);
    HU_RUN_TEST(test_voice_v2_decide_weekly_cap);
}
