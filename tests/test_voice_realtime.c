#include "human/channels/voice_realtime.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── F150-F152: Real-Time Voice Calls ───────────────────────────────────────── */

static void create_table_sql_valid(void)
{
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_voice_rt_create_table_sql(buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "CREATE TABLE") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "voice_sessions") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "session_id") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "contact_id") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "state") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "codec") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "started_at") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "ended_at") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "duration_ms") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "samples_processed") != NULL);
}

static void insert_sql_valid(void)
{
    hu_voice_config_t config = hu_voice_rt_default_config();
    hu_voice_session_t session = {0};
    hu_error_t err = hu_voice_rt_init_session(&config, "user_a", 6, &session);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_voice_rt_transition(&session, HU_VOICE_CALL_RINGING);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_voice_rt_transition(&session, HU_VOICE_CALL_CONNECTED);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_voice_rt_transition(&session, HU_VOICE_CALL_ENDED);
    HU_ASSERT_EQ(err, HU_OK);

    char buf[1024];
    size_t len = 0;
    err = hu_voice_rt_insert_sql(&session, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "INSERT INTO voice_sessions") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "user_a") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "ended") != NULL);
}

static void default_config_values(void)
{
    hu_voice_config_t c = hu_voice_rt_default_config();
    HU_ASSERT_EQ(c.sample_rate, 16000u);
    HU_ASSERT_EQ(c.channels, (uint8_t)1);
    HU_ASSERT_EQ(c.codec, HU_VOICE_CODEC_OPUS);
    HU_ASSERT_EQ(c.max_duration_ms, 30ULL * 60 * 1000);
    HU_ASSERT_EQ(c.silence_timeout_ms, 5000u);
    HU_ASSERT_TRUE(c.vad_enabled);
}

static void init_session_sets_idle(void)
{
    hu_voice_config_t config = hu_voice_rt_default_config();
    hu_voice_session_t session = {0};
    hu_error_t err = hu_voice_rt_init_session(&config, "user_a", 6, &session);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(session.state, HU_VOICE_CALL_IDLE);
    HU_ASSERT_TRUE(session.session_id[0] != '\0');
    HU_ASSERT_TRUE(strstr(session.session_id, "user_a") != NULL);
}

static void init_session_null_contact_returns_error(void)
{
    hu_voice_config_t config = hu_voice_rt_default_config();
    hu_voice_session_t session = {0};
    hu_error_t err = hu_voice_rt_init_session(&config, NULL, 6, &session);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    err = hu_voice_rt_init_session(&config, "user_a", 0, &session);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void transition_idle_to_ringing(void)
{
    hu_voice_config_t config = hu_voice_rt_default_config();
    hu_voice_session_t session = {0};
    hu_voice_rt_init_session(&config, "user_a", 6, &session);
    hu_error_t err = hu_voice_rt_transition(&session, HU_VOICE_CALL_RINGING);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(session.state, HU_VOICE_CALL_RINGING);
}

static void transition_ringing_to_connected(void)
{
    hu_voice_config_t config = hu_voice_rt_default_config();
    hu_voice_session_t session = {0};
    hu_voice_rt_init_session(&config, "user_a", 6, &session);
    hu_voice_rt_transition(&session, HU_VOICE_CALL_RINGING);
    hu_error_t err = hu_voice_rt_transition(&session, HU_VOICE_CALL_CONNECTED);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(session.state, HU_VOICE_CALL_CONNECTED);
    HU_ASSERT_TRUE(session.started_ms > 0);
}

static void transition_connected_to_on_hold(void)
{
    hu_voice_config_t config = hu_voice_rt_default_config();
    hu_voice_session_t session = {0};
    hu_voice_rt_init_session(&config, "user_a", 6, &session);
    hu_voice_rt_transition(&session, HU_VOICE_CALL_RINGING);
    hu_voice_rt_transition(&session, HU_VOICE_CALL_CONNECTED);
    hu_error_t err = hu_voice_rt_transition(&session, HU_VOICE_CALL_ON_HOLD);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(session.state, HU_VOICE_CALL_ON_HOLD);
}

static void transition_on_hold_to_connected(void)
{
    hu_voice_config_t config = hu_voice_rt_default_config();
    hu_voice_session_t session = {0};
    hu_voice_rt_init_session(&config, "user_a", 6, &session);
    hu_voice_rt_transition(&session, HU_VOICE_CALL_RINGING);
    hu_voice_rt_transition(&session, HU_VOICE_CALL_CONNECTED);
    hu_voice_rt_transition(&session, HU_VOICE_CALL_ON_HOLD);
    hu_error_t err = hu_voice_rt_transition(&session, HU_VOICE_CALL_CONNECTED);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(session.state, HU_VOICE_CALL_CONNECTED);
}

static void transition_connected_to_ended(void)
{
    hu_voice_config_t config = hu_voice_rt_default_config();
    hu_voice_session_t session = {0};
    hu_voice_rt_init_session(&config, "user_a", 6, &session);
    hu_voice_rt_transition(&session, HU_VOICE_CALL_RINGING);
    hu_voice_rt_transition(&session, HU_VOICE_CALL_CONNECTED);
    hu_error_t err = hu_voice_rt_transition(&session, HU_VOICE_CALL_ENDED);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(session.state, HU_VOICE_CALL_ENDED);
    HU_ASSERT_TRUE(session.duration_ms >= 0);
}

static void transition_invalid_returns_error(void)
{
    hu_voice_config_t config = hu_voice_rt_default_config();
    hu_voice_session_t session = {0};
    hu_voice_rt_init_session(&config, "user_a", 6, &session);
    /* IDLE -> CONNECTED is invalid (must go through RINGING) */
    hu_error_t err = hu_voice_rt_transition(&session, HU_VOICE_CALL_CONNECTED);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(session.state, HU_VOICE_CALL_IDLE);
}

static void is_valid_transition_checks(void)
{
    HU_ASSERT_TRUE(hu_voice_rt_is_valid_transition(HU_VOICE_CALL_IDLE, HU_VOICE_CALL_RINGING));
    HU_ASSERT_FALSE(hu_voice_rt_is_valid_transition(HU_VOICE_CALL_IDLE, HU_VOICE_CALL_CONNECTED));
    HU_ASSERT_TRUE(
        hu_voice_rt_is_valid_transition(HU_VOICE_CALL_RINGING, HU_VOICE_CALL_CONNECTED));
    HU_ASSERT_TRUE(
        hu_voice_rt_is_valid_transition(HU_VOICE_CALL_CONNECTED, HU_VOICE_CALL_ON_HOLD));
    HU_ASSERT_TRUE(
        hu_voice_rt_is_valid_transition(HU_VOICE_CALL_ON_HOLD, HU_VOICE_CALL_CONNECTED));
    HU_ASSERT_TRUE(
        hu_voice_rt_is_valid_transition(HU_VOICE_CALL_CONNECTED, HU_VOICE_CALL_ENDED));
    HU_ASSERT_FALSE(
        hu_voice_rt_is_valid_transition(HU_VOICE_CALL_ENDED, HU_VOICE_CALL_CONNECTED));
}

static void duration_ms_returns_zero_when_idle(void)
{
    hu_voice_config_t config = hu_voice_rt_default_config();
    hu_voice_session_t session = {0};
    hu_voice_rt_init_session(&config, "user_a", 6, &session);
    uint64_t dur = hu_voice_rt_duration_ms(&session);
    HU_ASSERT_EQ(dur, 0ULL);
}

static void build_prompt_active_call(void)
{
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_config_t config = hu_voice_rt_default_config();
    hu_voice_session_t session = {0};
    hu_voice_rt_init_session(&config, "user_a", 6, &session);
    hu_voice_rt_transition(&session, HU_VOICE_CALL_RINGING);
    hu_voice_rt_transition(&session, HU_VOICE_CALL_CONNECTED);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_voice_rt_build_prompt(&alloc, &session, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(out, "voice call") != NULL);
    HU_ASSERT_TRUE(strstr(out, "user_a") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Duration") != NULL);
    alloc.free(alloc.ctx, out, 512);
}

static void build_prompt_idle_session(void)
{
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_config_t config = hu_voice_rt_default_config();
    hu_voice_session_t session = {0};
    hu_voice_rt_init_session(&config, "user_a", 6, &session);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_voice_rt_build_prompt(&alloc, &session, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "No active voice call") != NULL);
    alloc.free(alloc.ctx, out, 512);
}

static void call_state_str_returns_labels(void)
{
    HU_ASSERT_STR_EQ(hu_voice_call_state_str(HU_VOICE_CALL_IDLE), "idle");
    HU_ASSERT_STR_EQ(hu_voice_call_state_str(HU_VOICE_CALL_RINGING), "ringing");
    HU_ASSERT_STR_EQ(hu_voice_call_state_str(HU_VOICE_CALL_CONNECTED), "connected");
    HU_ASSERT_STR_EQ(hu_voice_call_state_str(HU_VOICE_CALL_ON_HOLD), "on_hold");
    HU_ASSERT_STR_EQ(hu_voice_call_state_str(HU_VOICE_CALL_ENDED), "ended");
    HU_ASSERT_STR_EQ(hu_voice_call_state_str(HU_VOICE_CALL_FAILED), "failed");
}

static void codec_str_returns_labels(void)
{
    HU_ASSERT_STR_EQ(hu_voice_codec_str(HU_VOICE_CODEC_OPUS), "opus");
    HU_ASSERT_STR_EQ(hu_voice_codec_str(HU_VOICE_CODEC_PCM16), "pcm16");
    HU_ASSERT_STR_EQ(hu_voice_codec_str(HU_VOICE_CODEC_AAC), "aac");
}

static void session_init_copies_contact_id(void)
{
    hu_voice_config_t config = hu_voice_rt_default_config();
    hu_voice_session_t session = {0};
    const char *contact = "user_a";
    hu_error_t err = hu_voice_rt_init_session(&config, contact, 6, &session);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(session.contact_id_len, 6u);
    HU_ASSERT_TRUE(memcmp(session.contact_id, "user_a", 6) == 0);
    HU_ASSERT_EQ(session.contact_id[6], '\0');
}

void run_voice_realtime_tests(void)
{
    HU_TEST_SUITE("voice_realtime");
    HU_RUN_TEST(create_table_sql_valid);
    HU_RUN_TEST(insert_sql_valid);
    HU_RUN_TEST(default_config_values);
    HU_RUN_TEST(init_session_sets_idle);
    HU_RUN_TEST(init_session_null_contact_returns_error);
    HU_RUN_TEST(transition_idle_to_ringing);
    HU_RUN_TEST(transition_ringing_to_connected);
    HU_RUN_TEST(transition_connected_to_on_hold);
    HU_RUN_TEST(transition_on_hold_to_connected);
    HU_RUN_TEST(transition_connected_to_ended);
    HU_RUN_TEST(transition_invalid_returns_error);
    HU_RUN_TEST(is_valid_transition_checks);
    HU_RUN_TEST(duration_ms_returns_zero_when_idle);
    HU_RUN_TEST(build_prompt_active_call);
    HU_RUN_TEST(build_prompt_idle_session);
    HU_RUN_TEST(call_state_str_returns_labels);
    HU_RUN_TEST(codec_str_returns_labels);
    HU_RUN_TEST(session_init_copies_contact_id);
}
