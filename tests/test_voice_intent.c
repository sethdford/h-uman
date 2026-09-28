/* Voice-first memos (spec 2026-09-28): voice is decided from what arrived,
 * before the reply is written. One test per rule row of the spec's table. */
#include "human/context/voice_intent.h"
#include "test_framework.h"

#include <string.h>

static hu_voice_messages_config_t g_cfg;

static const char *decide(const char *inbound, int64_t since_last) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.enabled = true;
    hu_voice_intent_facts_t f = {.inbound = inbound,
                                 .inbound_len = strlen(inbound),
                                 .cfg = &g_cfg,
                                 .has_voice_id = true,
                                 .secs_since_last_memo = since_last,
                                 .min_gap_sec = 10800};
    const char *why = NULL;
    hu_voice_decision_t d = hu_voice_intent_decide(&f, &why);
    HU_ASSERT_NOT_NULL(why);
    HU_ASSERT_EQ((int)d, strncmp(why, "they_sent_audio", 15) == 0 ||
                                 strcmp(why, "heartfelt") == 0 ||
                                 strcmp(why, "question_worth_talking") == 0
                             ? (int)HU_VOICE_SEND_VOICE
                             : (int)HU_VOICE_SEND_TEXT);
    return why;
}

static void test_voice_intent_needs_a_voice_and_the_feature(void) {
    hu_voice_messages_config_t cfg = {.enabled = true};
    hu_voice_intent_facts_t f = {.inbound = "I miss you", .inbound_len = 10, .cfg = &cfg};
    const char *why = NULL;
    HU_ASSERT_EQ((int)hu_voice_intent_decide(&f, &why), (int)HU_VOICE_SEND_TEXT);
    HU_ASSERT_STR_EQ(why, "no_voice_id");
    f.has_voice_id = true;
    cfg.enabled = false;
    HU_ASSERT_EQ((int)hu_voice_intent_decide(&f, &why), (int)HU_VOICE_SEND_TEXT);
    HU_ASSERT_STR_EQ(why, "disabled");
}

static void test_voice_intent_answers_audio_with_audio_even_inside_the_gap(void) {
    HU_ASSERT_STR_EQ(decide("[Audio transcription: Yes it is getting better]", 60),
                     "they_sent_audio");
}

static void test_voice_intent_keeps_logistics_as_text(void) {
    HU_ASSERT_STR_EQ(decide("what time should I come over?", -1), "logistics");
    HU_ASSERT_STR_EQ(decide("wherever you want is fine", -1), "no_trigger");
}

static void test_voice_intent_spaces_memos_to_one_person(void) {
    HU_ASSERT_STR_EQ(decide("I'm so proud of you", 3600), "spacing");
    HU_ASSERT_STR_EQ(decide("I'm so proud of you", 20000), "heartfelt");
}

static void test_voice_intent_heartfelt_is_whole_words(void) {
    HU_ASSERT_STR_EQ(decide("I'm so proud of you", -1), "heartfelt");
    HU_ASSERT_STR_EQ(decide("Grandpa passed away this morning", -1), "heartfelt");
    HU_ASSERT_STR_EQ(decide("my keys are missing again", -1), "no_trigger");
    HU_ASSERT_STR_EQ(decide("the lovely weather here", -1), "no_trigger");
}

static void test_voice_intent_talks_through_real_questions_only(void) {
    HU_ASSERT_STR_EQ(decide("do you think I should take the job in Denver or stay here?", -1),
                     "question_worth_talking");
    HU_ASSERT_STR_EQ(decide("you coming?", -1), "no_trigger");
}

/* Review C2: a pre-decided memo is voiced only if the turn produced a memo —
 * a slim retry or a canned fallback goes back to the text classifier. */
static void test_voice_intent_memo_shape(void) {
    const char *slim = "Sounds good, talk soon.";
    HU_ASSERT_FALSE(hu_voice_intent_memo_shaped(slim, strlen(slim)));
    const char *memo = "Honestly I have been thinking about it all day, and I think you should "
                       "go for it, it is scary but you would regret not trying.";
    HU_ASSERT_TRUE(hu_voice_intent_memo_shaped(memo, strlen(memo)));
    char rambling[1400] = "";
    for (int i = 0; i < 115; i++)
        strcat(rambling, i ? " word" : "word");
    HU_ASSERT_FALSE(hu_voice_intent_memo_shaped(rambling, strlen(rambling)));
}

/* Review I4: everyday uses of the heartfelt words are not moments. */
static void test_voice_intent_ignores_everyday_love_and_sorry(void) {
    HU_ASSERT_STR_EQ(decide("sorry running late", -1), "no_trigger");
    HU_ASSERT_STR_EQ(decide("I'd love to, count me in", -1), "no_trigger");
    HU_ASSERT_STR_EQ(decide("love that place", -1), "no_trigger");
    HU_ASSERT_STR_EQ(decide("love you so much", -1), "heartfelt");
    HU_ASSERT_STR_EQ(decide("I'm so sorry about your dad", -1), "heartfelt");
}

/* Review I4: "when"/"where" in a real moment is not logistics. */
static void test_voice_intent_logistics_is_a_short_question(void) {
    HU_ASSERT_STR_EQ(decide("I miss you, when are you home?", -1), "heartfelt");
    HU_ASSERT_STR_EQ(decide("I cried when I heard the news", -1), "heartfelt");
    HU_ASSERT_STR_EQ(decide("where are you guys?", -1), "logistics");
}

static void test_voice_intent_gap_parse_rejects_nonsense(void) {
    HU_ASSERT_EQ(hu_voice_intent_parse_gap(NULL), 10800u);
    HU_ASSERT_EQ(hu_voice_intent_parse_gap(""), 10800u);
    HU_ASSERT_EQ(hu_voice_intent_parse_gap("-1"), 10800u);
    HU_ASSERT_EQ(hu_voice_intent_parse_gap("soon"), 10800u);
    HU_ASSERT_EQ(hu_voice_intent_parse_gap("0"), 0u);
    HU_ASSERT_EQ(hu_voice_intent_parse_gap("3600"), 3600u);
}

void run_voice_intent_tests(void) {
    HU_TEST_SUITE("voice intent (voice-first memos)");
    HU_RUN_TEST(test_voice_intent_needs_a_voice_and_the_feature);
    HU_RUN_TEST(test_voice_intent_answers_audio_with_audio_even_inside_the_gap);
    HU_RUN_TEST(test_voice_intent_keeps_logistics_as_text);
    HU_RUN_TEST(test_voice_intent_spaces_memos_to_one_person);
    HU_RUN_TEST(test_voice_intent_heartfelt_is_whole_words);
    HU_RUN_TEST(test_voice_intent_talks_through_real_questions_only);
    HU_RUN_TEST(test_voice_intent_memo_shape);
    HU_RUN_TEST(test_voice_intent_ignores_everyday_love_and_sorry);
    HU_RUN_TEST(test_voice_intent_logistics_is_a_short_question);
    HU_RUN_TEST(test_voice_intent_gap_parse_rejects_nonsense);
}
