#include "human/channels/voice_integration.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "test_framework.h"
#include <string.h>

/* ── Voice (F147-F149) ────────────────────────────────────────────────────── */

static void voice_create_table_sql_returns_valid_sql(void) {
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_voice_create_table_sql(buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "CREATE TABLE") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "voice_analyses") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "contact_id") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "transcription") != NULL);
}

static void voice_insert_analysis_sql_produces_valid_insert(void) {
    hu_voice_analysis_t a = {0};
    a.transcription = (char *)"hello there";
    a.transcription_len = 11;
    a.detected_emotion = (char *)"happy";
    a.detected_emotion_len = 5;
    a.emotion_confidence = 0.8;
    a.speech_rate = 180.0;
    a.duration_seconds = 45;

    char buf[1024];
    size_t len = 0;
    hu_error_t err = hu_voice_insert_analysis_sql(&a, "contact_a", 9, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "INSERT INTO voice_analyses") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "hello there") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "happy") != NULL);
}

static void voice_insert_analysis_sql_escapes_quotes(void) {
    hu_voice_analysis_t a = {0};
    a.transcription = (char *)"it's fine";
    a.transcription_len = 9;
    a.detected_emotion = (char *)"neutral";
    a.detected_emotion_len = 7;
    a.emotion_confidence = 0.5;
    a.speech_rate = 120.0;
    a.duration_seconds = 10;

    char buf[1024];
    size_t len = 0;
    hu_error_t err = hu_voice_insert_analysis_sql(&a, "c1", 2, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "''") != NULL);
}

static void voice_detect_emotion_happy_returns_happy(void) {
    char emotion[32];
    double conf = 0.0;
    hu_error_t err = hu_voice_detect_emotion_text("I'm so happy today", 18, emotion,
                                                   sizeof(emotion), &conf);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(emotion, "happy");
    HU_ASSERT_TRUE(conf > 0.5);
}

static void voice_detect_emotion_stressed_returns_stressed(void) {
    char emotion[32];
    double conf = 0.0;
    hu_error_t err = hu_voice_detect_emotion_text("I'm worried and anxious", 22, emotion,
                                                   sizeof(emotion), &conf);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(emotion, "stressed");
}

static void voice_detect_emotion_sad_returns_sad(void) {
    char emotion[32];
    double conf = 0.0;
    hu_error_t err = hu_voice_detect_emotion_text("I miss you so much", 18, emotion,
                                                   sizeof(emotion), &conf);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(emotion, "sad");
}

static void voice_detect_emotion_angry_returns_angry(void) {
    char emotion[32];
    double conf = 0.0;
    hu_error_t err = hu_voice_detect_emotion_text("I'm so frustrated", 17, emotion,
                                                   sizeof(emotion), &conf);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(emotion, "angry");
}

static void voice_detect_emotion_calm_returns_calm(void) {
    char emotion[32];
    double conf = 0.0;
    hu_error_t err = hu_voice_detect_emotion_text("feeling calm and relaxed", 23, emotion,
                                                   sizeof(emotion), &conf);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(emotion, "calm");
}

static void voice_detect_emotion_neutral_returns_neutral(void) {
    char emotion[32];
    double conf = 0.0;
    hu_error_t err = hu_voice_detect_emotion_text("the meeting is at 3pm", 20, emotion,
                                                   sizeof(emotion), &conf);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(emotion, "neutral");
}

static void voice_build_prompt_includes_analysis(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_analysis_t a = {0};
    a.transcription = (char *)"I'm stressed about work";
    a.transcription_len = 23;
    a.detected_emotion = (char *)"stressed";
    a.detected_emotion_len = 8;
    a.emotion_confidence = 0.8;
    a.speech_rate = 180.0;
    a.contains_sighing = true;
    a.duration_seconds = 30;

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_voice_build_prompt(&alloc, &a, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(out, "VOICE MESSAGE ANALYSIS") != NULL);
    HU_ASSERT_TRUE(strstr(out, "stressed") != NULL);
    HU_ASSERT_TRUE(strstr(out, "0.8") != NULL);
    HU_ASSERT_TRUE(strstr(out, "180") != NULL);
    HU_ASSERT_TRUE(strstr(out, "sighing") != NULL);
    HU_ASSERT_TRUE(strstr(out, "I'm stressed about work") != NULL);
    hu_str_free(&alloc, out);
}

static void voice_analysis_deinit_frees_strings(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_analysis_t a = {0};
    a.transcription = hu_strndup(&alloc, "test", 4);
    a.transcription_len = 4;
    a.detected_emotion = hu_strndup(&alloc, "happy", 5);
    a.detected_emotion_len = 5;
    hu_voice_analysis_deinit(&alloc, &a);
    HU_ASSERT_NULL(a.transcription);
    HU_ASSERT_NULL(a.detected_emotion);
}

/* ── Call (F169-F173) ─────────────────────────────────────────────────────── */

static void call_create_table_sql_returns_valid_sql(void) {
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_call_create_table_sql(buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "call_events") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "type") != NULL);
}

static void call_insert_event_sql_produces_valid_insert(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_call_event_t e = {0};
    e.type = HU_CALL_MISSED;
    e.contact_id = hu_strndup(&alloc, "contact_b", 9);
    e.contact_id_len = 9;
    e.timestamp = 1700000000ULL;
    e.duration_seconds = 0;

    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_call_insert_event_sql(&e, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "INSERT INTO call_events") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "missed") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "contact_b") != NULL);
    hu_call_event_deinit(&alloc, &e);
}

static void call_query_recent_sql_produces_valid_select(void) {
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_call_query_recent_sql("c1", 2, 10, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "SELECT") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "call_events") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "LIMIT 10") != NULL);
}

static void call_should_auto_text_missed_with_config_true(void) {
    hu_call_event_t e = {0};
    e.type = HU_CALL_MISSED;
    hu_call_response_config_t config = {0};
    config.auto_text_on_missed = true;
    config.auto_text_on_declined = true;
    bool ok = hu_call_should_auto_text(&e, &config);
    HU_ASSERT_TRUE(ok);
}

static void call_should_auto_text_missed_with_config_false(void) {
    hu_call_event_t e = {0};
    e.type = HU_CALL_MISSED;
    hu_call_response_config_t config = {0};
    config.auto_text_on_missed = false;
    config.auto_text_on_declined = true;
    bool ok = hu_call_should_auto_text(&e, &config);
    HU_ASSERT_FALSE(ok);
}

static void call_should_auto_text_incoming_returns_false(void) {
    hu_call_event_t e = {0};
    e.type = HU_CALL_INCOMING;
    hu_call_response_config_t config = {0};
    config.auto_text_on_missed = true;
    config.auto_text_on_declined = true;
    bool ok = hu_call_should_auto_text(&e, &config);
    HU_ASSERT_FALSE(ok);
}

static void call_build_text_prompt_missed_returns_template(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_call_build_text_prompt(&alloc, HU_CALL_MISSED, "Alice", 5, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "sorry I missed your call") != NULL);
    hu_str_free(&alloc, out);
}

static void call_build_text_prompt_declined_returns_template(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_call_build_text_prompt(&alloc, HU_CALL_DECLINED, "Bob", 3, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "can't talk rn") != NULL);
    hu_str_free(&alloc, out);
}

static void call_event_type_str_returns_all_types(void) {
    HU_ASSERT_STR_EQ(hu_call_event_type_str(HU_CALL_INCOMING), "incoming");
    HU_ASSERT_STR_EQ(hu_call_event_type_str(HU_CALL_OUTGOING), "outgoing");
    HU_ASSERT_STR_EQ(hu_call_event_type_str(HU_CALL_MISSED), "missed");
    HU_ASSERT_STR_EQ(hu_call_event_type_str(HU_CALL_DECLINED), "declined");
    HU_ASSERT_STR_EQ(hu_call_event_type_str(HU_CALL_FACETIME_INCOMING), "facetime_incoming");
    HU_ASSERT_STR_EQ(hu_call_event_type_str(HU_CALL_FACETIME_MISSED), "facetime_missed");
}

void run_voice_integration_tests(void) {
    HU_TEST_SUITE("voice_integration");
    HU_RUN_TEST(voice_create_table_sql_returns_valid_sql);
    HU_RUN_TEST(voice_insert_analysis_sql_produces_valid_insert);
    HU_RUN_TEST(voice_insert_analysis_sql_escapes_quotes);
    HU_RUN_TEST(voice_detect_emotion_happy_returns_happy);
    HU_RUN_TEST(voice_detect_emotion_stressed_returns_stressed);
    HU_RUN_TEST(voice_detect_emotion_sad_returns_sad);
    HU_RUN_TEST(voice_detect_emotion_angry_returns_angry);
    HU_RUN_TEST(voice_detect_emotion_calm_returns_calm);
    HU_RUN_TEST(voice_detect_emotion_neutral_returns_neutral);
    HU_RUN_TEST(voice_build_prompt_includes_analysis);
    HU_RUN_TEST(voice_analysis_deinit_frees_strings);
    HU_RUN_TEST(call_create_table_sql_returns_valid_sql);
    HU_RUN_TEST(call_insert_event_sql_produces_valid_insert);
    HU_RUN_TEST(call_query_recent_sql_produces_valid_select);
    HU_RUN_TEST(call_should_auto_text_missed_with_config_true);
    HU_RUN_TEST(call_should_auto_text_missed_with_config_false);
    HU_RUN_TEST(call_should_auto_text_incoming_returns_false);
    HU_RUN_TEST(call_build_text_prompt_missed_returns_template);
    HU_RUN_TEST(call_build_text_prompt_declined_returns_template);
    HU_RUN_TEST(call_event_type_str_returns_all_types);
}
