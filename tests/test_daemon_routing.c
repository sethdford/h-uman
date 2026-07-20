#include "human/channel_loop.h"
#include "human/config.h"
#include "human/daemon.h"
#include "human/daemon_routing.h"
#include "test_framework.h"
#include <string.h>

/* ── hu_daemon_is_tapback_worthy ─────────────────────────────────────── */

static void test_tapback_null_returns_false(void) {
    HU_ASSERT_FALSE(hu_daemon_is_tapback_worthy(NULL, 0));
    HU_ASSERT_FALSE(hu_daemon_is_tapback_worthy("hi", 0));
}

static void test_tapback_question_not_worthy(void) {
    HU_ASSERT_FALSE(hu_daemon_is_tapback_worthy("how are you?", 12));
}

static void test_tapback_short_ack(void) {
    HU_ASSERT_TRUE(hu_daemon_is_tapback_worthy("ok", 2));
    HU_ASSERT_TRUE(hu_daemon_is_tapback_worthy("lol", 3));
    HU_ASSERT_TRUE(hu_daemon_is_tapback_worthy("k", 1));
}

static void test_tapback_single_token_under_12(void) {
    HU_ASSERT_TRUE(hu_daemon_is_tapback_worthy("hahahahaha", 10));
}

static void test_tapback_multi_word_short_not_worthy(void) {
    HU_ASSERT_FALSE(hu_daemon_is_tapback_worthy("oh nice one there", 17));
}

static void test_tapback_long_message_not_worthy(void) {
    HU_ASSERT_FALSE(hu_daemon_is_tapback_worthy("that sounds like a great plan", 29));
}

/* ── hu_daemon_compute_photo_delay ───────────────────────────────────── */

static void test_photo_delay_no_attachment(void) {
    hu_channel_loop_msg_t msgs[2];
    memset(msgs, 0, sizeof(msgs));
    uint32_t delay = hu_daemon_compute_photo_delay(msgs, 0, 1, 123);
    HU_ASSERT_EQ(delay, 0);
}

static void test_photo_delay_with_attachment(void) {
    hu_channel_loop_msg_t msgs[2];
    memset(msgs, 0, sizeof(msgs));
    msgs[1].has_attachment = true;
    uint32_t delay = hu_daemon_compute_photo_delay(msgs, 0, 1, 0);
    HU_ASSERT(delay >= 3000 && delay <= 8000);
}

/* ── hu_daemon_compute_video_delay ───────────────────────────────────── */

static void test_video_delay_no_video(void) {
    hu_channel_loop_msg_t msgs[2];
    memset(msgs, 0, sizeof(msgs));
    uint32_t delay = hu_daemon_compute_video_delay(msgs, 0, 1, 123);
    HU_ASSERT_EQ(delay, 0);
}

static void test_video_delay_with_video(void) {
    hu_channel_loop_msg_t msgs[2];
    memset(msgs, 0, sizeof(msgs));
    msgs[0].has_video = true;
    uint32_t delay = hu_daemon_compute_video_delay(msgs, 0, 1, 0);
    HU_ASSERT(delay >= 2000 && delay <= 10000);
}

/* ── hu_missed_message_acknowledgment ────────────────────────────────── */

static void test_missed_msg_short_delay_null(void) {
    const char *phrase = hu_missed_message_acknowledgment(60, 14, 14, 0);
    HU_ASSERT_NULL(phrase);
}

static void test_missed_msg_long_delay_returns_phrase(void) {
    const char *phrase = hu_missed_message_acknowledgment(7200, 14, 14, 0);
    HU_ASSERT_NOT_NULL(phrase);
}

static void test_missed_msg_overnight_woke_phrase(void) {
    const char *phrase = hu_missed_message_acknowledgment(7200, 2, 8, 0);
    HU_ASSERT_NOT_NULL(phrase);
    HU_ASSERT_NOT_NULL(strstr(phrase, "woke up"));
}

static void test_missed_msg_deterministic(void) {
    const char *a = hu_missed_message_acknowledgment(7200, 14, 14, 42);
    const char *b = hu_missed_message_acknowledgment(7200, 14, 14, 42);
    HU_ASSERT_STR_EQ(a, b);
}

/* ── hu_daemon_set_missed_msg_threshold ──────────────────────────────── */

static void test_set_threshold_changes_behavior(void) {
    /* Default 1800s: 1900s delay should return a phrase */
    const char *before = hu_missed_message_acknowledgment(1900, 12, 12, 0);
    HU_ASSERT_NOT_NULL(before);

    /* Raise to 3600s: 1900s should now be under threshold */
    hu_daemon_set_missed_msg_threshold(3600);
    const char *after = hu_missed_message_acknowledgment(1900, 12, 12, 0);
    HU_ASSERT_NULL(after);

    /* Restore default */
    hu_daemon_set_missed_msg_threshold(1800);
}

static void test_set_threshold_rejects_too_small(void) {
    hu_daemon_set_missed_msg_threshold(1800); /* reset */
    hu_daemon_set_missed_msg_threshold(30);   /* too small, rejected */
    /* 1900s still returns phrase with 1800s threshold */
    const char *phrase = hu_missed_message_acknowledgment(1900, 12, 12, 0);
    HU_ASSERT_NOT_NULL(phrase);
}

/* ── Integration: fully-populated loop_msg metadata ─────────────────── */

static void test_photo_delay_group_msg_with_attachment(void) {
    hu_channel_loop_msg_t msgs[2];
    memset(msgs, 0, sizeof(msgs));
    strncpy(msgs[0].session_key, "C0001ABCDEF", sizeof(msgs[0].session_key) - 1);
    strncpy(msgs[0].chat_id, "C0001ABCDEF", sizeof(msgs[0].chat_id) - 1);
    strncpy(msgs[0].content, "check this photo", sizeof(msgs[0].content) - 1);
    msgs[0].is_group = true;
    msgs[0].has_attachment = true;
    msgs[0].timestamp_sec = 1700000000;
    msgs[0].message_id = 42;
    uint32_t delay = hu_daemon_compute_photo_delay(msgs, 0, 1, 123);
    HU_ASSERT(delay >= 3000 && delay <= 8000);
}

static void test_video_delay_group_msg_with_video(void) {
    hu_channel_loop_msg_t msgs[2];
    memset(msgs, 0, sizeof(msgs));
    strncpy(msgs[0].session_key, "C0001ABCDEF", sizeof(msgs[0].session_key) - 1);
    strncpy(msgs[0].chat_id, "C0001ABCDEF", sizeof(msgs[0].chat_id) - 1);
    strncpy(msgs[0].content, "watch this clip", sizeof(msgs[0].content) - 1);
    msgs[0].is_group = true;
    msgs[0].has_video = true;
    msgs[0].timestamp_sec = 1700000000;
    uint32_t delay = hu_daemon_compute_video_delay(msgs, 0, 1, 0);
    HU_ASSERT(delay >= 2000 && delay <= 10000);
}

static void test_photo_delay_dm_no_attachment(void) {
    hu_channel_loop_msg_t msgs[2];
    memset(msgs, 0, sizeof(msgs));
    strncpy(msgs[0].session_key, "D0001", sizeof(msgs[0].session_key) - 1);
    strncpy(msgs[0].content, "just text", sizeof(msgs[0].content) - 1);
    msgs[0].is_group = false;
    msgs[0].has_attachment = false;
    msgs[0].timestamp_sec = 1700000000;
    uint32_t delay = hu_daemon_compute_photo_delay(msgs, 0, 1, 0);
    HU_ASSERT_EQ(delay, 0);
}

static void test_tapback_worthy_with_group_context(void) {
    HU_ASSERT_TRUE(hu_daemon_is_tapback_worthy("ok", 2));
    HU_ASSERT_TRUE(hu_daemon_is_tapback_worthy("lol", 3));
    HU_ASSERT_FALSE(hu_daemon_is_tapback_worthy("what do you think about this?", 28));
}

static void test_photo_delay_reply_msg_with_attachment(void) {
    hu_channel_loop_msg_t msgs[2];
    memset(msgs, 0, sizeof(msgs));
    strncpy(msgs[0].session_key, "C0001", sizeof(msgs[0].session_key) - 1);
    strncpy(msgs[0].chat_id, "C0001", sizeof(msgs[0].chat_id) - 1);
    strncpy(msgs[0].reply_to_guid, "1699999999.000001", sizeof(msgs[0].reply_to_guid) - 1);
    strncpy(msgs[0].content, "replying with image", sizeof(msgs[0].content) - 1);
    msgs[0].is_group = true;
    msgs[0].has_attachment = true;
    uint32_t delay = hu_daemon_compute_photo_delay(msgs, 0, 1, 42);
    HU_ASSERT(delay >= 3000 && delay <= 8000);
}

static void test_video_delay_batch_mixed_content(void) {
    hu_channel_loop_msg_t msgs[4];
    memset(msgs, 0, sizeof(msgs));
    strncpy(msgs[0].session_key, "user1", sizeof(msgs[0].session_key) - 1);
    strncpy(msgs[0].content, "text only", sizeof(msgs[0].content) - 1);
    msgs[0].timestamp_sec = 1700000000;
    strncpy(msgs[1].session_key, "user1", sizeof(msgs[1].session_key) - 1);
    strncpy(msgs[1].content, "and a video", sizeof(msgs[1].content) - 1);
    msgs[1].has_video = true;
    msgs[1].timestamp_sec = 1700000001;
    uint32_t delay = hu_daemon_compute_video_delay(msgs, 0, 2, 0);
    HU_ASSERT(delay >= 2000 && delay <= 10000);
}

/* ── hu_daemon_fallback_model ────────────────────────────────────────── */

static void test_fallback_model_null_config_returns_default(void) {
    size_t len = 0;
    const char *m = hu_daemon_fallback_model(NULL, &len);
    HU_ASSERT_STR_EQ(m, HU_DAEMON_FALLBACK_MODEL_DEFAULT);
    HU_ASSERT_EQ(len, strlen(HU_DAEMON_FALLBACK_MODEL_DEFAULT));
}

static void test_fallback_model_unset_config_returns_default(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    size_t len = 0;
    const char *m = hu_daemon_fallback_model(&cfg, &len);
    HU_ASSERT_STR_EQ(m, HU_DAEMON_FALLBACK_MODEL_DEFAULT);
    HU_ASSERT_EQ(len, strlen(HU_DAEMON_FALLBACK_MODEL_DEFAULT));
}

static void test_fallback_model_empty_config_value_returns_default(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.agent.mr_reflexive_model = (char *)"";
    size_t len = 0;
    const char *m = hu_daemon_fallback_model(&cfg, &len);
    HU_ASSERT_STR_EQ(m, HU_DAEMON_FALLBACK_MODEL_DEFAULT);
    HU_ASSERT_EQ(len, strlen(HU_DAEMON_FALLBACK_MODEL_DEFAULT));
}

static void test_fallback_model_configured_returns_config_value(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.agent.mr_reflexive_model = (char *)"reflexive-tier-model";
    size_t len = 0;
    const char *m = hu_daemon_fallback_model(&cfg, &len);
    HU_ASSERT_STR_EQ(m, "reflexive-tier-model");
    HU_ASSERT_EQ(len, strlen("reflexive-tier-model"));
}

static void test_fallback_model_null_len_out_is_safe(void) {
    const char *m = hu_daemon_fallback_model(NULL, NULL);
    HU_ASSERT_STR_EQ(m, HU_DAEMON_FALLBACK_MODEL_DEFAULT);
}

void run_daemon_routing_tests(void) {
    HU_TEST_SUITE("daemon_routing");

    /* tapback */
    HU_RUN_TEST(test_tapback_null_returns_false);
    HU_RUN_TEST(test_tapback_question_not_worthy);
    HU_RUN_TEST(test_tapback_short_ack);
    HU_RUN_TEST(test_tapback_single_token_under_12);
    HU_RUN_TEST(test_tapback_multi_word_short_not_worthy);
    HU_RUN_TEST(test_tapback_long_message_not_worthy);

    /* photo delay */
    HU_RUN_TEST(test_photo_delay_no_attachment);
    HU_RUN_TEST(test_photo_delay_with_attachment);

    /* video delay */
    HU_RUN_TEST(test_video_delay_no_video);
    HU_RUN_TEST(test_video_delay_with_video);

    /* missed message */
    HU_RUN_TEST(test_missed_msg_short_delay_null);
    HU_RUN_TEST(test_missed_msg_long_delay_returns_phrase);
    HU_RUN_TEST(test_missed_msg_overnight_woke_phrase);
    HU_RUN_TEST(test_missed_msg_deterministic);
    HU_RUN_TEST(test_set_threshold_changes_behavior);
    HU_RUN_TEST(test_set_threshold_rejects_too_small);

    /* integration: fully-populated metadata */
    HU_RUN_TEST(test_photo_delay_group_msg_with_attachment);
    HU_RUN_TEST(test_video_delay_group_msg_with_video);
    HU_RUN_TEST(test_photo_delay_dm_no_attachment);
    HU_RUN_TEST(test_tapback_worthy_with_group_context);
    HU_RUN_TEST(test_photo_delay_reply_msg_with_attachment);
    HU_RUN_TEST(test_video_delay_batch_mixed_content);

    /* fallback model */
    HU_RUN_TEST(test_fallback_model_null_config_returns_default);
    HU_RUN_TEST(test_fallback_model_unset_config_returns_default);
    HU_RUN_TEST(test_fallback_model_empty_config_value_returns_default);
    HU_RUN_TEST(test_fallback_model_configured_returns_config_value);
    HU_RUN_TEST(test_fallback_model_null_len_out_is_safe);
}
