#include "human/channel.h"
#include "human/channels/imessage_action.h"
#include "human/channels/imessage_action_facts.h"
#include "human/config.h"
#include "human/core/time.h"
#include "human/daemon.h"
#include "human/persona.h"
#include "test_framework.h"
#include <stdint.h>
#include <string.h>

/* Test counters for mock vtable calls. */
static int reply_calls = 0;
static int send_calls = 0;
static int react_emoji_calls = 0;

/* Mock vtable functions. */
static hu_error_t mock_reply(void *ctx, const char *target, size_t target_len,
                             const char *parent_msg_guid, size_t parent_guid_len, const char *body,
                             size_t body_len) {
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)parent_msg_guid;
    (void)parent_guid_len;
    (void)body;
    (void)body_len;
    reply_calls++;
    return HU_OK;
}

static hu_error_t mock_reply_fails(void *ctx, const char *target, size_t target_len,
                                   const char *parent_msg_guid, size_t parent_guid_len,
                                   const char *body, size_t body_len) {
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)parent_msg_guid;
    (void)parent_guid_len;
    (void)body;
    (void)body_len;
    reply_calls++;
    return HU_ERR_NOT_SUPPORTED;
}

static hu_error_t mock_send(void *ctx, const char *target, size_t target_len, const char *message,
                            size_t message_len, const char *const *media, size_t media_count) {
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    (void)media;
    (void)media_count;
    send_calls++;
    return HU_OK;
}

static hu_error_t mock_react_emoji(void *ctx, const char *target, size_t target_len,
                                   int64_t message_id, const char *emoji_utf8,
                                   size_t emoji_utf8_len) {
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)message_id;
    (void)emoji_utf8;
    (void)emoji_utf8_len;
    react_emoji_calls++;
    return HU_OK;
}

static hu_error_t mock_send_fails(void *ctx, const char *target, size_t target_len,
                                  const char *message, size_t message_len, const char *const *media,
                                  size_t media_count) {
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    (void)media;
    (void)media_count;
    send_calls++;
    return HU_ERR_INTERNAL;
}

/* Mock vtable with all optional methods. */
static hu_channel_vtable_t mock_vtable = {0};
static hu_channel_t mock_ch = {0};

/* Mock config with action_surface_v2 enabled. */
static hu_config_t mock_config = {0};

/* Mock persona with fast pacing. */
static hu_persona_t mock_persona = {0};

static void setup_mocks(void) {
    reply_calls = 0;
    send_calls = 0;
    react_emoji_calls = 0;

    memset(&mock_vtable, 0, sizeof(mock_vtable));
    mock_vtable.reply = mock_reply;
    mock_vtable.send = mock_send;
    mock_vtable.react_emoji = mock_react_emoji;

    memset(&mock_ch, 0, sizeof(mock_ch));
    mock_ch.vtable = &mock_vtable;
    mock_ch.ctx = NULL;

    memset(&mock_config, 0, sizeof(mock_config));
    mock_config.channels.imessage.action_surface_v2.enabled = true;
    mock_config.channels.imessage.action_surface_v2.min_reply_delay_ms = 1;
    mock_config.channels.imessage.action_surface_v2.reply_delay_variance_ms = 0;

    memset(&mock_persona, 0, sizeof(mock_persona));
    mock_persona.min_reply_delay_ms = 1;
    mock_persona.reply_delay_variance_ms = 0;
}

/* AC: Invalid args (NULL ch, target, or body) short-circuit to INVALID_ARGUMENT. */
static void invalid_args_short_circuit(void) {
    setup_mocks();

    hu_conversation_snapshot_t snap = {0};
    hu_error_t err = hu_daemon_dispatch_imessage_reply(
        NULL, &mock_persona, NULL, &mock_config, "+15555551212", 12, "GUID", 4, "hi", 2,
        (const struct hu_conversation_snapshot *)&snap, 99);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(reply_calls + send_calls + react_emoji_calls, 0);

    err = hu_daemon_dispatch_imessage_reply(&mock_ch, &mock_persona, NULL, &mock_config, NULL, 12,
                                            "GUID", 4, "hi", 2,
                                            (const struct hu_conversation_snapshot *)&snap, 99);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_INVALID_ARGUMENT);

    err = hu_daemon_dispatch_imessage_reply(&mock_ch, &mock_persona, NULL, &mock_config,
                                            "+15555551212", 12, "GUID", 4, NULL, 2,
                                            (const struct hu_conversation_snapshot *)&snap, 99);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_INVALID_ARGUMENT);
}

/* AC: When action_surface_v2 is disabled, dispatcher falls back to flat send. */
static void disabled_feature_falls_back_to_flat(void) {
    setup_mocks();
    mock_config.channels.imessage.action_surface_v2.enabled = false;

    hu_conversation_snapshot_t snap = {0};
    hu_error_t err =
        hu_daemon_dispatch_imessage_reply(&mock_ch, &mock_persona, NULL, &mock_config,
                                          "+15555551212", 12, "GUID", 4, "hi", 2, &snap, 99);

    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(send_calls, 1); /* Exactly one flat send call */
    HU_ASSERT_EQ(reply_calls, 0);
    HU_ASSERT_EQ(react_emoji_calls, 0);
}

/* AC: When reply fails, dispatcher falls back to flat send (always-do-something).
 *
 * The dispatcher's RNG seed mixes inferred_message_id_for_react with
 * time(NULL), so varying message_id varies the predicate's pick. With
 * THREADED-favorable facts (p_thread ≈ 0.6), looping over 50 distinct
 * message_ids virtually guarantees we hit the THREADED branch at least
 * once. We loop until reply_calls > 0 (predicate picked THREADED + reply
 * was attempted), then verify the fallback chain landed correctly.
 *
 * If reply_calls stays 0 across all 50 attempts, the predicate weights
 * have drifted and the test fails — a real regression signal, not
 * flakiness. */
static void reply_failure_falls_back_to_flat(void) {
    setup_mocks();
    mock_vtable.reply = mock_reply_fails;

    hu_conversation_snapshot_t snap = {0};
    snap.parent_seconds_ago = 300;
    snap.parent_is_question = true;
    snap.other_threaded_replies_recent = 4;
    snap.conv_density_msgs_per_min = 1.0f;

    bool threaded_hit = false;
    for (int64_t mid = 1; mid <= 50 && !threaded_hit; mid++) {
        setup_mocks();
        mock_vtable.reply = mock_reply_fails;
        hu_error_t err = hu_daemon_dispatch_imessage_reply(
            &mock_ch, &mock_persona, NULL, &mock_config, "+15555551212", 12, "PARENT-GUID", 11,
            "hi", 2, (const struct hu_conversation_snapshot *)&snap, mid);
        HU_ASSERT_EQ((int)err, (int)HU_OK);
        if (reply_calls > 0) {
            threaded_hit = true;
            HU_ASSERT_EQ(reply_calls, 1); /* Reply was attempted */
            HU_ASSERT_EQ(send_calls, 1);  /* Then fell back to flat send */
            HU_ASSERT_EQ(react_emoji_calls, 0);
        }
    }
    HU_ASSERT(threaded_hit);
}

/* AC: When vtable->reply is NULL, ANY style routes through send (either as
 * primary or as fallback). We null reply AND react_emoji so the only
 * possible action is send — regardless of which style the predicate picks. */
static void no_reply_vtable_falls_back_to_flat(void) {
    setup_mocks();
    mock_vtable.reply = NULL;
    mock_vtable.react_emoji = NULL; /* force any style to land on send */

    hu_conversation_snapshot_t snap = {0};
    snap.parent_seconds_ago = 1000;
    snap.parent_is_question = true;
    snap.other_threaded_replies_recent = 10;
    snap.conv_density_msgs_per_min = 1.0f;
    snap.parent_position_from_bottom = 15;

    hu_error_t err = hu_daemon_dispatch_imessage_reply(
        &mock_ch, &mock_persona, NULL, &mock_config, "+15555551212", 12, "PARENT-GUID", 11, "hi", 2,
        (const struct hu_conversation_snapshot *)&snap, 1);

    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(reply_calls, 0);
    HU_ASSERT(send_calls >= 1); /* Always — only path left is send-fallback */
}

/* AC: When vtable->react_emoji is NULL, ANY non-reply style routes through
 * send. Null both reply AND react_emoji so the only possible path is send. */
static void no_react_emoji_vtable_falls_back_to_flat(void) {
    setup_mocks();
    mock_vtable.reply = NULL;       /* force THREADED to also fall back to send */
    mock_vtable.react_emoji = NULL; /* force TAPBACK / TAPBACK_PLUS_FLAT to send */

    hu_conversation_snapshot_t snap = {0};
    snap.parent_seconds_ago = 10;
    snap.parent_is_question = false;
    snap.other_threaded_replies_recent = 0;
    snap.conv_density_msgs_per_min = 0.5f;
    snap.parent_emotional_intensity = HU_EMOTION_THRESHOLD_HIGH;

    hu_error_t err = hu_daemon_dispatch_imessage_reply(
        &mock_ch, &mock_persona, NULL, &mock_config, "+15555551212", 12, "GUID", 4, "nice", 4,
        (const struct hu_conversation_snapshot *)&snap, 2);

    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT(send_calls >= 1);
}

/* AC: Pacing actually enforces minimum elapsed >= min_delay_ms * 1.2. */
static void pacing_enforces_minimum_delay(void) {
    setup_mocks();
    mock_persona.min_reply_delay_ms = 30;
    mock_persona.reply_delay_variance_ms = 0;

    hu_conversation_snapshot_t snap = {0};
    uint64_t t0 = hu_time_get_current_ms();
    hu_error_t err = hu_daemon_dispatch_imessage_reply(
        &mock_ch, &mock_persona, NULL, &mock_config, "+15555551212", 12, NULL, 0, "hi", 2,
        (const struct hu_conversation_snapshot *)&snap, 99);
    uint64_t t1 = hu_time_get_current_ms();

    HU_ASSERT_EQ((int)err, (int)HU_OK);
    int64_t elapsed = (int64_t)(t1 - t0);
    HU_ASSERT(elapsed >= 36); /* 30 * 1.2 = 36 ms */
}

/* AC: When ALL paths fail, dispatcher returns the send error from fallback.
 * Null reply + react_emoji so any style is forced through send; send fails;
 * dispatcher MUST propagate that error (always-do-something contract — but
 * propagate the error so daemon's caller knows). */
static void all_paths_fail_returns_send_error(void) {
    setup_mocks();
    mock_vtable.reply = NULL;
    mock_vtable.react_emoji = NULL;
    mock_vtable.send = mock_send_fails;

    hu_conversation_snapshot_t snap = {0};
    snap.conv_density_msgs_per_min = 50.0f;
    snap.parent_seconds_ago = 5;

    hu_error_t err = hu_daemon_dispatch_imessage_reply(
        &mock_ch, &mock_persona, NULL, &mock_config, "+15555551212", 12, NULL, 0, "hi", 2,
        (const struct hu_conversation_snapshot *)&snap, 4);

    HU_ASSERT_EQ((int)err, (int)HU_ERR_INTERNAL);
    HU_ASSERT(send_calls >= 1);
}

/* AC: With send-only vtable, every style routes through send. Forces the
 * dispatcher's switch arms to all converge on send (either as primary FLAT
 * or as fallback after reply/react_emoji NULL guards). */
static void flat_style_routes_to_send(void) {
    setup_mocks();
    mock_vtable.reply = NULL;
    mock_vtable.react_emoji = NULL;

    hu_conversation_snapshot_t snap = {0};
    snap.conv_density_msgs_per_min = 20.0f;
    snap.other_threaded_replies_recent = 0;
    snap.parent_seconds_ago = 5;

    hu_error_t err = hu_daemon_dispatch_imessage_reply(
        &mock_ch, &mock_persona, NULL, &mock_config, "+15555551212", 12, NULL, 0, "hi", 2,
        (const struct hu_conversation_snapshot *)&snap, 6);

    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT(send_calls >= 1);
}

void run_imessage_dispatcher_tests(void) {
    HU_TEST_SUITE("imessage_dispatcher");
    HU_RUN_TEST(invalid_args_short_circuit);
    HU_RUN_TEST(disabled_feature_falls_back_to_flat);
    HU_RUN_TEST(reply_failure_falls_back_to_flat);
    HU_RUN_TEST(no_reply_vtable_falls_back_to_flat);
    HU_RUN_TEST(no_react_emoji_vtable_falls_back_to_flat);
    HU_RUN_TEST(pacing_enforces_minimum_delay);
    HU_RUN_TEST(all_paths_fail_returns_send_error);
    HU_RUN_TEST(flat_style_routes_to_send);
}
