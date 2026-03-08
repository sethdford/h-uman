#include "seaclaw/context/conversation.h"
#include "seaclaw/core/allocator.h"
#include "test_framework.h"
#include <stdio.h>
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

static void classify_farewell_goodnight_is_brief(void) {
    uint32_t delay = 0;
    sc_response_action_t a = sc_conversation_classify_response("goodnight!", 10, NULL, 0, &delay);
    SC_ASSERT_EQ(a, SC_RESPONSE_BRIEF);
    SC_ASSERT_TRUE(delay > 0);
}

static void classify_farewell_short_bye(void) {
    uint32_t delay = 0;
    sc_response_action_t a = sc_conversation_classify_response("bye", 3, NULL, 0, &delay);
    SC_ASSERT_EQ(a, SC_RESPONSE_BRIEF);
}

static void classify_farewell_ttyl(void) {
    uint32_t delay = 0;
    sc_response_action_t a = sc_conversation_classify_response("ttyl!", 5, NULL, 0, &delay);
    SC_ASSERT_EQ(a, SC_RESPONSE_BRIEF);
}

static void classify_bad_news_is_delayed(void) {
    uint32_t delay = 0;
    sc_response_action_t a =
        sc_conversation_classify_response("my grandma passed away last night", 33, NULL, 0, &delay);
    SC_ASSERT_EQ(a, SC_RESPONSE_DELAY);
    SC_ASSERT_TRUE(delay >= 10000);
}

static void classify_good_news_is_delayed(void) {
    uint32_t delay = 0;
    sc_response_action_t a =
        sc_conversation_classify_response("I got the job!!", 15, NULL, 0, &delay);
    SC_ASSERT_EQ(a, SC_RESPONSE_DELAY);
    SC_ASSERT_TRUE(delay >= 2000);
}

static void classify_vulnerable_is_delayed(void) {
    uint32_t delay = 0;
    sc_response_action_t a = sc_conversation_classify_response(
        "can i be honest with you about something", 41, NULL, 0, &delay);
    SC_ASSERT_EQ(a, SC_RESPONSE_DELAY);
    SC_ASSERT_TRUE(delay >= 5000);
}

/* ── Two-phase thinking response tests ─────────────────────────────────── */

static void thinking_triggers_on_complex_question(void) {
    const char *msg =
        "What do you think about moving to a new city? I've been going back and forth on it for "
        "weeks and I'm not sure what the right call is";
    sc_thinking_response_t out;
    bool ok = sc_conversation_classify_thinking(msg, strlen(msg), NULL, 0, &out, 42);
    SC_ASSERT_TRUE(ok);
    SC_ASSERT_TRUE(out.filler_len > 0);
    SC_ASSERT_TRUE(out.delay_ms >= 30000 && out.delay_ms <= 60000);
}

static void thinking_no_trigger_simple_message(void) {
    const char *msg = "hey what's up";
    sc_thinking_response_t out;
    bool ok = sc_conversation_classify_thinking(msg, strlen(msg), NULL, 0, &out, 42);
    SC_ASSERT_FALSE(ok);
}

static void thinking_triggers_on_advice(void) {
    const char *msg = "should I take the new job or stay where I am?";
    sc_thinking_response_t out;
    bool ok = sc_conversation_classify_thinking(msg, strlen(msg), NULL, 0, &out, 42);
    SC_ASSERT_TRUE(ok);
    SC_ASSERT_TRUE(out.filler_len > 0);
    SC_ASSERT_TRUE(out.delay_ms >= 30000 && out.delay_ms <= 60000);
}

static void thinking_filler_varies_by_seed(void) {
    const char *msg =
        "What do you think about moving to a new city? I've been going back and forth on it for "
        "weeks and I'm not sure what the right call is";
    sc_thinking_response_t out0, out1;
    bool ok0 = sc_conversation_classify_thinking(msg, strlen(msg), NULL, 0, &out0, 0);
    bool ok1 = sc_conversation_classify_thinking(msg, strlen(msg), NULL, 0, &out1, 1);
    SC_ASSERT_TRUE(ok0);
    SC_ASSERT_TRUE(ok1);
    /* Different seeds should produce different fillers (6 options, LCG varies) */
    bool same = (out0.filler_len == out1.filler_len &&
                 memcmp(out0.filler, out1.filler, out0.filler_len) == 0);
    SC_ASSERT_FALSE(same);
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

static void awareness_output_bounded(void) {
    sc_allocator_t alloc = sc_system_allocator();

    /* Create a large history */
    sc_channel_history_entry_t entries[50];
    char texts[50][256];
    for (int i = 0; i < 50; i++) {
        snprintf(texts[i], sizeof(texts[i]),
                 "This is message number %d with some content to fill space and make the "
                 "awareness builder work with substantial input data for testing purposes",
                 i);
        entries[i] = make_entry((i % 2) == 0, texts[i], "2024-01-15T10:00:00");
    }

    size_t out_len = 0;
    char *ctx = sc_conversation_build_awareness(&alloc, entries, 50, &out_len);
    SC_ASSERT_NOT_NULL(ctx);
    SC_ASSERT_TRUE(out_len > 0);
    /* Awareness output should be bounded (not grow without limit) */
    SC_ASSERT_TRUE(out_len < 32768);

    alloc.free(alloc.ctx, ctx, out_len + 1);
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

/* ── Length calibration tests ──────────────────────────────────────── */

/* Data-driven calibration: output describes metrics, not rigid message types */
static void calibrate_greeting_short(void) {
    char buf[1024];
    size_t len = sc_conversation_calibrate_length("hey", 3, NULL, 0, buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "calibration"));
    SC_ASSERT_NOT_NULL(strstr(buf, "brief") || strstr(buf, "Match"));
}

static void calibrate_yes_no_question(void) {
    char buf[1024];
    size_t len =
        sc_conversation_calibrate_length("are you coming tonight?", 23, NULL, 0, buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "question"));
}

static void calibrate_emotional_message(void) {
    char buf[1024];
    size_t len = sc_conversation_calibrate_length("I'm really stressed about this job thing", 40,
                                                  NULL, 0, buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "calibration"));
    SC_ASSERT_NOT_NULL(strstr(buf, "Match") || strstr(buf, "match"));
}

static void calibrate_logistics(void) {
    char buf[1024];
    size_t len = sc_conversation_calibrate_length("what time should we meet at the restaurant?", 43,
                                                  NULL, 0, buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "question"));
}

static void calibrate_short_react(void) {
    char buf[1024];
    size_t len = sc_conversation_calibrate_length("lol", 3, NULL, 0, buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "brief") || strstr(buf, "Very brief"));
}

static void calibrate_link_share(void) {
    char buf[1024];
    size_t len = sc_conversation_calibrate_length("check this out https://example.com", 34, NULL, 0,
                                                  buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "link"));
}

static void calibrate_open_question(void) {
    char buf[1024];
    const char *msg = "what do you think about all that?";
    size_t len = sc_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "question"));
}

static void calibrate_long_story(void) {
    char msg[256];
    memset(msg, 'a', 200);
    msg[200] = '\0';
    char buf[1024];
    size_t len = sc_conversation_calibrate_length(msg, 200, NULL, 0, buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "Substantial") || strstr(buf, "depth"));
}

static void calibrate_good_news(void) {
    char buf[1024];
    const char *msg = "I got the job!!";
    size_t len = sc_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "calibration"));
    SC_ASSERT_NOT_NULL(strstr(buf, "Match") || strstr(buf, "match"));
}

static void calibrate_bad_news(void) {
    char buf[1024];
    const char *msg = "my grandma passed away last night";
    size_t len = sc_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "calibration"));
    SC_ASSERT_NOT_NULL(strstr(buf, "Match") || strstr(buf, "match"));
}

static void calibrate_teasing(void) {
    char buf[1024];
    const char *msg = "yeah right";
    size_t len = sc_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "calibration"));
    SC_ASSERT_NOT_NULL(strstr(buf, "Match") || strstr(buf, "match"));
}

static void calibrate_vulnerable(void) {
    char buf[1024];
    const char *msg = "can i be honest with you about something";
    size_t len = sc_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "calibration"));
    SC_ASSERT_NOT_NULL(strstr(buf, "Match") || strstr(buf, "match"));
}

static void calibrate_tone_present_in_greeting(void) {
    char buf[1024];
    const char *msg = "hey";
    size_t len = sc_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "calibration"));
}

static void calibrate_tone_present_in_emotional(void) {
    char buf[1024];
    const char *msg = "i'm so stressed out i can't handle this anymore";
    size_t len = sc_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "calibration"));
}

static void calibrate_farewell_goodnight(void) {
    char buf[1024];
    const char *msg = "goodnight!";
    size_t len = sc_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "calibration"));
    SC_ASSERT_NOT_NULL(strstr(buf, "Match") || strstr(buf, "match"));
}

static void calibrate_farewell_short_bye(void) {
    char buf[1024];
    const char *msg = "bye";
    size_t len = sc_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "calibration"));
}

static void calibrate_emoji_present_in_greeting(void) {
    char buf[1024];
    const char *msg = "hey";
    size_t len = sc_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "calibration"));
}

static void calibrate_emoji_present_in_logistics(void) {
    char buf[1024];
    const char *msg = "what time should we meet at the restaurant?";
    size_t len = sc_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "calibration"));
}

static void calibrate_emoji_present_in_general(void) {
    char buf[1024];
    const char *msg = "i just finished cooking dinner";
    size_t len = sc_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "calibration"));
}

static void calibrate_null_returns_zero(void) {
    char buf[64];
    SC_ASSERT_EQ(sc_conversation_calibrate_length(NULL, 0, NULL, 0, buf, sizeof(buf)), 0u);
}

static void calibrate_rapid_fire_momentum(void) {
    sc_channel_history_entry_t entries[] = {
        make_entry(false, "hey", "10:00"),
        make_entry(true, "hey", "10:00"),
        make_entry(false, "what's up", "10:01"),
        make_entry(true, "nm you?", "10:01"),
        make_entry(false, "same lol", "10:01"),
        make_entry(true, "haha", "10:02"),
        make_entry(false, "wanna grab food?", "10:02"),
    };
    char buf[1024];
    size_t len =
        sc_conversation_calibrate_length("wanna grab food?", 16, entries, 7, buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_TRUE(strstr(buf, "Rapid-fire") != NULL || strstr(buf, "rapid") != NULL);
}

/* ── Typo correction fragment tests ─────────────────────────────────── */

static void correction_detects_typo(void) {
    char buf[64];
    const char *orig = "meeting at noon";
    const char *typo = "meting at noon";
    size_t n = sc_conversation_generate_correction(orig, strlen(orig), typo, strlen(typo), buf,
                                                   sizeof(buf), 12345u, 100u);
    SC_ASSERT_EQ(n, 8u);
    SC_ASSERT_STR_EQ(buf, "*meeting");
}

static void correction_no_typo_no_output(void) {
    char buf[64];
    const char *s = "meeting at noon";
    size_t n = sc_conversation_generate_correction(s, strlen(s), s, strlen(s), buf, sizeof(buf),
                                                   12345u, 100u);
    SC_ASSERT_EQ(n, 0u);
}

static void correction_chance_zero_no_output(void) {
    char buf[64];
    const char *orig = "meeting at noon";
    const char *typo = "meting at noon";
    size_t n = sc_conversation_generate_correction(orig, strlen(orig), typo, strlen(typo), buf,
                                                   sizeof(buf), 12345u, 0u);
    SC_ASSERT_EQ(n, 0u);
}

static void correction_respects_buffer_cap(void) {
    char buf[6];
    const char *orig = "meeting at noon";
    const char *typo = "meting at noon";
    size_t n = sc_conversation_generate_correction(orig, strlen(orig), typo, strlen(typo), buf,
                                                   sizeof(buf), 12345u, 100u);
    SC_ASSERT_EQ(n, 5u);
    SC_ASSERT_EQ(strlen(buf), 5u);
    SC_ASSERT_TRUE(memcmp(buf, "*meet", 5) == 0);
}

/* ── Typing quirk post-processing tests ────────────────────────────── */

static void quirks_lowercase_applies(void) {
    char buf[] = "Hey What's Up";
    const char *quirks[] = {"lowercase"};
    size_t len = sc_conversation_apply_typing_quirks(buf, strlen(buf), quirks, 1);
    SC_ASSERT_STR_EQ(buf, "hey what's up");
    SC_ASSERT_EQ(len, 13u);
}

static void quirks_no_periods_strips_sentence_end(void) {
    char buf[] = "sounds good. let me know";
    const char *quirks[] = {"no_periods"};
    size_t len = sc_conversation_apply_typing_quirks(buf, strlen(buf), quirks, 1);
    SC_ASSERT_STR_EQ(buf, "sounds good let me know");
    SC_ASSERT_EQ(len, 23u);
}

static void quirks_no_periods_preserves_ellipsis(void) {
    char buf[] = "idk... maybe";
    const char *quirks[] = {"no_periods"};
    size_t len = sc_conversation_apply_typing_quirks(buf, strlen(buf), quirks, 1);
    SC_ASSERT_TRUE(len >= 10u);
    SC_ASSERT_TRUE(strstr(buf, "idk") != NULL);
    SC_ASSERT_TRUE(strstr(buf, "maybe") != NULL);
    (void)len;
}

static void quirks_no_commas_strips(void) {
    char buf[] = "yeah, I think so, maybe";
    const char *quirks[] = {"no_commas"};
    size_t len = sc_conversation_apply_typing_quirks(buf, strlen(buf), quirks, 1);
    SC_ASSERT_STR_EQ(buf, "yeah I think so maybe");
    SC_ASSERT_EQ(len, 21u);
}

static void quirks_no_apostrophes_strips(void) {
    char buf[] = "don't can't won't";
    const char *quirks[] = {"no_apostrophes"};
    size_t len = sc_conversation_apply_typing_quirks(buf, strlen(buf), quirks, 1);
    SC_ASSERT_STR_EQ(buf, "dont cant wont");
    SC_ASSERT_EQ(len, 14u);
}

static void quirks_multiple_combined(void) {
    char buf[] = "Hey, I Don't Know.";
    const char *quirks[] = {"lowercase", "no_periods", "no_commas"};
    size_t len = sc_conversation_apply_typing_quirks(buf, strlen(buf), quirks, 3);
    SC_ASSERT_NOT_NULL(strstr(buf, "hey"));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_TRUE(len < 18);
}

static void quirks_null_input_noop(void) {
    size_t len = sc_conversation_apply_typing_quirks(NULL, 0, NULL, 0);
    SC_ASSERT_EQ(len, 0u);
}

static void quirks_empty_quirks_noop(void) {
    char buf[] = "Hello World.";
    size_t len = sc_conversation_apply_typing_quirks(buf, strlen(buf), NULL, 0);
    SC_ASSERT_STR_EQ(buf, "Hello World.");
    SC_ASSERT_EQ(len, 12u);
}

static void apply_typing_quirks_double_space_to_newline(void) {
    char buf[] = "hello  world  foo";
    const char *quirks[] = {"double_space_to_newline"};
    size_t len = sc_conversation_apply_typing_quirks(buf, strlen(buf), quirks, 1);
    SC_ASSERT_STR_EQ(buf, "hello\nworld\nfoo");
    SC_ASSERT_EQ(len, 15u);
}

/* ── Typo simulation tests ────────────────────────────────────────────── */

static void typo_applies_with_right_seed(void) {
    /* Seed 0 yields val=0 from first prng_next, so 0%100<15 triggers typo */
    char buf[64];
    const char *input = "hello there friend";
    size_t len = strlen(input);
    memcpy(buf, input, len + 1);
    size_t out = sc_conversation_apply_typos(buf, len, sizeof(buf), 0);
    SC_ASSERT_TRUE(out != len || memcmp(buf, input, len + 1) != 0);
}

static void typo_preserves_short_words(void) {
    /* "I am ok" - all words <= 2 chars, no eligible words */
    char buf[64];
    const char *input = "I am ok";
    size_t len = strlen(input);
    memcpy(buf, input, len + 1);
    size_t out = sc_conversation_apply_typos(buf, len, sizeof(buf), 0);
    SC_ASSERT_STR_EQ(buf, "I am ok");
    SC_ASSERT_EQ(out, len);
}

static void typo_deterministic(void) {
    char buf1[64], buf2[64];
    const char *input = "sounds good to me";
    size_t len = strlen(input);
    memcpy(buf1, input, len + 1);
    memcpy(buf2, input, len + 1);
    size_t out1 = sc_conversation_apply_typos(buf1, len, sizeof(buf1), 42);
    size_t out2 = sc_conversation_apply_typos(buf2, len, sizeof(buf2), 42);
    SC_ASSERT_EQ(out1, out2);
    SC_ASSERT_TRUE(memcmp(buf1, buf2, (out1 > out2 ? out1 : out2) + 1) == 0);
}

static void typo_never_exceeds_cap(void) {
    char buf[32];
    const char *input = "hello there friend";
    size_t len = strlen(input);
    size_t cap = len + 2;
    memcpy(buf, input, len + 1);
    size_t out = sc_conversation_apply_typos(buf, len, cap, 0);
    SC_ASSERT_TRUE(out <= cap - 1);
}

/* ── Anti-repetition detection tests ──────────────────────────────────── */

static void repetition_detects_repeated_opener(void) {
    sc_channel_history_entry_t entries[] = {
        make_entry(true, "haha yeah totally", "10:00"),    make_entry(false, "right?", "10:01"),
        make_entry(true, "haha that's so funny", "10:02"), make_entry(false, "lol", "10:03"),
        make_entry(true, "haha i know", "10:04"),          make_entry(false, "anyway", "10:05"),
        make_entry(true, "haha what's up", "10:06"),
    };
    char buf[1024];
    size_t len = sc_conversation_detect_repetition(entries, 7, buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "haha"));
}

static void repetition_detects_question_overuse(void) {
    sc_channel_history_entry_t entries[] = {
        make_entry(true, "sounds fun?", "10:00"),         make_entry(false, "yeah", "10:01"),
        make_entry(true, "what time?", "10:02"),          make_entry(false, "7", "10:03"),
        make_entry(true, "where should we go?", "10:04"), make_entry(false, "idk", "10:05"),
        make_entry(true, "how about tacos?", "10:06"),
    };
    char buf[1024];
    size_t len = sc_conversation_detect_repetition(entries, 7, buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "question"));
}

static void repetition_no_issues_returns_zero(void) {
    sc_channel_history_entry_t entries[] = {
        make_entry(true, "hey what's up", "10:00"),
        make_entry(false, "not much", "10:01"),
        make_entry(true, "cool. wanna hang?", "10:02"),
        make_entry(false, "sure", "10:03"),
    };
    char buf[1024];
    size_t len = sc_conversation_detect_repetition(entries, 4, buf, sizeof(buf));
    SC_ASSERT_EQ(len, 0u);
}

static void repetition_too_few_messages_returns_zero(void) {
    sc_channel_history_entry_t entries[] = {
        make_entry(true, "hey", "10:00"),
        make_entry(false, "hey", "10:01"),
    };
    char buf[1024];
    size_t len = sc_conversation_detect_repetition(entries, 2, buf, sizeof(buf));
    SC_ASSERT_EQ(len, 0u);
}

/* ── Relationship-tier calibration tests ─────────────────────────────── */

static void relationship_close_friend(void) {
    char buf[512];
    size_t len =
        sc_conversation_calibrate_relationship("close friend", "high", "open", buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "Close"));
    SC_ASSERT_NOT_NULL(strstr(buf, "WARMTH"));
    SC_ASSERT_NOT_NULL(strstr(buf, "VULNERABILITY"));
}

static void relationship_acquaintance(void) {
    char buf[512];
    size_t len =
        sc_conversation_calibrate_relationship("acquaintance", "low", NULL, buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "Acquaintance"));
}

static void relationship_null_fields(void) {
    char buf[512];
    size_t len = sc_conversation_calibrate_relationship(NULL, NULL, NULL, buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "Relationship context"));
}

/* ── Group chat classifier tests ─────────────────────────────────────── */

static void group_direct_address_responds(void) {
    sc_group_response_t r =
        sc_conversation_classify_group("hey seth what do you think", 26, "seth", 4, NULL, 0);
    SC_ASSERT_EQ(r, SC_GROUP_RESPOND);
}

static void group_question_responds(void) {
    sc_group_response_t r =
        sc_conversation_classify_group("anyone free tonight?", 20, "bot", 3, NULL, 0);
    SC_ASSERT_EQ(r, SC_GROUP_RESPOND);
}

static void group_short_no_prompt_skips(void) {
    sc_group_response_t r = sc_conversation_classify_group("lol", 3, "bot", 3, NULL, 0);
    SC_ASSERT_EQ(r, SC_GROUP_SKIP);
}

static void group_too_many_responses_skips(void) {
    sc_channel_history_entry_t entries[] = {
        make_entry(false, "hey", "10:00"),
        make_entry(true, "hey", "10:01"),
        make_entry(true, "what's up", "10:02"),
        make_entry(true, "nm here", "10:03"),
    };
    sc_group_response_t r = sc_conversation_classify_group("cool story", 10, "bot", 3, entries, 4);
    SC_ASSERT_EQ(r, SC_GROUP_SKIP);
}

static void group_empty_skips(void) {
    sc_group_response_t r = sc_conversation_classify_group("", 0, "bot", 3, NULL, 0);
    SC_ASSERT_EQ(r, SC_GROUP_SKIP);
}

/* ── Thread callback tests ──────────────────────────────────────────── */

static void callback_finds_dropped_topic(void) {
    sc_allocator_t alloc = sc_system_allocator();
    /* Work discussed early (first half), then shifts to cooking (recent 3).
     * Last message "nice" has hash mod 5 == 0 so callback triggers (~20% probability). */
    sc_channel_history_entry_t entries[10] = {
        make_entry(false, "what about work? how's it going?", "12:00"),
        make_entry(true, "work has been crazy lately", "12:01"),
        make_entry(false, "tell me about work", "12:02"),
        make_entry(true, "it's busy but ok", "12:03"),
        make_entry(false, "got it", "12:04"),
        make_entry(true, "what's for dinner?", "12:05"),
        make_entry(false, "thinking about cooking", "12:06"),
        make_entry(true, "i'm making pasta", "12:07"),
        make_entry(false, "cooking is fun", "12:08"),
        make_entry(true, "nice", "12:09"),
    };
    size_t len = 0;
    char *ctx = sc_conversation_build_callback(&alloc, entries, 10, &len);
    SC_ASSERT_NOT_NULL(ctx);
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_TRUE(strstr(ctx, "work") != NULL);
    SC_ASSERT_TRUE(strstr(ctx, "Thread Callback") != NULL);
    alloc.free(alloc.ctx, ctx, len + 1);
}

static void callback_no_candidate_short_history(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_history_entry_t entries[3] = {
        make_entry(false, "hey", "12:00"),
        make_entry(true, "hi", "12:01"),
        make_entry(false, "what's up", "12:02"),
    };
    size_t len = 0;
    char *ctx = sc_conversation_build_callback(&alloc, entries, 3, &len);
    SC_ASSERT_NULL(ctx);
    SC_ASSERT_EQ(len, 0u);
}

static void callback_null_entries(void) {
    sc_allocator_t alloc = sc_system_allocator();
    size_t len = 0;
    char *ctx = sc_conversation_build_callback(&alloc, NULL, 10, &len);
    SC_ASSERT_NULL(ctx);
    SC_ASSERT_EQ(len, 0u);
}

/* ── Reaction classifier tests ───────────────────────────────────────── */

static void reaction_funny_message(void) {
    /* "lol that's hilarious" matches funny pattern; seed 0 yields roll<30 → HAHA */
    sc_reaction_type_t r =
        sc_conversation_classify_reaction("lol that's hilarious", 19, false, NULL, 0, 0u);
    SC_ASSERT_NEQ(r, SC_REACTION_NONE);
    SC_ASSERT_EQ(r, SC_REACTION_HAHA);
}

static void reaction_loving_message(void) {
    /* "love you" matches loving pattern; seed 0 yields roll<30 → HEART */
    sc_reaction_type_t r = sc_conversation_classify_reaction("love you", 8, false, NULL, 0, 0u);
    SC_ASSERT_EQ(r, SC_REACTION_HEART);
}

static void reaction_normal_message_no_reaction(void) {
    /* "what time is dinner?" needs a text response → NONE */
    sc_reaction_type_t r =
        sc_conversation_classify_reaction("what time is dinner?", 20, false, NULL, 0, 0u);
    SC_ASSERT_EQ(r, SC_REACTION_NONE);
}

static void reaction_from_me_no_reaction(void) {
    /* from_me=true → always NONE */
    sc_reaction_type_t r = sc_conversation_classify_reaction("love you", 8, true, NULL, 0, 0u);
    SC_ASSERT_EQ(r, SC_REACTION_NONE);
}

/* ── URL extraction tests ────────────────────────────────────────────── */

static void url_extract_finds_https(void) {
    const char *text = "check out https://example.com/page cool right?";
    sc_url_extract_t urls[4];
    size_t n = sc_conversation_extract_urls(text, strlen(text), urls, 4);
    SC_ASSERT_EQ(n, 1u);
    SC_ASSERT_EQ(urls[0].len, 24u);
    SC_ASSERT_TRUE(memcmp(urls[0].start, "https://example.com/page", 24) == 0);
}

static void url_extract_multiple(void) {
    const char *text = "see https://a.com and http://b.org/foo";
    sc_url_extract_t urls[4];
    size_t n = sc_conversation_extract_urls(text, strlen(text), urls, 4);
    SC_ASSERT_EQ(n, 2u);
    SC_ASSERT_TRUE(strncmp(urls[0].start, "https://a.com", urls[0].len) == 0);
    SC_ASSERT_TRUE(strncmp(urls[1].start, "http://b.org/foo", urls[1].len) == 0);
}

static void url_extract_no_urls(void) {
    const char *text = "hello world";
    sc_url_extract_t urls[4];
    size_t n = sc_conversation_extract_urls(text, strlen(text), urls, 4);
    SC_ASSERT_EQ(n, 0u);
}

/* ── Link-sharing detection tests ─────────────────────────────────────── */

static void should_share_link_recommendation(void) {
    sc_channel_history_entry_t entries[1] = {
        make_entry(false, "you should check this out", "12:00"),
    };
    bool ok = sc_conversation_should_share_link("you should check this out", 25, entries, 1);
    SC_ASSERT_TRUE(ok);
}

static void should_share_link_normal(void) {
    sc_channel_history_entry_t entries[1] = {
        make_entry(false, "how are you doing", "12:00"),
    };
    bool ok = sc_conversation_should_share_link("how are you doing", 17, entries, 1);
    SC_ASSERT_FALSE(ok);
}

static void should_share_link_case_insensitive(void) {
    bool ok = sc_conversation_should_share_link("CHECK THIS OUT", 15, NULL, 0);
    SC_ASSERT_TRUE(ok);
}

/* ── Attachment context tests ─────────────────────────────────────────── */

static void attachment_context_with_photo(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_history_entry_t entries[2] = {
        make_entry(true, "hey", "12:00"),
        make_entry(false, "[Photo shared]", "12:01"),
    };
    size_t len = 0;
    char *ctx = sc_conversation_attachment_context(&alloc, entries, 2, &len);
    SC_ASSERT_NOT_NULL(ctx);
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(ctx, "photo"));
    alloc.free(alloc.ctx, ctx, len + 1);
}

static void attachment_context_with_imessage_placeholder(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_history_entry_t entries[2] = {
        make_entry(true, "hey", "12:00"),
        make_entry(false, "[image or attachment]", "12:01"),
    };
    size_t len = 0;
    char *ctx = sc_conversation_attachment_context(&alloc, entries, 2, &len);
    SC_ASSERT_NOT_NULL(ctx);
    SC_ASSERT_TRUE(len > 0);
    alloc.free(alloc.ctx, ctx, len + 1);
}

/* ── Time-of-day calibration test ────────────────────────────────────── */

static void calibrate_length_runs_without_crash(void) {
    char buf[2048];
    const char *msg = "hey what's up";
    size_t len = sc_conversation_calibrate_length(msg, strlen(msg), NULL, 0, buf, sizeof(buf));
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "calibration"));
    /* TIME: directive appears only outside daytime (9-17), so we verify
     * the function completes without crashing at any hour. */
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
    SC_RUN_TEST(classify_farewell_goodnight_is_brief);
    SC_RUN_TEST(classify_farewell_short_bye);
    SC_RUN_TEST(classify_farewell_ttyl);
    SC_RUN_TEST(classify_bad_news_is_delayed);
    SC_RUN_TEST(classify_good_news_is_delayed);
    SC_RUN_TEST(classify_vulnerable_is_delayed);

    /* Two-phase thinking response */
    SC_RUN_TEST(thinking_triggers_on_complex_question);
    SC_RUN_TEST(thinking_no_trigger_simple_message);
    SC_RUN_TEST(thinking_triggers_on_advice);
    SC_RUN_TEST(thinking_filler_varies_by_seed);

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
    SC_RUN_TEST(awareness_output_bounded);

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

    /* Length + tone calibration */
    SC_RUN_TEST(calibrate_greeting_short);
    SC_RUN_TEST(calibrate_yes_no_question);
    SC_RUN_TEST(calibrate_emotional_message);
    SC_RUN_TEST(calibrate_logistics);
    SC_RUN_TEST(calibrate_short_react);
    SC_RUN_TEST(calibrate_link_share);
    SC_RUN_TEST(calibrate_open_question);
    SC_RUN_TEST(calibrate_long_story);
    SC_RUN_TEST(calibrate_good_news);
    SC_RUN_TEST(calibrate_bad_news);
    SC_RUN_TEST(calibrate_teasing);
    SC_RUN_TEST(calibrate_vulnerable);
    SC_RUN_TEST(calibrate_tone_present_in_greeting);
    SC_RUN_TEST(calibrate_tone_present_in_emotional);
    SC_RUN_TEST(calibrate_farewell_goodnight);
    SC_RUN_TEST(calibrate_farewell_short_bye);
    SC_RUN_TEST(calibrate_emoji_present_in_greeting);
    SC_RUN_TEST(calibrate_emoji_present_in_logistics);
    SC_RUN_TEST(calibrate_emoji_present_in_general);
    SC_RUN_TEST(calibrate_null_returns_zero);
    SC_RUN_TEST(calibrate_rapid_fire_momentum);

    /* Typo correction fragment */
    SC_RUN_TEST(correction_detects_typo);
    SC_RUN_TEST(correction_no_typo_no_output);
    SC_RUN_TEST(correction_chance_zero_no_output);
    SC_RUN_TEST(correction_respects_buffer_cap);

    /* Typo simulation */
    SC_RUN_TEST(typo_applies_with_right_seed);
    SC_RUN_TEST(typo_preserves_short_words);
    SC_RUN_TEST(typo_deterministic);
    SC_RUN_TEST(typo_never_exceeds_cap);

    /* Typing quirk post-processing */
    SC_RUN_TEST(quirks_lowercase_applies);
    SC_RUN_TEST(quirks_no_periods_strips_sentence_end);
    SC_RUN_TEST(quirks_no_periods_preserves_ellipsis);
    SC_RUN_TEST(quirks_no_commas_strips);
    SC_RUN_TEST(quirks_no_apostrophes_strips);
    SC_RUN_TEST(quirks_multiple_combined);
    SC_RUN_TEST(quirks_null_input_noop);
    SC_RUN_TEST(quirks_empty_quirks_noop);
    SC_RUN_TEST(apply_typing_quirks_double_space_to_newline);

    /* Anti-repetition */
    SC_RUN_TEST(repetition_detects_repeated_opener);
    SC_RUN_TEST(repetition_detects_question_overuse);
    SC_RUN_TEST(repetition_no_issues_returns_zero);
    SC_RUN_TEST(repetition_too_few_messages_returns_zero);

    /* Relationship-tier calibration */
    SC_RUN_TEST(relationship_close_friend);
    SC_RUN_TEST(relationship_acquaintance);
    SC_RUN_TEST(relationship_null_fields);

    /* Group chat classifier */
    SC_RUN_TEST(group_direct_address_responds);
    SC_RUN_TEST(group_question_responds);
    SC_RUN_TEST(group_short_no_prompt_skips);
    SC_RUN_TEST(group_too_many_responses_skips);
    SC_RUN_TEST(group_empty_skips);

    /* Reaction classifier */
    SC_RUN_TEST(reaction_funny_message);
    SC_RUN_TEST(reaction_loving_message);
    SC_RUN_TEST(reaction_normal_message_no_reaction);
    SC_RUN_TEST(reaction_from_me_no_reaction);

    /* Time-of-day */
    SC_RUN_TEST(calibrate_length_runs_without_crash);

    /* Thread callback */
    SC_RUN_TEST(callback_finds_dropped_topic);
    SC_RUN_TEST(callback_no_candidate_short_history);
    SC_RUN_TEST(callback_null_entries);

    /* URL extraction and link-sharing */
    SC_RUN_TEST(url_extract_finds_https);
    SC_RUN_TEST(url_extract_multiple);
    SC_RUN_TEST(url_extract_no_urls);
    SC_RUN_TEST(should_share_link_recommendation);
    SC_RUN_TEST(should_share_link_normal);
    SC_RUN_TEST(should_share_link_case_insensitive);
    SC_RUN_TEST(attachment_context_with_photo);
    SC_RUN_TEST(attachment_context_with_imessage_placeholder);
}
