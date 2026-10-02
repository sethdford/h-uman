/* Coverage anchor: these tests exercise the iMessage reply dispatcher and
 * cross-channel context formatters in src/daemon/daemon_message_router.c
 * (hu_daemon_dispatch_imessage_reply). Naming the source path here lets
 * check-untested.sh recognize the file as tested: its exported symbols use
 * the hu_daemon_dispatch and hu_daemon_cross_channel prefixes rather than
 * hu_daemon_message_router, so the basename heuristic alone would miss it. */
#include "human/agent.h"
#include "human/agent/style_governor.h"
#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/channels/imessage.h"
#include "human/channels/imessage_action.h"
#include "human/channels/imessage_action_facts.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/time.h"
#include "human/daemon.h"
#include "human/daemon/message_router.h"
#include "human/ml/dpo.h"
#include "human/persona.h"
#include "test_framework.h"
#include <stdint.h>
#include <string.h>
#include <time.h>
#if defined(HU_ENABLE_SQLITE)
#include <sqlite3.h>
#endif

/* Test counters for mock vtable calls. */
static int reply_calls = 0;
static int send_calls = 0;
static int react_emoji_calls = 0;

/* Capture the most recent body passed to reply()/send() so tests can assert on
 * the quoted-parent substitution. */
static char last_reply_body[512] = {0};
static char last_send_body[512] = {0};
static void capture_body(char *dst, const char *body, size_t body_len) {
    size_t n = body_len < sizeof(last_reply_body) - 1 ? body_len : sizeof(last_reply_body) - 1;
    memcpy(dst, body, n);
    dst[n] = '\0';
}

/* Mock vtable functions. */
static hu_error_t mock_reply(void *ctx, const char *target, size_t target_len,
                             const char *parent_msg_guid, size_t parent_guid_len, const char *body,
                             size_t body_len) {
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)parent_msg_guid;
    (void)parent_guid_len;
    capture_body(last_reply_body, body, body_len);
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
    (void)media;
    (void)media_count;
    capture_body(last_send_body, message, message_len);
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
    hu_error_t err = hu_daemon_dispatch_imessage_reply(
        &mock_ch, &mock_persona, NULL, &mock_config, "+15555551212", 12, "GUID", 4, "hi", 2,
        (const struct hu_conversation_snapshot *)&snap, 99);

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

/* AC (regression): An EMPTY/NULL parent_guid must suppress the threaded
 * reply attempt, even when vtable->reply is present AND the predicate picks
 * THREADED. This pins the production bug fixed in daemon.c: all four
 * dispatcher call sites hardcoded parent_msg_guid = NULL, so the dispatcher's
 * guard `threaded_attempted = (vtable->reply && parent_msg_guid &&
 * parent_guid_len > 0)` was always false → threaded AX path was structurally
 * dead. The fix plumbs the inbound msgs[batch_start].guid through; if a future
 * refactor reverts a call site to NULL (or passes an empty-string guid with
 * len 0), this test catches it.
 *
 * Same THREADED-favorable facts as reply_failure_falls_back_to_flat, which
 * hits the reply path within 50 iterations when the guid is non-empty. Here
 * the guid is empty, so reply must NEVER be attempted across all 50 — any
 * reply_call is a regression (the empty-guard stopped working). react_emoji is
 * nulled so every non-flat style collapses to send, keeping the always-do-
 * something assertion deterministic. */
static void empty_parent_guid_never_attempts_threaded_reply(void) {
    hu_conversation_snapshot_t snap = {0};
    snap.parent_seconds_ago = 300;
    snap.parent_is_question = true;
    snap.other_threaded_replies_recent = 4;
    snap.conv_density_msgs_per_min = 1.0f;

    for (int64_t mid = 1; mid <= 50; mid++) {
        setup_mocks();
        mock_vtable.react_emoji = NULL; /* force any non-flat style onto send */

        /* NULL guid, len 0 — the production-bug shape. */
        hu_error_t err = hu_daemon_dispatch_imessage_reply(
            &mock_ch, &mock_persona, NULL, &mock_config, "+15555551212", 12, NULL, 0, "hi", 2,
            (const struct hu_conversation_snapshot *)&snap, mid);
        HU_ASSERT_EQ((int)err, (int)HU_OK);
        HU_ASSERT_EQ(reply_calls, 0); /* threaded reply must never be attempted */
        HU_ASSERT(send_calls >= 1);   /* always-do-something: it still sends */
    }
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

/* ── text-sent reporting + delivered-reply recording (2026-09-12) ──────────
 * production_outcomes used to be written when the model returned, so a turn
 * that ended as a tapback, was parrot-dropped or generated twice still landed
 * as a "sent reply" with ungoverned text. The reply loop now records from
 * the send funnel, gated on the dispatcher's text-sent flag. */

static hu_error_t mock_send_refuses(void *ctx, const char *target, size_t target_len,
                                    const char *message, size_t message_len,
                                    const char *const *media, size_t media_count) {
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    (void)media;
    (void)media_count;
    send_calls++;
    return HU_ERR_NOT_SUPPORTED;
}

static void text_sent_true_on_flat_send_false_when_send_refuses(void) {
    setup_mocks();
    mock_vtable.reply = NULL;
    mock_vtable.react_emoji = NULL;
    hu_conversation_snapshot_t snap = {0};
    snap.conv_density_msgs_per_min = 20.0f;
    snap.parent_seconds_ago = 5;
    bool sent = true;
    hu_error_t err = hu_daemon_dispatch_imessage_reply_ex(
        &mock_ch, &mock_persona, NULL, &mock_config, "+15555551212", 12, NULL, 0, "hi", 2,
        (const struct hu_conversation_snapshot *)&snap, 6, &sent, HU_REACTION_NONE);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_TRUE(sent);
    HU_ASSERT(send_calls >= 1);

    setup_mocks();
    mock_vtable.reply = NULL;
    mock_vtable.react_emoji = NULL;
    mock_vtable.send = mock_send_refuses;
    sent = true;
    err = hu_daemon_dispatch_imessage_reply_ex(
        &mock_ch, &mock_persona, NULL, &mock_config, "+15555551212", 12, NULL, 0, "hi", 2,
        (const struct hu_conversation_snapshot *)&snap, 6, &sent, HU_REACTION_NONE);
    HU_ASSERT_NEQ((int)err, (int)HU_OK);
    HU_ASSERT_FALSE(sent);
}

static void text_sent_true_when_feature_disabled_falls_back_to_flat(void) {
    setup_mocks();
    mock_config.channels.imessage.action_surface_v2.enabled = false;
    hu_conversation_snapshot_t snap = {0};
    bool sent = false;
    hu_error_t err = hu_daemon_dispatch_imessage_reply_ex(
        &mock_ch, &mock_persona, NULL, &mock_config, "+15555551212", 12, "GUID", 4, "hi", 2,
        (const struct hu_conversation_snapshot *)&snap, 1, &sent, HU_REACTION_NONE);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_TRUE(sent);
    HU_ASSERT_EQ(send_calls, 1);
}

/* The reply text was decided upstream, so it always goes out. A reaction rides
 * along exactly when the director asked for one — across the predicate's seed
 * space, so no random draw can add or drop it (DEF-2, 2026-10-01; before, the
 * draw decided, and live 2026-09-29 07:54 a split reply lost its first bubble
 * to a bare tapback). */
static void director_reaction_rides_along_with_the_text(void) {
    for (int64_t mid = 1; mid <= 400; mid++) {
        hu_conversation_snapshot_t snap = {0};
        snap.conv_density_msgs_per_min = 20.0f;
        snap.parent_seconds_ago = 5;
        bool sent = false;
        setup_mocks();
        mock_vtable.reply = NULL; /* no threaded slot: text goes out flat */
        hu_error_t err = hu_daemon_dispatch_imessage_reply_ex(
            &mock_ch, &mock_persona, NULL, &mock_config, "+15555551212", 12, NULL, 0, "hi", 2,
            (const struct hu_conversation_snapshot *)&snap, mid, &sent, HU_REACTION_HAHA);
        HU_ASSERT_EQ((int)err, (int)HU_OK);
        HU_ASSERT_TRUE(sent);
        HU_ASSERT_EQ(send_calls, 1);
        HU_ASSERT_EQ(react_emoji_calls, 1);
    }
}

/* A second bubble answering the same inbound message never taps it again. */
static void second_bubble_never_reacts_to_the_same_message_again(void) {
    for (int64_t mid = 1000; mid <= 1400; mid++) {
        setup_mocks();
        mock_vtable.reply = NULL;
        hu_conversation_snapshot_t snap = {0};
        snap.conv_density_msgs_per_min = 20.0f;
        snap.parent_seconds_ago = 5;
        bool sent = false;
        (void)hu_daemon_dispatch_imessage_reply_ex(
            &mock_ch, &mock_persona, NULL, &mock_config, "+15555551212", 12, NULL, 0, "hi", 2,
            (const struct hu_conversation_snapshot *)&snap, mid, &sent, HU_REACTION_HEART);
        if (react_emoji_calls == 0)
            continue;
        for (int bubble = 0; bubble < 3; bubble++) {
            setup_mocks();
            mock_vtable.reply = NULL;
            (void)hu_daemon_dispatch_imessage_reply_ex(
                &mock_ch, &mock_persona, NULL, &mock_config, "+15555551212", 12, NULL, 0,
                "and you?", 8, (const struct hu_conversation_snapshot *)&snap, mid, &sent,
                HU_REACTION_HEART);
            HU_ASSERT_EQ(react_emoji_calls, 0);
            HU_ASSERT_EQ(send_calls, 1);
        }
        return;
    }
    HU_ASSERT_TRUE(false); /* no seed reacted: the sweep proved nothing */
}

/* Round-1 fix: a director reaction must not cost the reply its threading. When
 * the predicate draws THREADED, the reply still goes through reply() (native
 * thread) and the reaction is added alongside it. */
static void director_reaction_keeps_a_threaded_reply_threaded(void) {
    bool saw_threaded = false;
    for (int64_t mid = 7000; mid <= 7400; mid++) {
        setup_mocks();
        hu_conversation_snapshot_t snap = {0};
        snap.parent_seconds_ago = 400;
        snap.parent_is_question = true;
        snap.other_threaded_replies_recent = 3;
        snap.conv_density_msgs_per_min = 6.0f;
        bool sent = false;
        hu_error_t err = hu_daemon_dispatch_imessage_reply_ex(
            &mock_ch, &mock_persona, NULL, &mock_config, "+15555551212", 12, "GUID-1", 6, "yes", 3,
            (const struct hu_conversation_snapshot *)&snap, mid, &sent, HU_REACTION_HEART);
        HU_ASSERT_EQ((int)err, (int)HU_OK);
        HU_ASSERT_TRUE(sent);
        HU_ASSERT_EQ(react_emoji_calls, 1);
        HU_ASSERT_EQ(reply_calls + send_calls, 1);
        if (reply_calls == 1)
            saw_threaded = true;
    }
    HU_ASSERT_TRUE(saw_threaded);
}

/* DEF-2 (2026-10-01): the director chose TEXT and the model wrote it; the
 * whole-reply dispatch (daemon.c) used to let the reply-style predicate's
 * random draw (p_tap 0.15) answer with a bare thumbs-up instead — 15
 * "tapback emoji sent" in prod. Across the seed space the text always goes out
 * and no reaction is invented. */
static void director_text_reply_is_never_swallowed_by_a_random_tapback(void) {
    for (int64_t mid = 3000; mid <= 3400; mid++) {
        setup_mocks();
        mock_vtable.reply = NULL;
        hu_conversation_snapshot_t snap = {0};
        snap.conv_density_msgs_per_min = 20.0f;
        snap.parent_seconds_ago = 5;
        bool sent = false;
        hu_error_t err = hu_daemon_dispatch_imessage_reply_ex(
            &mock_ch, &mock_persona, NULL, &mock_config, "+15555551212", 12, NULL, 0, "hi", 2,
            (const struct hu_conversation_snapshot *)&snap, mid, &sent, HU_REACTION_NONE);
        HU_ASSERT_EQ((int)err, (int)HU_OK);
        HU_ASSERT_TRUE(sent);
        HU_ASSERT_EQ(send_calls, 1);
        HU_ASSERT_EQ(react_emoji_calls, 0);
    }
}

static void msg_ex_parrot_guard_reports_no_text_sent(void) {
    setup_mocks();
    hu_channel_loop_msg_t m;
    memset(&m, 0, sizeof(m));
    /* The parrot predicate ignores bubbles under 16 bytes ("ok", "lol" are
     * legitimate echoes); use a real sentence. */
    const char *echo = "did you get the wrong chat";
    snprintf(m.content, sizeof(m.content), "%s", echo);
    bool sent = true;
    hu_error_t err = hu_daemon_dispatch_imessage_reply_msg_ex(
        &mock_ch, &mock_persona, NULL, &mock_config, "+15555551212", 12,
        (const struct hu_channel_loop_msg *)&m, echo, strlen(echo), &sent, HU_REACTION_NONE);
    HU_ASSERT_EQ((int)err, (int)HU_OK); /* dropped, not failed */
    HU_ASSERT_FALSE(sent);
    HU_ASSERT_EQ(send_calls + reply_calls + react_emoji_calls, 0);
}

static void record_delivered_reply_noops_without_collector(void) {
    static hu_agent_t agent;
    memset(&agent, 0, sizeof(agent)); /* sota_initialized == false */
    HU_ASSERT_EQ(hu_daemon_record_delivered_reply(NULL, "imessage", "+1", 2, "p", 1, "t", 1),
                 HU_OK);
    HU_ASSERT_EQ(hu_daemon_record_delivered_reply(&agent, "imessage", "+1", 2, "p", 1, "t", 1),
                 HU_OK);
}

#if defined(HU_ENABLE_SQLITE) && defined(HU_ENABLE_ML)
static int count_rows(sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    int n = -1;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM production_outcomes", -1, &st, NULL) ==
            SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

/* Self-test traffic is not training data (2026-09-30: 43 production_outcomes
 * rows from the owner's own number, several of them garbage rewrites, sat in
 * the table the evals read). Handles the persona marks relationship "test"
 * are never recorded; everyone else still is. */
static void record_delivered_reply_skips_the_owners_test_handles(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_dpo_collector_t col = {0};
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 64, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);
    static const char pj[] = "{\"name\":\"Seth\",\"contacts\":{\"+15550000099\":{\"name\":"
                             "\"Seth\",\"relationship\":\"test\"}}}";
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    HU_ASSERT_EQ(hu_persona_load_json(&alloc, pj, strlen(pj), &persona), HU_OK);
    static hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.sota.dpo_collector = col;
    agent.sota.sota_initialized = true;
    agent.persona = &persona;
    HU_ASSERT_EQ(hu_daemon_record_delivered_reply(&agent, "imessage", "+15550000099", 12, "q", 1,
                                                  "went better than expected actually", 34),
                 HU_OK);
    HU_ASSERT_EQ(count_rows(db), 0);
    HU_ASSERT_EQ(hu_daemon_record_delivered_reply(&agent, "imessage", "+15555551212", 12, "q", 1,
                                                  "sounds good", 11),
                 HU_OK);
    HU_ASSERT_EQ(count_rows(db), 1);
    hu_persona_deinit(&alloc, &persona);
    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
}

static void record_delivered_reply_stores_the_text_as_sent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_dpo_collector_t col = {0};
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 64, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);
    static hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.sota.dpo_collector = col;
    agent.sota.sota_initialized = true;

    /* Shaped, delivered text — capitalized by the governor, no trailing
     * period — is what lands, not the model's draft. */
    const char *delivered = "Need more details first";
    HU_ASSERT_EQ(hu_daemon_record_delivered_reply(&agent, "imessage", "+15555551212", 12,
                                                  "can you do 4300?", 16, delivered,
                                                  strlen(delivered)),
                 HU_OK);
    HU_ASSERT_EQ(count_rows(db), 1);
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT chosen, channel, target FROM production_outcomes",
                                    -1, &st, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(st, 0), delivered);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(st, 1), "imessage");
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(st, 2), "+15555551212");
    sqlite3_finalize(st);

    /* No prompt (media-only turn) and empty text: no row, no error. */
    HU_ASSERT_EQ(hu_daemon_record_delivered_reply(&agent, "imessage", "+15555551212", 12, "", 0,
                                                  delivered, strlen(delivered)),
                 HU_OK);
    HU_ASSERT_EQ(
        hu_daemon_record_delivered_reply(&agent, "imessage", "+15555551212", 12, "p", 1, "", 0),
        HU_OK);
    HU_ASSERT_EQ(count_rows(db), 1);
    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
}
#endif

/* ── BUG #3: threaded-reply parent-match predicate ──────────────────────
 * hu_imessage_desc_prefix_match must match the parent prefix only when it
 * begins at a word boundary (kills the wrong-parent mid-token false match),
 * while still matching a prefix truncated mid-word (trailing unconstrained). */

#if HU_HAS_IMESSAGE /* hu_imessage_desc_prefix_match is defined in the HU_HAS_IMESSAGE-gated \
                       imessage.c */
static void desc_prefix_match_at_string_start(void) {
    /* Prefix begins the haystack — trivially a boundary. */
    HU_ASSERT_TRUE(hu_imessage_desc_prefix_match("How's st. Pete?", "How's st"));
}

static void desc_prefix_match_after_separator(void) {
    /* Real AX description shape: sender/timestamp then ": " then the body.
     * The prefix begins at a boundary (after the space). */
    HU_ASSERT_TRUE(hu_imessage_desc_prefix_match("From Alexis at 10:13 AM: How's st", "How's st"));
}

static void desc_prefix_no_match_mid_token(void) {
    /* The prefix appears only as a fragment inside a longer word — the
     * preceding byte is alphanumeric, so it must NOT match (the bug). */
    HU_ASSERT_FALSE(hu_imessage_desc_prefix_match("everyoneHow's stuff", "How's st"));
}

static void desc_prefix_match_truncated_mid_word(void) {
    /* Parent prefix is truncated mid-word ("st" cut from "st. Pete"); the
     * trailing edge is unconstrained, so it must still match. A both-sided
     * word-boundary matcher would WRONGLY reject this. */
    HU_ASSERT_TRUE(hu_imessage_desc_prefix_match(": How's st", "How's st"));
}

static void desc_prefix_match_null_and_empty_safe(void) {
    HU_ASSERT_FALSE(hu_imessage_desc_prefix_match(NULL, "x"));
    HU_ASSERT_FALSE(hu_imessage_desc_prefix_match("hay", NULL));
    HU_ASSERT_FALSE(hu_imessage_desc_prefix_match("hay", ""));
}

static void desc_prefix_no_match_absent(void) {
    HU_ASSERT_FALSE(hu_imessage_desc_prefix_match("totally different message", "How's st"));
}
#endif /* HU_HAS_IMESSAGE */

/* AC: when the dispatcher picks THREADED and a parent GUID resolves to text,
 * the body sent is the QUOTED form (↩ "<parent>"\n<body>) — the working
 * substitute for native threading, which is unreachable via automation.
 * Same loop-until-THREADED pattern as reply_failure_falls_back_to_flat. With a
 * non-NULL agent+allocator and an injected parent lookup, the reply attempt
 * receives the quoted body (mock_reply returns OK → threaded_flat path).
 *
 * Guarded by HU_HAS_IMESSAGE: the parent lookup + its test-injection helper
 * (hu_imessage_test_set_guid_lookup) live in imessage.c, compiled only on
 * platforms with the iMessage channel. The dispatcher's quote block is gated
 * the same way, so off-platform there is nothing to assert. */
#if HU_HAS_IMESSAGE
/* 2026-07-20: inverted. This test used to PIN the fake inline `↩ "quote"`
 * substitute, which existed only because native threading was believed
 * unreachable. With the IMCore bridge live the reply genuinely nests
 * (thread_originator_guid == parent), so the quote is pure bot-tell and is
 * gone. The contract is now: the body sent is the body given, verbatim. */
static void threaded_reply_sends_plain_body_never_quotes(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &sys;

    hu_imessage_test_set_guid_lookup("QUOTE-PARENT-GUID", "dinner tonight?");

    hu_conversation_snapshot_t snap = {0};
    snap.parent_seconds_ago = 300;
    snap.parent_is_question = true;
    snap.other_threaded_replies_recent = 4;
    snap.conv_density_msgs_per_min = 1.0f;

    bool hit = false;
    for (int64_t mid = 1; mid <= 50 && !hit; mid++) {
        setup_mocks();
        last_reply_body[0] = '\0';
        hu_error_t err = hu_daemon_dispatch_imessage_reply(
            &mock_ch, &mock_persona, &agent, &mock_config, "+15555551212", 12, "QUOTE-PARENT-GUID",
            17, "hi", 2, (const struct hu_conversation_snapshot *)&snap, mid);
        HU_ASSERT_EQ((int)err, (int)HU_OK);
        if (reply_calls > 0) {
            hit = true;
            /* Plain body, verbatim — no fabricated quote prefix. */
            HU_ASSERT_STR_EQ(last_reply_body, "hi");
            HU_ASSERT_TRUE(strstr(last_reply_body, "\xE2\x86\xA9") == NULL);
        }
    }
    HU_ASSERT(hit);
}
#endif /* HU_HAS_IMESSAGE */

/* ── Roadmap #18: stale-tapback demotion on the reply-style path ────── */

/* Pure demotion truth table: stale parent collapses tapback styles to FLAT;
 * everything else is untouched. */
static void demote_stale_tapback_style_truth_table(void) {
    /* No band → 15-min default cap. The audit case: 100 min stale. */
    HU_ASSERT_EQ((int)hu_daemon_demote_stale_tapback_style(HU_REPLY_STYLE_TAPBACK, 100 * 60, NULL),
                 (int)HU_REPLY_STYLE_FLAT);
    HU_ASSERT_EQ(
        (int)hu_daemon_demote_stale_tapback_style(HU_REPLY_STYLE_TAPBACK_PLUS_FLAT, 100 * 60, NULL),
        (int)HU_REPLY_STYLE_FLAT);
    /* Fresh parent: tapback styles pass through. */
    HU_ASSERT_EQ((int)hu_daemon_demote_stale_tapback_style(HU_REPLY_STYLE_TAPBACK, 30, NULL),
                 (int)HU_REPLY_STYLE_TAPBACK);
    /* Non-tapback styles never demoted, even when stale. */
    HU_ASSERT_EQ((int)hu_daemon_demote_stale_tapback_style(HU_REPLY_STYLE_THREADED, 100 * 60, NULL),
                 (int)HU_REPLY_STYLE_THREADED);
    HU_ASSERT_EQ((int)hu_daemon_demote_stale_tapback_style(HU_REPLY_STYLE_FLAT, 100 * 60, NULL),
                 (int)HU_REPLY_STYLE_FLAT);
    /* Unknown age (0) → don't demote on missing data. */
    HU_ASSERT_EQ((int)hu_daemon_demote_stale_tapback_style(HU_REPLY_STYLE_TAPBACK, 0, NULL),
                 (int)HU_REPLY_STYLE_TAPBACK);
    /* A measured band tightens the cap below the default. */
    hu_tapback_band_t b = {0};
    b.valid = true;
    b.p90_ms = 2 * 60 * 1000;
    HU_ASSERT_EQ((int)hu_daemon_demote_stale_tapback_style(HU_REPLY_STYLE_TAPBACK, 5 * 60, &b),
                 (int)HU_REPLY_STYLE_FLAT);
    HU_ASSERT_EQ((int)hu_daemon_demote_stale_tapback_style(HU_REPLY_STYLE_TAPBACK, 60, &b),
                 (int)HU_REPLY_STYLE_TAPBACK);
}

/* snapshot-age helper: 0/negative/future timestamps report unknown (0). */
static void snapshot_age_sec_handles_unknown_and_future(void) {
    HU_ASSERT_EQ((int)hu_daemon_snapshot_age_sec(0), 0);
    HU_ASSERT_EQ((int)hu_daemon_snapshot_age_sec(-7), 0);
    int64_t now = (int64_t)time(NULL);
    HU_ASSERT_EQ((int)hu_daemon_snapshot_age_sec(now + 3600), 0);
    int64_t age = hu_daemon_snapshot_age_sec(now - 100);
    HU_ASSERT_TRUE(age >= 100 && age <= 102);
}

/* Dispatcher-level: a stale parent (audit case, 100 min) must NEVER produce a
 * react_emoji, even when the director asked for a reaction — never a late
 * tapback. The reply text must still be delivered. */
static void stale_parent_never_reacts_tapback(void) {
    hu_conversation_snapshot_t snap = {0};
    snap.parent_seconds_ago = 100 * 60;

    for (int64_t mid = 1; mid <= 50; mid++) {
        setup_mocks();
        mock_vtable.reply = NULL; /* keep THREADED off the table for determinism */
        bool sent = false;
        hu_error_t err = hu_daemon_dispatch_imessage_reply_ex(
            &mock_ch, &mock_persona, NULL, &mock_config, "+15555551212", 12, NULL, 0, "hi", 2,
            (const struct hu_conversation_snapshot *)&snap, 5000 + mid, &sent, HU_REACTION_HEART);
        HU_ASSERT_EQ((int)err, (int)HU_OK);
        HU_ASSERT_EQ(react_emoji_calls, 0); /* no late tapback, ever */
        HU_ASSERT(send_calls >= 1);         /* the text still flows */
    }
}

/* Control for the sweep above: with a FRESH parent the same director request
 * DOES reach react_emoji — proving the stale sweep is non-vacuous (the band,
 * not a missing reaction, is what suppresses it). */
static void fresh_parent_still_reacts_tapback_sometimes(void) {
    hu_conversation_snapshot_t snap = {0};
    snap.parent_seconds_ago = 30;
    setup_mocks();
    mock_vtable.reply = NULL;
    bool sent = false;
    hu_error_t err = hu_daemon_dispatch_imessage_reply_ex(
        &mock_ch, &mock_persona, NULL, &mock_config, "+15555551212", 12, NULL, 0, "hi", 2,
        (const struct hu_conversation_snapshot *)&snap, 6001, &sent, HU_REACTION_HEART);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(react_emoji_calls, 1);
    HU_ASSERT_EQ(send_calls, 1);
}

/* The burst re-poll before a reply consumes everything new on the channel.
 * 2026-09-30: a message from another sender that landed during the reading
 * delay (Dermot during Lexi's turn, twice on 09-24) was read and discarded;
 * it must be carried into the tick's batch list instead. */
/* The director's "reply after N seconds" used to be slept BEFORE the turn's
 * work (memory, planner, generation: ~30-50 s), so the two added up and the
 * twin never answered in under ~50 s (median 1.8 min vs Seth's 30 s, 30 days to
 * 2026-10-01). Now the delay is a hold the send waits out: the work runs
 * inside it. */
static void reply_hold_is_per_contact_and_replaced_by_the_next(void) {
    hu_daemon_reply_hold("+15550001111", 12, 10000);
    HU_ASSERT_EQ(hu_daemon_reply_hold_wait_ms("+15550001111", 12, 4000), (int64_t)6000);
    HU_ASSERT_EQ(hu_daemon_reply_hold_wait_ms("+15550002222", 12, 4000), (int64_t)0);
    HU_ASSERT_EQ(hu_daemon_reply_hold_wait_ms("+15550001111", 12, 12000), (int64_t)0);
    hu_daemon_reply_hold("+15550002222", 12, 9000);
    HU_ASSERT_EQ(hu_daemon_reply_hold_wait_ms("+15550001111", 12, 4000), (int64_t)0);
    HU_ASSERT_EQ(hu_daemon_reply_hold_wait_ms("+15550002222", 12, 4000), (int64_t)5000);
    hu_daemon_reply_hold(NULL, 0, 0);
    HU_ASSERT_EQ(hu_daemon_reply_hold_wait_ms("+15550002222", 12, 4000), (int64_t)0);
}

/* A burst's later bubbles must not wait again: the first send consumes it. */
static void dispatch_consumes_the_reply_hold(void) {
    setup_mocks();
    mock_vtable.reply = NULL;
    mock_vtable.react_emoji = NULL;
    hu_daemon_reply_hold_for("+15555551212", 12, 600000);
    HU_ASSERT_TRUE(hu_daemon_reply_hold_wait_ms("+15555551212", 12, hu_time_get_current_ms()) > 0);
    static const char body[] = "omw";
    hu_conversation_snapshot_t snap = {0};
    snap.parent_seconds_ago = 5;
    HU_ASSERT_EQ((int)hu_daemon_dispatch_imessage_reply(
                     &mock_ch, &mock_persona, NULL, &mock_config, "+15555551212", 12, NULL, 0, body,
                     sizeof(body) - 1, (const struct hu_conversation_snapshot *)&snap, 6),
                 (int)HU_OK);
    HU_ASSERT_EQ(hu_daemon_reply_hold_wait_ms("+15555551212", 12, hu_time_get_current_ms()),
                 (int64_t)0);
}

/* Every bubble is cased at dispatch, not just the reply's first line: a
 * bubble split mid-line ("nah too windy." | "just hung out...") used to go out
 * lowercase (32% of follow-on bubbles, 2026-09-30). */
static void dispatch_capitalizes_a_lowercase_bubble_when_the_governor_is_live(void) {
    setup_mocks();
    mock_vtable.reply = NULL;
    mock_vtable.react_emoji = NULL;
    hu_style_governor_set_mode_for_test(HU_STYLE_GOVERNOR_LIVE);
    static const char body[] = "just hung out by the water";
    /* This text's roll takes the capitalize branch at the card's rate. */
    HU_ASSERT_TRUE(hu_style_governor_casing_roll(body, sizeof(body) - 1) >=
                   hu_style_governor_lowercase_start_pct(&mock_persona));
    hu_conversation_snapshot_t snap = {0};
    snap.parent_seconds_ago = 5;
    HU_ASSERT_EQ((int)hu_daemon_dispatch_imessage_reply(
                     &mock_ch, &mock_persona, NULL, &mock_config, "+15555551212", 12, NULL, 0, body,
                     sizeof(body) - 1, (const struct hu_conversation_snapshot *)&snap, 6),
                 (int)HU_OK);
    HU_ASSERT_STR_EQ(last_send_body, "Just hung out by the water");
    hu_style_governor_set_mode_for_test(-1);
}

/* The quality retry freed the draft before regenerating; when the retry came
 * back empty the contact got nothing (Lexi 2026-09-23: an 81-char draft
 * scored 55, the retry was empty; 1 of 5 retries in a week ended empty). */
static char *qd_dup(hu_allocator_t *a, const char *s) {
    size_t n = strlen(s);
    char *p = (char *)a->alloc(a->ctx, n + 1);
    memcpy(p, s, n + 1);
    return p;
}

static void quality_draft_restores_when_the_retry_is_empty(void) {
    hu_allocator_t a = hu_system_allocator();
    char *draft = qd_dup(&a, "Your cat seems to be enjoying the new home");
    hu_daemon_quality_draft_keep(&a, "+15550000001", 12, draft, strlen(draft));
    char *resp = NULL;
    size_t len = 0;
    HU_ASSERT_TRUE(hu_daemon_quality_draft_settle(&a, "+15550000001", 12, &resp, &len));
    HU_ASSERT_STR_EQ(resp, "Your cat seems to be enjoying the new home");
    a.free(a.ctx, resp, len + 1);
    /* Settled: nothing left to restore into a later empty turn. */
    resp = NULL;
    len = 0;
    HU_ASSERT_FALSE(hu_daemon_quality_draft_settle(&a, "+15550000001", 12, &resp, &len));
}

static void quality_draft_is_dropped_when_the_retry_succeeds_or_the_contact_differs(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_daemon_quality_draft_keep(&a, "+15550000001", 12, qd_dup(&a, "old draft"), 9);
    char *resp = qd_dup(&a, "better reply");
    size_t len = 12;
    HU_ASSERT_TRUE(hu_daemon_quality_draft_settle(&a, "+15550000001", 12, &resp, &len));
    HU_ASSERT_STR_EQ(resp, "better reply");
    a.free(a.ctx, resp, len + 1);
    resp = NULL;
    len = 0;
    HU_ASSERT_FALSE(hu_daemon_quality_draft_settle(&a, "+15550000001", 12, &resp, &len));

    hu_daemon_quality_draft_keep(&a, "+15550000001", 12, qd_dup(&a, "for someone else"), 16);
    HU_ASSERT_FALSE(hu_daemon_quality_draft_settle(&a, "+15550000002", 12, &resp, &len));
    HU_ASSERT_NULL(resp);
}

static void burst_carry_keeps_other_senders_for_this_tick(void) {
    static hu_channel_loop_msg_t msgs[4], burst[3];
    memset(msgs, 0, sizeof(msgs));
    memset(burst, 0, sizeof(burst));
    size_t count = 1;
    strcpy(msgs[0].session_key, "+15550000001");
    strcpy(msgs[0].content, "heyy");
    strcpy(burst[0].session_key, "+15550000001"); /* the batch's own follow-up */
    strcpy(burst[0].content, "what are you up to");
    strcpy(burst[1].session_key, "+15550000002");
    strcpy(burst[1].content, "did you see the game");
    burst[1].message_id = 42;
    strcpy(burst[2].session_key, "+15550000003"); /* empty content: nothing to answer */
    size_t lost = hu_daemon_burst_carry(msgs, &count, 4, burst, 3, "+15550000001");
    HU_ASSERT_EQ(lost, 0u);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_STR_EQ(msgs[1].session_key, "+15550000002");
    HU_ASSERT_STR_EQ(msgs[1].content, "did you see the game");
    HU_ASSERT_EQ(msgs[1].message_id, 42);
}

/* Vision never goes to a text-only local primary. 2026-09-30: every photo hit
 * :8741 (422, "no vision processor") first; those failures opened the primary's
 * circuit breaker (23 of 25 opens followed a vision call), so the next 5 min of
 * real replies came from the cloud fallback instead of the persona model. */
static void vision_route_uses_the_declared_cloud_fallback(void) {
    char *fb_models[] = {"gemini-3.8-flash"};
    hu_config_model_fallback_t mf = {
        .model = "GLM-4.5-Air-4bit", .fallback_models = fb_models, .fallback_models_len = 1};
    char *fb_providers[] = {"gemini"};
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.reliability.primary_provider = "mlx_local";
    cfg.reliability.fallback_providers = fb_providers;
    cfg.reliability.fallback_providers_len = 1;
    cfg.reliability.model_fallbacks = &mf;
    cfg.reliability.model_fallbacks_len = 1;
    const char *prov = NULL, *model = NULL;
    HU_ASSERT_TRUE(hu_daemon_vision_route(&cfg, "GLM-4.5-Air-4bit", 16, &prov, &model));
    HU_ASSERT_STR_EQ(prov, "gemini");
    HU_ASSERT_STR_EQ(model, "gemini-3.8-flash");
    /* No declared mapping for the model: keep the agent's own provider. */
    HU_ASSERT_FALSE(hu_daemon_vision_route(&cfg, "gpt-4o", 6, &prov, &model));
    cfg.reliability.fallback_providers_len = 0;
    HU_ASSERT_FALSE(hu_daemon_vision_route(&cfg, "GLM-4.5-Air-4bit", 16, &prov, &model));
    HU_ASSERT_FALSE(hu_daemon_vision_route(NULL, "GLM-4.5-Air-4bit", 16, &prov, &model));
}

/* A photo vision could not describe must not reach the model as a bare U+FFFC:
 * alone it drew "Please let me know what information you need" (Mindy
 * 09-27); with text it drew "looks cozy!" about a photo never seen (09-30). */
static void unseen_photo_alone_becomes_a_note(void) {
    char buf[256];
    size_t len = 3;
    const char *out = hu_daemon_unseen_photo("\xEF\xBF\xBC", &len, buf, sizeof(buf));
    HU_ASSERT_STR_EQ(out, "[They sent a picture that didn't load on your phone \xE2\x80\x94 you "
                          "can't see it]");
    HU_ASSERT_EQ(len, strlen(out));
}

static void unseen_photo_with_text_keeps_the_text(void) {
    char buf[256];
    const char *in = "\xEF\xBF\xBCMy new little office at kiln";
    size_t len = strlen(in);
    const char *out = hu_daemon_unseen_photo(in, &len, buf, sizeof(buf));
    HU_ASSERT_STR_EQ(out, "My new little office at kiln\n[They sent a picture that didn't load "
                          "on your phone \xE2\x80\x94 you can't see it]");
}

static void unseen_photo_leaves_described_and_plain_text_alone(void) {
    char buf[256];
    size_t len = 5;
    HU_ASSERT_STR_EQ(hu_daemon_unseen_photo("hello", &len, buf, sizeof(buf)), "hello");
    /* Already rewritten into buf by vision: returned as is. */
    strcpy(buf, "\xEF\xBF\xBC\n[They sent a photo: a dog]");
    len = strlen(buf);
    HU_ASSERT_TRUE(hu_daemon_unseen_photo(buf, &len, buf, sizeof(buf)) == buf);
    HU_ASSERT_EQ(len, strlen("\xEF\xBF\xBC\n[They sent a photo: a dog]"));
}

static void burst_carry_reports_what_it_cannot_keep(void) {
    static hu_channel_loop_msg_t msgs[2], burst[2];
    memset(msgs, 0, sizeof(msgs));
    memset(burst, 0, sizeof(burst));
    size_t count = 1;
    strcpy(msgs[0].session_key, "+15550000001");
    strcpy(burst[0].session_key, "+15550000002");
    strcpy(burst[0].content, "one");
    strcpy(burst[1].session_key, "+15550000003");
    strcpy(burst[1].content, "two");
    HU_ASSERT_EQ(hu_daemon_burst_carry(msgs, &count, 2, burst, 2, "+15550000001"), 1u);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_STR_EQ(msgs[1].content, "one");
}

void run_imessage_dispatcher_tests(void) {
    HU_TEST_SUITE("imessage_dispatcher");
    HU_RUN_TEST(invalid_args_short_circuit);
    HU_RUN_TEST(disabled_feature_falls_back_to_flat);
    HU_RUN_TEST(reply_failure_falls_back_to_flat);
    HU_RUN_TEST(empty_parent_guid_never_attempts_threaded_reply);
    HU_RUN_TEST(no_reply_vtable_falls_back_to_flat);
    HU_RUN_TEST(no_react_emoji_vtable_falls_back_to_flat);
    HU_RUN_TEST(pacing_enforces_minimum_delay);
    HU_RUN_TEST(all_paths_fail_returns_send_error);
    HU_RUN_TEST(flat_style_routes_to_send);
    HU_RUN_TEST(text_sent_true_on_flat_send_false_when_send_refuses);
    HU_RUN_TEST(text_sent_true_when_feature_disabled_falls_back_to_flat);
    HU_RUN_TEST(director_reaction_rides_along_with_the_text);
    HU_RUN_TEST(second_bubble_never_reacts_to_the_same_message_again);
    HU_RUN_TEST(director_text_reply_is_never_swallowed_by_a_random_tapback);
    HU_RUN_TEST(director_reaction_keeps_a_threaded_reply_threaded);
    HU_RUN_TEST(msg_ex_parrot_guard_reports_no_text_sent);
    HU_RUN_TEST(record_delivered_reply_noops_without_collector);
    HU_RUN_TEST(burst_carry_keeps_other_senders_for_this_tick);
    HU_RUN_TEST(quality_draft_restores_when_the_retry_is_empty);
    HU_RUN_TEST(quality_draft_is_dropped_when_the_retry_succeeds_or_the_contact_differs);
    HU_RUN_TEST(dispatch_capitalizes_a_lowercase_bubble_when_the_governor_is_live);
    HU_RUN_TEST(reply_hold_is_per_contact_and_replaced_by_the_next);
    HU_RUN_TEST(dispatch_consumes_the_reply_hold);
    HU_RUN_TEST(burst_carry_reports_what_it_cannot_keep);
    HU_RUN_TEST(vision_route_uses_the_declared_cloud_fallback);
    HU_RUN_TEST(unseen_photo_alone_becomes_a_note);
    HU_RUN_TEST(unseen_photo_with_text_keeps_the_text);
    HU_RUN_TEST(unseen_photo_leaves_described_and_plain_text_alone);
#if defined(HU_ENABLE_SQLITE) && defined(HU_ENABLE_ML)
    HU_RUN_TEST(record_delivered_reply_stores_the_text_as_sent);
    HU_RUN_TEST(record_delivered_reply_skips_the_owners_test_handles);
#endif
    HU_RUN_TEST(demote_stale_tapback_style_truth_table);
    HU_RUN_TEST(snapshot_age_sec_handles_unknown_and_future);
    HU_RUN_TEST(stale_parent_never_reacts_tapback);
    HU_RUN_TEST(fresh_parent_still_reacts_tapback_sometimes);
#if HU_HAS_IMESSAGE
    HU_RUN_TEST(threaded_reply_sends_plain_body_never_quotes);
    HU_RUN_TEST(desc_prefix_match_at_string_start);
    HU_RUN_TEST(desc_prefix_match_after_separator);
    HU_RUN_TEST(desc_prefix_no_match_mid_token);
    HU_RUN_TEST(desc_prefix_match_truncated_mid_word);
    HU_RUN_TEST(desc_prefix_match_null_and_empty_safe);
    HU_RUN_TEST(desc_prefix_no_match_absent);
#endif
}
