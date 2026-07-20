/* Coverage anchor: these tests exercise the iMessage reply dispatcher and
 * cross-channel context formatters in src/daemon/daemon_message_router.c
 * (hu_daemon_dispatch_imessage_reply). Naming the source path here lets
 * check-untested.sh recognize the file as tested: its exported symbols use
 * the hu_daemon_dispatch and hu_daemon_cross_channel prefixes rather than
 * hu_daemon_message_router, so the basename heuristic alone would miss it. */
#include "human/agent.h"
#include "human/channel.h"
#include "human/channels/imessage.h"
#include "human/channels/imessage_action.h"
#include "human/channels/imessage_action_facts.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/time.h"
#include "human/daemon.h"
#include "human/daemon/message_router.h"
#include "human/persona.h"
#include "test_framework.h"
#include <stdint.h>
#include <string.h>
#include <time.h>

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
 * react_emoji, across the predicate's whole seed space — the tapback mass is
 * ~20% per draw with these facts, so 50 seeds would hit it many times if the
 * demotion gate were missing. The reply text must still be delivered. */
static void stale_parent_never_reacts_tapback(void) {
    hu_conversation_snapshot_t snap = {0};
    snap.parent_seconds_ago = 100 * 60;

    for (int64_t mid = 1; mid <= 50; mid++) {
        setup_mocks();
        mock_vtable.reply = NULL; /* keep THREADED off the table for determinism */
        hu_error_t err = hu_daemon_dispatch_imessage_reply(
            &mock_ch, &mock_persona, NULL, &mock_config, "+15555551212", 12, NULL, 0, "hi", 2,
            (const struct hu_conversation_snapshot *)&snap, mid);
        HU_ASSERT_EQ((int)err, (int)HU_OK);
        HU_ASSERT_EQ(react_emoji_calls, 0); /* no late tapback, ever */
        HU_ASSERT(send_calls >= 1);         /* the text still flows */
    }
}

/* Control for the sweep above: with a FRESH parent the same facts DO reach
 * react_emoji within 50 seeds — proving the stale sweep is non-vacuous (the
 * gate, not the predicate weights, is what suppresses the reaction). */
static void fresh_parent_still_reacts_tapback_sometimes(void) {
    hu_conversation_snapshot_t snap = {0};
    snap.parent_seconds_ago = 30;

    bool tapback_hit = false;
    for (int64_t mid = 1; mid <= 50 && !tapback_hit; mid++) {
        setup_mocks();
        mock_vtable.reply = NULL;
        hu_error_t err = hu_daemon_dispatch_imessage_reply(
            &mock_ch, &mock_persona, NULL, &mock_config, "+15555551212", 12, NULL, 0, "hi", 2,
            (const struct hu_conversation_snapshot *)&snap, mid);
        HU_ASSERT_EQ((int)err, (int)HU_OK);
        if (react_emoji_calls > 0)
            tapback_hit = true;
    }
    HU_ASSERT(tapback_hit);
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
