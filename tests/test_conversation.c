#include "seaclaw/context/conversation.h"
#include "seaclaw/core/allocator.h"
#include "test_framework.h"
#include <string.h>

/* ── Helper to build history entries ─────────────────────────────────── */

static sc_channel_history_entry_t make_entry(bool from_me, const char *text, const char *ts) {
    sc_channel_history_entry_t e;
    memset(&e, 0, sizeof(e));
    e.from_me = from_me;
    size_t tl = strlen(text);
    if (tl >= sizeof(e.text))
        tl = sizeof(e.text) - 1;
    memcpy(e.text, text, tl);
    e.text[tl] = '\0';
    size_t tsl = strlen(ts);
    if (tsl >= sizeof(e.timestamp))
        tsl = sizeof(e.timestamp) - 1;
    memcpy(e.timestamp, ts, tsl);
    e.timestamp[tsl] = '\0';
    return e;
}

/* ── Multi-message splitting tests ───────────────────────────────────── */

static void split_short_response_stays_single(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_message_fragment_t frags[4];
    size_t n = sc_conversation_split_response(&alloc, "yeah for sure", 13, frags, 4);
    SC_ASSERT_EQ(n, 1u);
    SC_ASSERT_STR_EQ(frags[0].text, "yeah for sure");
    SC_ASSERT_EQ(frags[0].delay_ms, 0u);
    alloc.free(alloc.ctx, frags[0].text, frags[0].text_len + 1);
}

static void split_null_input_returns_zero(void) {
    sc_message_fragment_t frags[4];
    size_t n = sc_conversation_split_response(NULL, "hello", 5, frags, 4);
    SC_ASSERT_EQ(n, 0u);
}

static void split_empty_returns_zero(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_message_fragment_t frags[4];
    size_t n = sc_conversation_split_response(&alloc, "", 0, frags, 4);
    SC_ASSERT_EQ(n, 0u);
}

static void split_on_newlines(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_message_fragment_t frags[4];
    const char *resp = "hey how are you\n\nso i was thinking about that thing";
    size_t n = sc_conversation_split_response(&alloc, resp, strlen(resp), frags, 4);
    SC_ASSERT_TRUE(n >= 2);
    SC_ASSERT_TRUE(frags[0].text_len > 0);
    SC_ASSERT_TRUE(frags[1].text_len > 0);
    for (size_t i = 0; i < n; i++)
        alloc.free(alloc.ctx, frags[i].text, frags[i].text_len + 1);
}

static void split_on_conjunction_starter(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_message_fragment_t frags[4];
    const char *resp =
        "that sounds really fun honestly. but i think we should check the weather first";
    size_t n = sc_conversation_split_response(&alloc, resp, strlen(resp), frags, 4);
    SC_ASSERT_TRUE(n >= 2);
    /* First fragment should end with the sentence before "but" */
    SC_ASSERT_TRUE(strstr(frags[0].text, "honestly") != NULL);
    /* Second fragment should start with "but" */
    SC_ASSERT_TRUE(frags[1].text[0] == 'b' || frags[1].text[0] == 'B');
    for (size_t i = 0; i < n; i++)
        alloc.free(alloc.ctx, frags[i].text, frags[i].text_len + 1);
}

static void split_respects_max_fragments(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_message_fragment_t frags[2];
    const char *resp = "first thing.\nsecond thing.\nthird thing.\nfourth thing.";
    size_t n = sc_conversation_split_response(&alloc, resp, strlen(resp), frags, 2);
    SC_ASSERT_TRUE(n <= 2);
    for (size_t i = 0; i < n; i++)
        alloc.free(alloc.ctx, frags[i].text, frags[i].text_len + 1);
}

static void split_inter_message_delay_nonzero_for_later_fragments(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_message_fragment_t frags[4];
    const char *resp = "omg that's wild.\noh wait also did you hear about the thing at work";
    size_t n = sc_conversation_split_response(&alloc, resp, strlen(resp), frags, 4);
    if (n >= 2) {
        SC_ASSERT_EQ(frags[0].delay_ms, 0u);
        SC_ASSERT_TRUE(frags[1].delay_ms > 0);
    }
    for (size_t i = 0; i < n; i++)
        alloc.free(alloc.ctx, frags[i].text, frags[i].text_len + 1);
}

/* ── Style analysis tests ────────────────────────────────────────────── */

static void style_null_returns_null(void) {
    size_t len = 0;
    char *s = sc_conversation_analyze_style(NULL, NULL, 0, &len);
    SC_ASSERT_NULL(s);
}

static void style_too_few_messages_returns_null(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_history_entry_t entries[2] = {
        make_entry(false, "hey", "12:00"),
        make_entry(true, "hi", "12:01"),
    };
    size_t len = 0;
    char *s = sc_conversation_analyze_style(&alloc, entries, 2, &len);
    SC_ASSERT_NULL(s);
}

static void style_detects_all_lowercase(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_history_entry_t entries[6] = {
        make_entry(false, "hey whats up", "12:00"),
        make_entry(true, "nm", "12:01"),
        make_entry(false, "lol same", "12:02"),
        make_entry(false, "did you see that thing", "12:03"),
        make_entry(true, "yeah", "12:04"),
        make_entry(false, "so wild right", "12:05"),
    };
    size_t len = 0;
    char *s = sc_conversation_analyze_style(&alloc, entries, 6, &len);
    SC_ASSERT_NOT_NULL(s);
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_TRUE(strstr(s, "lowercase") != NULL || strstr(s, "capitalize") != NULL);
    alloc.free(alloc.ctx, s, len + 1);
}

static void style_detects_no_periods(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_history_entry_t entries[6] = {
        make_entry(false, "hey whats good", "12:00"),
        make_entry(true, "nm u", "12:01"),
        make_entry(false, "not much just chillin", "12:02"),
        make_entry(false, "thinking about getting food", "12:03"),
        make_entry(true, "same", "12:04"),
        make_entry(false, "wanna come", "12:05"),
    };
    size_t len = 0;
    char *s = sc_conversation_analyze_style(&alloc, entries, 6, &len);
    SC_ASSERT_NOT_NULL(s);
    SC_ASSERT_TRUE(strstr(s, "period") != NULL);
    alloc.free(alloc.ctx, s, len + 1);
}

static void style_includes_anti_patterns(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_history_entry_t entries[6] = {
        make_entry(false, "hey", "12:00"),
        make_entry(true, "hey", "12:01"),
        make_entry(false, "what are you doing", "12:02"),
        make_entry(false, "i'm bored lol", "12:03"),
        make_entry(true, "same", "12:04"),
        make_entry(false, "wanna hang", "12:05"),
    };
    size_t len = 0;
    char *s = sc_conversation_analyze_style(&alloc, entries, 6, &len);
    SC_ASSERT_NOT_NULL(s);
    SC_ASSERT_TRUE(strstr(s, "Anti-pattern") != NULL || strstr(s, "NEVER") != NULL);
    alloc.free(alloc.ctx, s, len + 1);
}

/* ── Response classification tests ───────────────────────────────────── */

static void classify_empty_skips(void) {
    uint32_t delay = 0;
    sc_response_action_t a = sc_conversation_classify_response("", 0, NULL, 0, &delay);
    SC_ASSERT_EQ(a, SC_RESPONSE_SKIP);
}

static void classify_tapback_skips(void) {
    uint32_t delay = 0;
    sc_response_action_t a =
        sc_conversation_classify_response("Loved an image", 14, NULL, 0, &delay);
    SC_ASSERT_EQ(a, SC_RESPONSE_SKIP);
}

static void classify_lol_is_brief(void) {
    uint32_t delay = 0;
    sc_response_action_t a = sc_conversation_classify_response("lol", 3, NULL, 0, &delay);
    SC_ASSERT_EQ(a, SC_RESPONSE_BRIEF);
}

static void classify_haha_is_brief(void) {
    uint32_t delay = 0;
    sc_response_action_t a = sc_conversation_classify_response("haha", 4, NULL, 0, &delay);
    SC_ASSERT_EQ(a, SC_RESPONSE_BRIEF);
}

static void classify_nice_is_brief(void) {
    uint32_t delay = 0;
    sc_response_action_t a = sc_conversation_classify_response("nice", 4, NULL, 0, &delay);
    SC_ASSERT_EQ(a, SC_RESPONSE_BRIEF);
}

static void classify_question_is_full(void) {
    uint32_t delay = 0;
    sc_response_action_t a =
        sc_conversation_classify_response("what are you up to tonight?", 27, NULL, 0, &delay);
    SC_ASSERT_EQ(a, SC_RESPONSE_FULL);
    SC_ASSERT_TRUE(delay > 0);
}

static void classify_emotional_is_delayed(void) {
    uint32_t delay = 0;
    sc_response_action_t a =
        sc_conversation_classify_response("i've been really stressed lately", 31, NULL, 0, &delay);
    SC_ASSERT_EQ(a, SC_RESPONSE_DELAY);
    SC_ASSERT_TRUE(delay >= 5000);
}

static void classify_ok_after_question_skips(void) {
    sc_channel_history_entry_t entries[2] = {
        make_entry(true, "want to grab dinner?", "12:00"),
        make_entry(false, "ok", "12:01"),
    };
    uint32_t delay = 0;
    sc_response_action_t a = sc_conversation_classify_response("ok", 2, entries, 2, &delay);
    SC_ASSERT_EQ(a, SC_RESPONSE_SKIP);
}

static void classify_ok_after_distant_question_skips(void) {
    sc_channel_history_entry_t entries[4] = {
        make_entry(true, "want to grab dinner?", "12:00"),
        make_entry(false, "maybe", "12:01"),
        make_entry(true, "cool let me know", "12:02"),
        make_entry(false, "ok", "12:03"),
    };
    uint32_t delay = 0;
    sc_response_action_t a = sc_conversation_classify_response("ok", 2, entries, 4, &delay);
    SC_ASSERT_EQ(a, SC_RESPONSE_SKIP);
}

static void classify_normal_statement_is_full(void) {
    uint32_t delay = 0;
    sc_response_action_t a = sc_conversation_classify_response(
        "i just got back from the store and got us some stuff", 52, NULL, 0, &delay);
    SC_ASSERT_EQ(a, SC_RESPONSE_FULL);
}

/* ── Quality evaluator enhanced tests ────────────────────────────────── */

static void quality_penalizes_semicolons(void) {
    sc_quality_score_t score =
        sc_conversation_evaluate_quality("that sounds good; I'll check it out", 35, NULL, 0, 300);
    SC_ASSERT_TRUE(score.naturalness < 20);
}

static void quality_penalizes_exclamation_overuse(void) {
    sc_quality_score_t score = sc_conversation_evaluate_quality(
        "Wow! This is amazing! So cool! Love it!", 39, NULL, 0, 300);
    SC_ASSERT_TRUE(score.naturalness <= 20);
}

static void quality_rewards_contractions(void) {
    sc_quality_score_t score =
        sc_conversation_evaluate_quality("i don't think that's a great idea tbh", 37, NULL, 0, 300);
    SC_ASSERT_TRUE(score.naturalness >= 20);
}

static void quality_penalizes_service_language(void) {
    sc_quality_score_t score = sc_conversation_evaluate_quality(
        "Certainly! I'd be happy to help you with that.", 47, NULL, 0, 300);
    SC_ASSERT_TRUE(score.warmth < 5);
}

static void quality_good_casual_scores_high(void) {
    sc_quality_score_t score =
        sc_conversation_evaluate_quality("yeah that's wild lol", 20, NULL, 0, 300);
    SC_ASSERT_TRUE(score.total >= 60);
}

/* ── Awareness builder tests ─────────────────────────────────────────── */

static void awareness_null_returns_null(void) {
    size_t len = 0;
    char *ctx = sc_conversation_build_awareness(NULL, NULL, 0, &len);
    SC_ASSERT_NULL(ctx);
    SC_ASSERT_EQ(len, 0u);
}

static void awareness_builds_context(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_history_entry_t entries[4] = {
        make_entry(false, "hey!", "12:00"),
        make_entry(true, "hi whats up", "12:01"),
        make_entry(false, "not much, excited about the trip!", "12:02"),
        make_entry(true, "me too!", "12:03"),
    };
    size_t len = 0;
    char *ctx = sc_conversation_build_awareness(&alloc, entries, 4, &len);
    SC_ASSERT_NOT_NULL(ctx);
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_TRUE(strstr(ctx, "thread") != NULL || strstr(ctx, "Thread") != NULL ||
                   strstr(ctx, "conversation") != NULL);
    alloc.free(alloc.ctx, ctx, len + 1);
}

static void awareness_detects_excitement(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_history_entry_t entries[2] = {
        make_entry(false, "omg I got the job!!!", "12:00"),
        make_entry(true, "wait what", "12:01"),
    };
    size_t len = 0;
    char *ctx = sc_conversation_build_awareness(&alloc, entries, 2, &len);
    SC_ASSERT_NOT_NULL(ctx);
    SC_ASSERT_TRUE(strstr(ctx, "excited") != NULL);
    alloc.free(alloc.ctx, ctx, len + 1);
}

/* ── Narrative detection tests ───────────────────────────────────────── */

static void narrative_opening_with_few_exchanges(void) {
    sc_channel_history_entry_t entries[2] = {
        make_entry(false, "hey", "12:00"),
        make_entry(true, "hi", "12:01"),
    };
    sc_narrative_phase_t phase = sc_conversation_detect_narrative(entries, 2);
    SC_ASSERT_EQ(phase, SC_NARRATIVE_OPENING);
}

static void narrative_closing_detected(void) {
    sc_channel_history_entry_t entries[3] = {
        make_entry(false, "ok sounds good", "12:00"),
        make_entry(true, "cool", "12:01"),
        make_entry(false, "ttyl", "12:02"),
    };
    sc_narrative_phase_t phase = sc_conversation_detect_narrative(entries, 3);
    SC_ASSERT_EQ(phase, SC_NARRATIVE_CLOSING);
}

/* ── Engagement detection tests ──────────────────────────────────────── */

static void engagement_high_with_questions(void) {
    sc_channel_history_entry_t entries[4] = {
        make_entry(false, "what did you end up doing about the car situation?", "12:00"),
        make_entry(true, "still figuring it out", "12:01"),
        make_entry(false, "did you check with that mechanic i told you about?", "12:02"),
        make_entry(true, "not yet", "12:03"),
    };
    sc_engagement_level_t eng = sc_conversation_detect_engagement(entries, 4);
    SC_ASSERT_EQ(eng, SC_ENGAGEMENT_HIGH);
}

static void engagement_distracted_with_single_words(void) {
    sc_channel_history_entry_t entries[4] = {
        make_entry(true, "did you see the game?", "12:00"),
        make_entry(false, "ya", "12:01"),
        make_entry(true, "it was wild right?", "12:02"),
        make_entry(false, "mhm", "12:03"),
    };
    sc_engagement_level_t eng = sc_conversation_detect_engagement(entries, 4);
    SC_ASSERT_TRUE(eng == SC_ENGAGEMENT_DISTRACTED || eng == SC_ENGAGEMENT_LOW);
}

/* ── Emotion detection tests ─────────────────────────────────────────── */

static void emotion_detects_positive(void) {
    sc_channel_history_entry_t entries[3] = {
        make_entry(false, "i'm so happy right now", "12:00"),
        make_entry(false, "everything is amazing", "12:01"),
        make_entry(false, "i love this", "12:02"),
    };
    sc_emotional_state_t emo = sc_conversation_detect_emotion(entries, 3);
    SC_ASSERT_TRUE(emo.valence > 0.0f);
}

static void emotion_detects_negative(void) {
    sc_channel_history_entry_t entries[3] = {
        make_entry(false, "i'm so stressed about everything", "12:00"),
        make_entry(false, "feeling really anxious and worried", "12:01"),
        make_entry(false, "i just feel sad", "12:02"),
    };
    sc_emotional_state_t emo = sc_conversation_detect_emotion(entries, 3);
    SC_ASSERT_TRUE(emo.valence < 0.0f);
    SC_ASSERT_TRUE(emo.intensity > 0.3f);
}

static void emotion_neutral_for_normal_chat(void) {
    sc_channel_history_entry_t entries[2] = {
        make_entry(false, "hey whats up", "12:00"),
        make_entry(false, "nothing much", "12:01"),
    };
    sc_emotional_state_t emo = sc_conversation_detect_emotion(entries, 2);
    SC_ASSERT_TRUE(emo.intensity < 0.3f);
}

/* ── Honesty guardrail tests ─────────────────────────────────────────── */

static void honesty_detects_action_query(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *result = sc_conversation_honesty_check(&alloc, "did you call mom?", 17);
    SC_ASSERT_NOT_NULL(result);
    SC_ASSERT_TRUE(strstr(result, "HONESTY") != NULL);
    alloc.free(alloc.ctx, result, 512);
}

static void honesty_null_for_normal_message(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *result = sc_conversation_honesty_check(&alloc, "what are you up to", 18);
    SC_ASSERT_NULL(result);
}

/* ── Test suite registration ─────────────────────────────────────────── */

void run_conversation_tests(void) {
    SC_TEST_SUITE("Conversation Intelligence");

    /* Multi-message splitting */
    SC_RUN_TEST(split_short_response_stays_single);
    SC_RUN_TEST(split_null_input_returns_zero);
    SC_RUN_TEST(split_empty_returns_zero);
    SC_RUN_TEST(split_on_newlines);
    SC_RUN_TEST(split_on_conjunction_starter);
    SC_RUN_TEST(split_respects_max_fragments);
    SC_RUN_TEST(split_inter_message_delay_nonzero_for_later_fragments);

    /* Style analysis */
    SC_RUN_TEST(style_null_returns_null);
    SC_RUN_TEST(style_too_few_messages_returns_null);
    SC_RUN_TEST(style_detects_all_lowercase);
    SC_RUN_TEST(style_detects_no_periods);
    SC_RUN_TEST(style_includes_anti_patterns);

    /* Response classification */
    SC_RUN_TEST(classify_empty_skips);
    SC_RUN_TEST(classify_tapback_skips);
    SC_RUN_TEST(classify_lol_is_brief);
    SC_RUN_TEST(classify_haha_is_brief);
    SC_RUN_TEST(classify_nice_is_brief);
    SC_RUN_TEST(classify_question_is_full);
    SC_RUN_TEST(classify_emotional_is_delayed);
    SC_RUN_TEST(classify_ok_after_question_skips);
    SC_RUN_TEST(classify_ok_after_distant_question_skips);
    SC_RUN_TEST(classify_normal_statement_is_full);

    /* Quality evaluator */
    SC_RUN_TEST(quality_penalizes_semicolons);
    SC_RUN_TEST(quality_penalizes_exclamation_overuse);
    SC_RUN_TEST(quality_rewards_contractions);
    SC_RUN_TEST(quality_penalizes_service_language);
    SC_RUN_TEST(quality_good_casual_scores_high);

    /* Awareness builder */
    SC_RUN_TEST(awareness_null_returns_null);
    SC_RUN_TEST(awareness_builds_context);
    SC_RUN_TEST(awareness_detects_excitement);

    /* Narrative detection */
    SC_RUN_TEST(narrative_opening_with_few_exchanges);
    SC_RUN_TEST(narrative_closing_detected);

    /* Engagement detection */
    SC_RUN_TEST(engagement_high_with_questions);
    SC_RUN_TEST(engagement_distracted_with_single_words);

    /* Emotion detection */
    SC_RUN_TEST(emotion_detects_positive);
    SC_RUN_TEST(emotion_detects_negative);
    SC_RUN_TEST(emotion_neutral_for_normal_chat);

    /* Honesty guardrail */
    SC_RUN_TEST(honesty_detects_action_query);
    SC_RUN_TEST(honesty_null_for_normal_message);
}
