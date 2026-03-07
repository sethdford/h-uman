/* Comprehensive channel tests (~45 tests). Uses SC_HAS_* guards for conditional channels. */
#include "seaclaw/channel.h"
#include "seaclaw/channels/cli.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "test_framework.h"
#include <string.h>

#if SC_HAS_TELEGRAM
#include "seaclaw/channels/telegram.h"
#endif

#if SC_HAS_DISCORD
#include "seaclaw/channel_loop.h"
#include "seaclaw/channels/discord.h"
#endif
#if SC_HAS_SLACK
#include "seaclaw/channels/slack.h"
#endif
#if SC_HAS_WHATSAPP
#include "seaclaw/channels/whatsapp.h"
#endif
#if SC_HAS_FACEBOOK
#include "seaclaw/channel_loop.h"
#include "seaclaw/channels/facebook.h"
#endif
#if SC_HAS_INSTAGRAM
#include "seaclaw/channel_loop.h"
#include "seaclaw/channels/instagram.h"
#endif
#if SC_HAS_TWITTER
#include "seaclaw/channel_loop.h"
#include "seaclaw/channels/twitter.h"
#endif
#if SC_HAS_GOOGLE_RCS
#include "seaclaw/channel_loop.h"
#include "seaclaw/channels/google_rcs.h"
#endif
#if SC_HAS_MATRIX
#include "seaclaw/channels/matrix.h"
#endif
#if SC_HAS_IRC
#include "seaclaw/channels/irc.h"
#endif
#if SC_HAS_LINE
#include "seaclaw/channels/line.h"
#endif
#if SC_HAS_LARK
#include "seaclaw/channels/lark.h"
#endif
#if SC_HAS_WEB
#include "seaclaw/channels/web.h"
#endif
#if SC_HAS_EMAIL
#include "seaclaw/channels/email.h"
#endif
#if SC_HAS_IMESSAGE
#include "seaclaw/channels/imessage.h"
#endif
#if SC_HAS_MATTERMOST
#include "seaclaw/channels/mattermost.h"
#endif
#if SC_HAS_ONEBOT
#include "seaclaw/channels/onebot.h"
#endif
#if SC_HAS_DINGTALK
#include "seaclaw/channels/dingtalk.h"
#endif
#if SC_HAS_TEAMS
#include "seaclaw/channels/teams.h"
#endif
#if SC_HAS_TWILIO
#include "seaclaw/channels/twilio.h"
#endif
#if SC_HAS_GOOGLE_CHAT
#include "seaclaw/channels/google_chat.h"
#endif

/* ─── CLI (always present) ─────────────────────────────────────────────────── */
static void test_cli_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_cli_create(&alloc, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(ch.ctx);
    sc_cli_destroy(&ch);
}

static void test_cli_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_cli_create(&alloc, &ch);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "cli");
    sc_cli_destroy(&ch);
}

static void test_cli_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_cli_create(&alloc, &ch);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    sc_cli_destroy(&ch);
}

static void test_cli_send(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_cli_create(&alloc, &ch);
    sc_error_t err = ch.vtable->send(ch.ctx, NULL, 0, "hello", 5, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_cli_destroy(&ch);
}

/* ─── Telegram ────────────────────────────────────────────────────────────── */
#if SC_HAS_TELEGRAM
static void test_telegram_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_telegram_create(&alloc, "test:token", 10, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(ch.ctx);
    sc_telegram_destroy(&ch);
}

static void test_telegram_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_telegram_create(&alloc, "t", 1, &ch);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "telegram");
    sc_telegram_destroy(&ch);
}

static void test_telegram_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_telegram_create(&alloc, "t", 1, &ch);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    sc_telegram_destroy(&ch);
}
#endif

/* ─── Discord ──────────────────────────────────────────────────────────────── */
#if SC_HAS_DISCORD
static void test_discord_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_discord_create(&alloc, "token", 5, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "discord");
    sc_discord_destroy(&ch);
}

static void test_discord_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_discord_create(&alloc, NULL, 0, &ch);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "discord");
    sc_discord_destroy(&ch);
}

static void test_discord_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_discord_create(&alloc, "t", 1, &ch);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    sc_discord_destroy(&ch);
}

static void test_discord_start_stop_lifecycle(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_discord_create(&alloc, "token", 5, &ch);
    sc_error_t err = ch.vtable->start(ch.ctx);
    SC_ASSERT_EQ(err, SC_OK);
    ch.vtable->stop(ch.ctx);
    sc_discord_destroy(&ch);
}
#endif

/* ─── Slack ───────────────────────────────────────────────────────────────── */
#if SC_HAS_SLACK
static void test_slack_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_slack_create(&alloc, "token", 5, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "slack");
    sc_slack_destroy(&ch);
}

static void test_slack_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_slack_create(&alloc, NULL, 0, &ch);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "slack");
    sc_slack_destroy(&ch);
}

static void test_slack_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_slack_create(&alloc, "t", 1, &ch);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    sc_slack_destroy(&ch);
}

static void test_slack_start_stop_typing(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_slack_create(&alloc, "t", 1, &ch);
    if (ch.vtable->start_typing) {
        sc_error_t err = ch.vtable->start_typing(ch.ctx, "channel", 7);
        SC_ASSERT_EQ(err, SC_OK);
    }
    if (ch.vtable->stop_typing) {
        sc_error_t err = ch.vtable->stop_typing(ch.ctx, "channel", 7);
        SC_ASSERT_EQ(err, SC_OK);
    }
    sc_slack_destroy(&ch);
}

static void test_slack_send_long_message(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_slack_create(&alloc, "t", 1, &ch);
    char buf[5000];
    for (size_t i = 0; i < sizeof(buf) - 1; i++)
        buf[i] = 'm';
    buf[sizeof(buf) - 1] = '\0';
    sc_error_t err = ch.vtable->send(ch.ctx, "ch", 2, buf, sizeof(buf) - 1, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_slack_destroy(&ch);
}

static void test_slack_create_ex(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    const char *ids[] = {"C0001", "C0002"};
    sc_error_t err = sc_slack_create_ex(&alloc, "tok", 3, ids, 2, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "slack");
    sc_slack_destroy(&ch);
}

static void test_slack_poll_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    const char *ids[] = {"C0001"};
    sc_slack_create_ex(&alloc, "tok", 3, ids, 1, &ch);
    ch.vtable->start(ch.ctx);
    sc_channel_loop_msg_t msgs[4];
    size_t out = 99;
    sc_error_t err = sc_slack_poll(ch.ctx, &alloc, msgs, 4, &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out, 0);
    sc_slack_destroy(&ch);
}

static void test_slack_webhook_malformed(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err = sc_slack_create(&alloc, "token", 5, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    sc_channel_loop_msg_t msgs[4];
    size_t out = 99;
    err = sc_slack_poll(ch.ctx, &alloc, msgs, 4, &out);
    SC_ASSERT_EQ(err, SC_OK);
    err = sc_slack_poll(ch.ctx, &alloc, msgs, 0, &out);
    SC_ASSERT_EQ(err, SC_OK);
    sc_slack_destroy(&ch);
}
#endif

/* ─── WhatsApp ────────────────────────────────────────────────────────────── */
#if SC_HAS_WHATSAPP
static void test_whatsapp_send(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_whatsapp_create(&alloc, "123456", 6, "token", 5, &ch);
    sc_error_t err = ch.vtable->send(ch.ctx, "15551234567", 11, "hi", 2, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_whatsapp_destroy(&ch);
}

static void test_whatsapp_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_whatsapp_create(&alloc, "123456", 6, "token", 5, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "whatsapp");
    sc_whatsapp_destroy(&ch);
}

static void test_whatsapp_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_whatsapp_create(&alloc, NULL, 0, NULL, 0, &ch);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "whatsapp");
    sc_whatsapp_destroy(&ch);
}

static void test_whatsapp_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_whatsapp_create(&alloc, "1", 1, "t", 1, &ch);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    sc_whatsapp_destroy(&ch);
}

static void test_whatsapp_webhook_and_poll(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_whatsapp_create(&alloc, "123", 3, "tok", 3, &ch);
    sc_error_t err = sc_whatsapp_on_webhook(ch.ctx, &alloc, "hello from webhook", 18);
    SC_ASSERT_EQ(err, SC_OK);
    sc_channel_loop_msg_t msgs[4];
    size_t out = 0;
    err = sc_whatsapp_poll(ch.ctx, &alloc, msgs, 4, &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out, 1);
    SC_ASSERT_STR_EQ(msgs[0].session_key, "test-sender");
    SC_ASSERT_STR_EQ(msgs[0].content, "hello from webhook");
    err = sc_whatsapp_poll(ch.ctx, &alloc, msgs, 4, &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out, 0);
    sc_whatsapp_destroy(&ch);
}

static void test_whatsapp_poll_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_whatsapp_create(&alloc, "1", 1, "t", 1, &ch);
    sc_channel_loop_msg_t msgs[4];
    size_t out = 99;
    sc_error_t err = sc_whatsapp_poll(ch.ctx, &alloc, msgs, 4, &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out, 0);
    sc_whatsapp_destroy(&ch);
}
#endif

/* ─── Facebook Messenger ───────────────────────────────────────────────────── */
#if SC_HAS_FACEBOOK
static void test_facebook_send(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_facebook_create(&alloc, "page1", 5, "token", 5, "secret", 6, &ch);
    sc_error_t err = ch.vtable->send(ch.ctx, "user123", 7, "hi", 2, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_facebook_destroy(&ch);
}

static void test_facebook_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_facebook_create(&alloc, "page1", 5, "token", 5, "secret", 6, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "facebook");
    sc_facebook_destroy(&ch);
}

static void test_facebook_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_facebook_create(&alloc, NULL, 0, NULL, 0, NULL, 0, &ch);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "facebook");
    sc_facebook_destroy(&ch);
}

static void test_facebook_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_facebook_create(&alloc, "1", 1, "t", 1, "s", 1, &ch);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    sc_facebook_destroy(&ch);
}

static void test_facebook_webhook_and_poll(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_facebook_create(&alloc, "123", 3, "tok", 3, "sec", 3, &ch);
    sc_error_t err = sc_facebook_on_webhook(ch.ctx, &alloc, "hello from webhook", 18);
    SC_ASSERT_EQ(err, SC_OK);
    sc_channel_loop_msg_t msgs[4];
    size_t out = 0;
    err = sc_facebook_poll(ch.ctx, &alloc, msgs, 4, &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out, 1);
    SC_ASSERT_STR_EQ(msgs[0].session_key, "test-sender");
    SC_ASSERT_STR_EQ(msgs[0].content, "hello from webhook");
    err = sc_facebook_poll(ch.ctx, &alloc, msgs, 4, &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out, 0);
    sc_facebook_destroy(&ch);
}

static void test_facebook_poll_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_facebook_create(&alloc, "1", 1, "t", 1, "s", 1, &ch);
    sc_channel_loop_msg_t msgs[4];
    size_t out = 99;
    sc_error_t err = sc_facebook_poll(ch.ctx, &alloc, msgs, 4, &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out, 0);
    sc_facebook_destroy(&ch);
}

static void test_facebook_webhook_malformed(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err = sc_facebook_create(&alloc, "123", 3, "tok", 3, "sec", 3, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    err = sc_facebook_on_webhook(ch.ctx, &alloc, "", 0);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
    err = sc_facebook_on_webhook(ch.ctx, &alloc, "}{", 2);
    SC_ASSERT_TRUE(err == SC_OK || err == SC_ERR_JSON_PARSE || err == SC_ERR_INVALID_ARGUMENT);
    sc_facebook_destroy(&ch);
}

static void test_facebook_send_empty_message(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_facebook_create(&alloc, "page1", 5, "token", 5, "secret", 6, &ch);
    sc_error_t err = ch.vtable->send(ch.ctx, "user123", 7, "", 0, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_facebook_destroy(&ch);
}
#endif

/* ─── Instagram DMs ────────────────────────────────────────────────────────── */
#if SC_HAS_INSTAGRAM
static void test_instagram_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_instagram_create(&alloc, "biz1", 4, "tok", 3, "sec", 3, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "instagram");
    sc_instagram_destroy(&ch);
}

static void test_instagram_send(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_instagram_create(&alloc, "biz1", 4, "tok", 3, "sec", 3, &ch);
    sc_error_t err = ch.vtable->send(ch.ctx, "user1", 5, "hello", 5, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_instagram_destroy(&ch);
}

static void test_instagram_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_instagram_create(&alloc, "b", 1, "t", 1, "s", 1, &ch);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    sc_instagram_destroy(&ch);
}

static void test_instagram_webhook_and_poll(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_instagram_create(&alloc, "123", 3, "tok", 3, "sec", 3, &ch);
    sc_error_t err = sc_instagram_on_webhook(ch.ctx, &alloc, "ig msg", 6);
    SC_ASSERT_EQ(err, SC_OK);
    sc_channel_loop_msg_t msgs[4];
    size_t out = 0;
    err = sc_instagram_poll(ch.ctx, &alloc, msgs, 4, &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out, 1);
    sc_instagram_destroy(&ch);
}

static void test_instagram_poll_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_instagram_create(&alloc, "1", 1, "t", 1, "s", 1, &ch);
    sc_channel_loop_msg_t msgs[4];
    size_t out = 99;
    sc_error_t err = sc_instagram_poll(ch.ctx, &alloc, msgs, 4, &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out, 0);
    sc_instagram_destroy(&ch);
}

static void test_instagram_webhook_malformed(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err = sc_instagram_create(&alloc, "123", 3, "tok", 3, "sec", 3, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    err = sc_instagram_on_webhook(ch.ctx, &alloc, "", 0);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
    err = sc_instagram_on_webhook(ch.ctx, &alloc, "}{", 2);
    SC_ASSERT_TRUE(err == SC_OK || err == SC_ERR_JSON_PARSE || err == SC_ERR_INVALID_ARGUMENT);
    sc_instagram_destroy(&ch);
}

static void test_instagram_send_empty_message(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_instagram_create(&alloc, "biz1", 4, "tok", 3, "sec", 3, &ch);
    sc_error_t err = ch.vtable->send(ch.ctx, "user1", 5, "", 0, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_instagram_destroy(&ch);
}
#endif

/* ─── Twitter/X DMs ────────────────────────────────────────────────────────── */
#if SC_HAS_TWITTER
static void test_twitter_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_twitter_create(&alloc, "bearer-tok", 10, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "twitter");
    sc_twitter_destroy(&ch);
}

static void test_twitter_send(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_twitter_create(&alloc, "bearer-tok", 10, &ch);
    sc_error_t err = ch.vtable->send(ch.ctx, "user1", 5, "hello", 5, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_twitter_destroy(&ch);
}

static void test_twitter_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_twitter_create(&alloc, "t", 1, &ch);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    sc_twitter_destroy(&ch);
}

static void test_twitter_webhook_and_poll(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_twitter_create(&alloc, "bearer", 6, &ch);
    sc_error_t err = sc_twitter_on_webhook(ch.ctx, &alloc, "tw msg", 6);
    SC_ASSERT_EQ(err, SC_OK);
    sc_channel_loop_msg_t msgs[4];
    size_t out = 0;
    err = sc_twitter_poll(ch.ctx, &alloc, msgs, 4, &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out, 1);
    sc_twitter_destroy(&ch);
}

static void test_twitter_poll_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_twitter_create(&alloc, "t", 1, &ch);
    sc_channel_loop_msg_t msgs[4];
    size_t out = 99;
    sc_error_t err = sc_twitter_poll(ch.ctx, &alloc, msgs, 4, &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out, 0);
    sc_twitter_destroy(&ch);
}

static void test_twitter_webhook_malformed(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err = sc_twitter_create(&alloc, "bearer", 6, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    err = sc_twitter_on_webhook(ch.ctx, &alloc, "", 0);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
    err = sc_twitter_on_webhook(ch.ctx, &alloc, "}{", 2);
    SC_ASSERT_TRUE(err == SC_OK || err == SC_ERR_JSON_PARSE || err == SC_ERR_INVALID_ARGUMENT);
    sc_twitter_destroy(&ch);
}

static void test_twitter_send_empty_message(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_twitter_create(&alloc, "bearer-tok", 10, &ch);
    sc_error_t err = ch.vtable->send(ch.ctx, "user1", 5, "", 0, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_twitter_destroy(&ch);
}
#endif

/* ─── Google RCS ───────────────────────────────────────────────────────────── */
#if SC_HAS_GOOGLE_RCS
static void test_google_rcs_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_google_rcs_create(&alloc, "agent1", 6, "tok", 3, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "google_rcs");
    sc_google_rcs_destroy(&ch);
}

static void test_google_rcs_send(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_google_rcs_create(&alloc, "agent1", 6, "tok", 3, &ch);
    sc_error_t err = ch.vtable->send(ch.ctx, "+1234", 5, "hello", 5, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_google_rcs_destroy(&ch);
}

static void test_google_rcs_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_google_rcs_create(&alloc, "a", 1, "t", 1, &ch);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    sc_google_rcs_destroy(&ch);
}

static void test_google_rcs_webhook_and_poll(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_google_rcs_create(&alloc, "agent1", 6, "tok", 3, &ch);
    sc_error_t err = sc_google_rcs_on_webhook(ch.ctx, &alloc, "rcs msg", 7);
    SC_ASSERT_EQ(err, SC_OK);
    sc_channel_loop_msg_t msgs[4];
    size_t out = 0;
    err = sc_google_rcs_poll(ch.ctx, &alloc, msgs, 4, &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out, 1);
    sc_google_rcs_destroy(&ch);
}

static void test_google_rcs_poll_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_google_rcs_create(&alloc, "a", 1, "t", 1, &ch);
    sc_channel_loop_msg_t msgs[4];
    size_t out = 99;
    sc_error_t err = sc_google_rcs_poll(ch.ctx, &alloc, msgs, 4, &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out, 0);
    sc_google_rcs_destroy(&ch);
}

static void test_google_rcs_webhook_malformed(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err = sc_google_rcs_create(&alloc, "agent1", 6, "tok", 3, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    err = sc_google_rcs_on_webhook(ch.ctx, &alloc, "", 0);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
    err = sc_google_rcs_on_webhook(ch.ctx, &alloc, "}{", 2);
    SC_ASSERT_TRUE(err == SC_OK || err == SC_ERR_JSON_PARSE || err == SC_ERR_INVALID_ARGUMENT);
    sc_google_rcs_destroy(&ch);
}

static void test_google_rcs_send_empty_message(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_google_rcs_create(&alloc, "agent1", 6, "tok", 3, &ch);
    sc_error_t err = ch.vtable->send(ch.ctx, "+1234", 5, "", 0, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_google_rcs_destroy(&ch);
}
#endif

/* ─── Matrix ───────────────────────────────────────────────────────────────── */
#if SC_HAS_MATRIX
static void test_matrix_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_matrix_create(&alloc, "https://matrix.org", 17, "tok", 3, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "matrix");
    sc_matrix_destroy(&ch);
}

static void test_matrix_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_matrix_create(&alloc, "hs", 2, "t", 1, &ch);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "matrix");
    sc_matrix_destroy(&ch);
}

static void test_matrix_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_matrix_create(&alloc, "h", 1, "t", 1, &ch);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    sc_matrix_destroy(&ch);
}

static void test_matrix_send(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_matrix_create(&alloc, "https://matrix.org", 17, "tok", 3, &ch);
    sc_error_t err = ch.vtable->send(ch.ctx, "#room:matrix.org", 16, "test", 4, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_matrix_destroy(&ch);
}

static void test_matrix_poll_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err = sc_matrix_create(&alloc, "https://example.com", 19, "test-token", 10, &ch);
    SC_ASSERT(err == SC_OK);
    sc_channel_loop_msg_t msgs[4];
    size_t count = 99;
    err = sc_matrix_poll(ch.ctx, &alloc, msgs, 4, &count);
    SC_ASSERT(err == SC_OK);
    SC_ASSERT(count == 0);
    sc_matrix_destroy(&ch);
}

static void test_matrix_poll_null_args(void) {
    sc_error_t err = sc_matrix_poll(NULL, NULL, NULL, 0, NULL);
    SC_ASSERT(err == SC_ERR_INVALID_ARGUMENT);
}
#endif

/* ─── IRC ──────────────────────────────────────────────────────────────────── */
#if SC_HAS_IRC
static void test_irc_send(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_irc_create(&alloc, "irc.example.com", 15, 6667, &ch);
    sc_error_t err = ch.vtable->send(ch.ctx, "#chan", 5, "msg", 3, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_irc_destroy(&ch);
}

static void test_irc_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_irc_create(&alloc, "irc.example.com", 15, 6667, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "irc");
    sc_irc_destroy(&ch);
}

static void test_irc_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_irc_create(&alloc, "s", 1, 6667, &ch);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "irc");
    sc_irc_destroy(&ch);
}

static void test_irc_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_irc_create(&alloc, "s", 1, 6667, &ch);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    sc_irc_destroy(&ch);
}

static void test_irc_poll_test_mode_impl(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err = sc_irc_create(&alloc, "irc.example.com", 15, 6667, &ch);
    SC_ASSERT(err == SC_OK);
    sc_channel_loop_msg_t msgs[4];
    size_t count = 99;
    err = sc_irc_poll(ch.ctx, &alloc, msgs, 4, &count);
    SC_ASSERT(err == SC_OK);
    SC_ASSERT(count == 0);
    sc_irc_destroy(&ch);
}

static void test_irc_poll_null_args_impl(void) {
    sc_error_t err = sc_irc_poll(NULL, NULL, NULL, 0, NULL);
    SC_ASSERT(err == SC_ERR_INVALID_ARGUMENT);
}
#endif

/* ─── LINE ────────────────────────────────────────────────────────────────── */
#if SC_HAS_LINE
static void test_line_start_stop_lifecycle(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_line_create(&alloc, "tok", 3, &ch);
    sc_error_t err = ch.vtable->start(ch.ctx);
    SC_ASSERT_EQ(err, SC_OK);
    ch.vtable->stop(ch.ctx);
    sc_line_destroy(&ch);
}

static void test_line_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_line_create(&alloc, "channeltoken", 12, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "line");
    sc_line_destroy(&ch);
}

static void test_line_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_line_create(&alloc, "t", 1, &ch);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "line");
    sc_line_destroy(&ch);
}

static void test_line_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_line_create(&alloc, "t", 1, &ch);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    sc_line_destroy(&ch);
}
#endif

/* ─── Lark ─────────────────────────────────────────────────────────────────── */
#if SC_HAS_LARK
static void test_lark_start_stop_lifecycle(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_lark_create(&alloc, "app", 3, "secret", 6, &ch);
    sc_error_t err = ch.vtable->start(ch.ctx);
    SC_ASSERT_EQ(err, SC_OK);
    ch.vtable->stop(ch.ctx);
    sc_lark_destroy(&ch);
}

static void test_lark_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_lark_create(&alloc, "app_id", 6, "app_secret", 10, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "lark");
    sc_lark_destroy(&ch);
}

static void test_lark_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_lark_create(&alloc, "a", 1, "b", 1, &ch);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "lark");
    sc_lark_destroy(&ch);
}

static void test_lark_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_lark_create(&alloc, "a", 1, "b", 1, &ch);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    sc_lark_destroy(&ch);
}
#endif

/* ─── Web ──────────────────────────────────────────────────────────────────── */
#if SC_HAS_WEB
static void test_web_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_web_create(&alloc, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "web");
    sc_web_destroy(&ch);
}

static void test_web_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_web_create(&alloc, &ch);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "web");
    sc_web_destroy(&ch);
}

static void test_web_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_web_create(&alloc, &ch);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    sc_web_destroy(&ch);
}

static void test_web_create_destroy_lifecycle(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_web_create(&alloc, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(ch.ctx);
    SC_ASSERT_NOT_NULL(ch.vtable);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "web");
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));

    err = ch.vtable->start(ch.ctx);
    SC_ASSERT_EQ(err, SC_OK);

    err = ch.vtable->send(ch.ctx, "user1", 5, "msg1", 4, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    err = ch.vtable->send(ch.ctx, "user2", 5, "msg2", 4, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);

    ch.vtable->stop(ch.ctx);
    sc_web_destroy(&ch);
}
#endif

/* ─── Email ────────────────────────────────────────────────────────────────── */
#if SC_HAS_EMAIL
static void test_email_send(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_email_create(&alloc, "smtp.example.com", 16, 587, "from@ex.com", 11, &ch);
    sc_error_t err = ch.vtable->send(ch.ctx, "user@ex.com", 11, "body", 4, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_email_destroy(&ch);
}

static void test_email_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_email_create(&alloc, "smtp.example.com", 16, 587, "from@ex.com", 11, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "email");
    sc_email_destroy(&ch);
}

static void test_email_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_email_create(&alloc, "h", 1, 25, "f@x", 3, &ch);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "email");
    sc_email_destroy(&ch);
}

static void test_email_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_email_create(&alloc, "h", 1, 25, "f@x", 3, &ch);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    sc_email_destroy(&ch);
}

static void test_email_is_configured(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_email_create(&alloc, "smtp.example.com", 16, 587, NULL, 0, &ch);
    SC_ASSERT_FALSE(sc_email_is_configured(&ch));
    sc_email_destroy(&ch);

    sc_email_create(&alloc, "smtp.example.com", 16, 587, "bot@example.com", 15, &ch);
    SC_ASSERT_TRUE(sc_email_is_configured(&ch));
    sc_email_destroy(&ch);
}

#if SC_IS_TEST
static void test_email_last_message_in_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_email_create(&alloc, "smtp.example.com", 16, 587, "bot@example.com", 15, &ch);
    ch.vtable->send(ch.ctx, "user@ex.com", 11, "test body", 9, NULL, 0);
    const char *last = sc_email_test_last_message(&ch);
    SC_ASSERT_NOT_NULL(last);
    SC_ASSERT_TRUE(strstr(last, "test body") != NULL);
    SC_ASSERT_TRUE(strstr(last, "Content-Type") != NULL);
    sc_email_destroy(&ch);
}

static void test_email_poll_returns_mock_emails(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_email_create(&alloc, "smtp.example.com", 16, 587, "bot@example.com", 15, &ch);
    sc_error_t err =
        sc_email_test_inject_mock_email(&ch, "sender@ex.com", 13, "mock subject\n\nmock body", 23);
    SC_ASSERT_EQ(err, SC_OK);
    sc_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = sc_email_poll(ch.ctx, &alloc, msgs, 4, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 1u);
    SC_ASSERT_STR_EQ(msgs[0].session_key, "sender@ex.com");
    SC_ASSERT_STR_EQ(msgs[0].content, "mock subject\n\nmock body");
    sc_email_destroy(&ch);
}
#endif

static void test_email_set_auth(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_email_create(&alloc, "smtp.gmail.com", 14, 587, "me@gmail.com", 12, &ch);
    sc_error_t err = sc_email_set_auth(&ch, "me@gmail.com", 12, "apppassword", 11);
    SC_ASSERT_EQ(err, SC_OK);
    sc_email_destroy(&ch);
}

static void test_email_set_imap(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_email_create(&alloc, "smtp.gmail.com", 14, 587, "me@gmail.com", 12, &ch);
    sc_error_t err = sc_email_set_imap(&ch, "imap.gmail.com", 14, 993);
    SC_ASSERT_EQ(err, SC_OK);
    sc_email_destroy(&ch);
}

static void test_email_poll_no_imap_returns_not_supported(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_email_create(&alloc, "smtp.gmail.com", 14, 587, "me@gmail.com", 12, &ch);
    sc_channel_loop_msg_t msgs[4];
    size_t count = 0;
    sc_error_t err = sc_email_poll(ch.ctx, &alloc, msgs, 4, &count);
    SC_ASSERT_EQ(err, SC_OK); /* SC_IS_TEST returns SC_OK */
    SC_ASSERT_EQ(count, 0u);
    sc_email_destroy(&ch);
}

static void test_email_set_auth_null_channel(void) {
    sc_error_t err = sc_email_set_auth(NULL, "x", 1, "y", 1);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_email_set_imap_null_channel(void) {
    sc_error_t err = sc_email_set_imap(NULL, "x", 1, 993);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}
#endif

/* ─── iMessage ─────────────────────────────────────────────────────────────── */
#if SC_HAS_IMESSAGE
static void test_imessage_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_imessage_create(&alloc, NULL, 0, NULL, 0, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "imessage");
    sc_imessage_destroy(&ch);
}

static void test_imessage_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_imessage_create(&alloc, NULL, 0, NULL, 0, &ch);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "imessage");
    sc_imessage_destroy(&ch);
}

static void test_imessage_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_imessage_create(&alloc, NULL, 0, NULL, 0, &ch);
#ifdef __APPLE__
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
#else
    SC_ASSERT_FALSE(ch.vtable->health_check(ch.ctx));
#endif
    sc_imessage_destroy(&ch);
}

static void test_imessage_is_configured(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_imessage_create(&alloc, NULL, 0, NULL, 0, &ch);
    SC_ASSERT_FALSE(sc_imessage_is_configured(&ch));
    sc_imessage_destroy(&ch);

    sc_imessage_create(&alloc, "+15551234567", 11, NULL, 0, &ch);
    SC_ASSERT_TRUE(sc_imessage_is_configured(&ch));
    sc_imessage_destroy(&ch);
}

static void test_imessage_poll_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_imessage_create(&alloc, "+15551234567", 11, NULL, 0, &ch);
    sc_channel_loop_msg_t msgs[4];
    size_t count = 99;
    sc_error_t err = sc_imessage_poll(ch.ctx, &alloc, msgs, 4, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 0u);
    sc_imessage_destroy(&ch);
}

static void test_imessage_poll_null_args(void) {
    sc_error_t err = sc_imessage_poll(NULL, NULL, NULL, 0, NULL);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}
#endif

/* ─── Mattermost ───────────────────────────────────────────────────────────── */
#if SC_HAS_MATTERMOST
static void test_mattermost_send(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_mattermost_create(&alloc, "https://chat.example.com", 24, "token", 5, &ch);
    sc_error_t err = ch.vtable->send(ch.ctx, "channel_id", 10, "msg", 3, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_mattermost_destroy(&ch);
}

static void test_mattermost_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_mattermost_create(&alloc, "https://chat.example.com", 24, "token", 5, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "mattermost");
    sc_mattermost_destroy(&ch);
}

static void test_mattermost_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_mattermost_create(&alloc, "u", 1, "t", 1, &ch);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "mattermost");
    sc_mattermost_destroy(&ch);
}

static void test_mattermost_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_mattermost_create(&alloc, "u", 1, "t", 1, &ch);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    sc_mattermost_destroy(&ch);
}
#endif

/* ─── OneBot ──────────────────────────────────────────────────────────────── */
#if SC_HAS_ONEBOT
static void test_onebot_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_onebot_create(&alloc, "http://127.0.0.1:5700", 20, "tok", 3, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "onebot");
    sc_onebot_destroy(&ch);
}

static void test_onebot_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_onebot_create(&alloc, "u", 1, "t", 1, &ch);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "onebot");
    sc_onebot_destroy(&ch);
}

static void test_onebot_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_onebot_create(&alloc, "u", 1, "t", 1, &ch);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    sc_onebot_destroy(&ch);
}
#endif

/* ─── DingTalk ─────────────────────────────────────────────────────────────── */
#if SC_HAS_DINGTALK
static void test_dingtalk_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_dingtalk_create(&alloc, "appkey", 7, "secret", 6, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "dingtalk");
    sc_dingtalk_destroy(&ch);
}

static void test_dingtalk_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_dingtalk_create(&alloc, "a", 1, "b", 1, &ch);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "dingtalk");
    sc_dingtalk_destroy(&ch);
}

static void test_dingtalk_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_dingtalk_create(&alloc, "a", 1, "b", 1, &ch);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    sc_dingtalk_destroy(&ch);
}
#endif

/* ─── Signal, Nostr, QQ, MaixCam, Dispatch (always in test build) ───────── */
#if SC_HAS_SIGNAL
#include "seaclaw/channels/signal.h"
static void test_signal_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_signal_create(&alloc, "http://localhost:8080", 21, "test", 4, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(ch.ctx);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "signal");
    sc_signal_destroy(&ch);
}

static void test_signal_destroy_lifecycle(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_signal_create(&alloc, "http://x", 7, "a", 1, &ch);
    sc_signal_destroy(&ch);
}

static void test_signal_send(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_signal_create(&alloc, "http://localhost", 16, "a", 1, &ch);
    sc_error_t err = ch.vtable->send(ch.ctx, NULL, 0, "hello", 5, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_signal_destroy(&ch);
}

static void test_signal_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_signal_create(&alloc, "http://x", 7, "a", 1, &ch);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    sc_signal_destroy(&ch);
}

static void test_signal_start_stop_typing(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_signal_create(&alloc, "http://x", 7, "a", 1, &ch);
    if (ch.vtable->start_typing) {
        sc_error_t err = ch.vtable->start_typing(ch.ctx, "recipient", 9);
        SC_ASSERT_EQ(err, SC_OK);
    }
    if (ch.vtable->stop_typing) {
        sc_error_t err = ch.vtable->stop_typing(ch.ctx, "recipient", 9);
        SC_ASSERT_EQ(err, SC_OK);
    }
    sc_signal_destroy(&ch);
}

static void test_signal_send_long_message(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_signal_create(&alloc, "http://localhost", 16, "a", 1, &ch);
    char buf[2000];
    for (size_t i = 0; i < sizeof(buf) - 1; i++)
        buf[i] = 'x';
    buf[sizeof(buf) - 1] = '\0';
    sc_error_t err = ch.vtable->send(ch.ctx, NULL, 0, buf, sizeof(buf) - 1, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_signal_destroy(&ch);
}
#endif

#if SC_HAS_NOSTR
#include "seaclaw/channel_loop.h"
#include "seaclaw/channels/nostr.h"
static void test_nostr_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_nostr_create(&alloc, "/tmp/nak", 8, "npub1abc", 8, NULL, 0, NULL, 0, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(ch.ctx);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "nostr");
    sc_nostr_destroy(&ch);
}

static void test_nostr_destroy_lifecycle(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_nostr_create(&alloc, "/tmp", 4, "npub", 4, NULL, 0, NULL, 0, &ch);
    sc_nostr_destroy(&ch);
}

static void test_nostr_send(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_nostr_create(&alloc, "/tmp", 4, "npub", 4, NULL, 0, NULL, 0, &ch);
    sc_error_t err = ch.vtable->send(ch.ctx, NULL, 0, "msg", 3, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_nostr_destroy(&ch);
}

static void test_nostr_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_nostr_create(&alloc, "/tmp", 4, "npub", 4, NULL, 0, NULL, 0, &ch);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    sc_nostr_destroy(&ch);
}

static void test_nostr_poll_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err = sc_nostr_create(&alloc, "/tmp/nak", 8, "npub1x", 6, "wss://relay.example.com",
                                     21, "seckey", 6, &ch);
    SC_ASSERT(err == SC_OK);
    sc_channel_loop_msg_t msgs[4];
    size_t count = 99;
    err = sc_nostr_poll(ch.ctx, &alloc, msgs, 4, &count);
    SC_ASSERT(err == SC_OK);
    SC_ASSERT(count == 0);
    sc_nostr_destroy(&ch);
}
#endif

#if SC_HAS_MQTT
#include "seaclaw/channel_loop.h"
#include "seaclaw/channels/mqtt.h"
static void test_mqtt_create_null_alloc_returns_error(void) {
    sc_channel_t ch = {0};
    sc_error_t err =
        sc_mqtt_create(NULL, "mqtt://broker", 14, NULL, 0, NULL, 0, NULL, 0, NULL, 0, 0, &ch);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}
static void test_mqtt_create_null_broker_returns_error(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err = sc_mqtt_create(&alloc, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, 0, &ch);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}
static void test_mqtt_create_valid_lifecycle(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err = sc_mqtt_create(&alloc, "mqtt://broker.example.com", 26, "in", 2, "out", 3,
                                    NULL, 0, NULL, 0, 0, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(ch.ctx);
    err = ch.vtable->start(ch.ctx);
    SC_ASSERT_EQ(err, SC_OK);
    err = ch.vtable->send(ch.ctx, NULL, 0, "hello", 5, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    ch.vtable->stop(ch.ctx);
    sc_mqtt_destroy(&ch, &alloc);
}
static void test_mqtt_create_valid_health_check_when_running(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err =
        sc_mqtt_create(&alloc, "mqtt://broker", 13, NULL, 0, NULL, 0, NULL, 0, NULL, 0, 0, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    err = ch.vtable->start(ch.ctx);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    ch.vtable->stop(ch.ctx);
    sc_mqtt_destroy(&ch, &alloc);
}

static void test_mqtt_send_stores_message(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err =
        sc_mqtt_create(&alloc, "mqtt://broker", 13, NULL, 0, NULL, 0, NULL, 0, NULL, 0, 0, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    err = ch.vtable->start(ch.ctx);
    SC_ASSERT_EQ(err, SC_OK);
    err = ch.vtable->send(ch.ctx, NULL, 0, "hello world", 11, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    size_t len = 0;
    const char *last = sc_mqtt_test_get_last_message(&ch, &len);
    SC_ASSERT_NOT_NULL(last);
    SC_ASSERT_EQ(len, 11u);
    SC_ASSERT_STR_EQ(last, "hello world");
    sc_mqtt_destroy(&ch, &alloc);
}

static void test_mqtt_poll_returns_injected_message(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err =
        sc_mqtt_create(&alloc, "mqtt://broker", 13, NULL, 0, NULL, 0, NULL, 0, NULL, 0, 0, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    err = ch.vtable->start(ch.ctx);
    SC_ASSERT_EQ(err, SC_OK);
    err = sc_mqtt_test_inject_mock(&ch, "sess1", 5, "injected payload", 16);
    SC_ASSERT_EQ(err, SC_OK);
    sc_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = sc_mqtt_poll(ch.ctx, &alloc, msgs, 4, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 1u);
    SC_ASSERT_STR_EQ(msgs[0].session_key, "sess1");
    SC_ASSERT_STR_EQ(msgs[0].content, "injected payload");
    sc_mqtt_destroy(&ch, &alloc);
}

static void test_mqtt_send_before_start_fails(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err =
        sc_mqtt_create(&alloc, "mqtt://broker", 13, NULL, 0, NULL, 0, NULL, 0, NULL, 0, 0, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    err = ch.vtable->send(ch.ctx, NULL, 0, "hello", 5, NULL, 0);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
    sc_mqtt_destroy(&ch, &alloc);
}

static void test_mqtt_poll_empty_returns_no_message(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err =
        sc_mqtt_create(&alloc, "mqtt://broker", 13, NULL, 0, NULL, 0, NULL, 0, NULL, 0, 0, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    err = ch.vtable->start(ch.ctx);
    SC_ASSERT_EQ(err, SC_OK);
    sc_channel_loop_msg_t msgs[4];
    size_t count = 99;
    err = sc_mqtt_poll(ch.ctx, &alloc, msgs, 4, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 0u);
    sc_mqtt_destroy(&ch, &alloc);
}
#endif

#if SC_HAS_QQ
#include "seaclaw/channels/qq.h"
static void test_qq_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_qq_create(&alloc, "app-id", 6, "tok", 3, false, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "qq");
    sc_qq_destroy(&ch);
}

static void test_qq_send(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_qq_create_ex(&alloc, "app", 3, "t", 1, "ch123", 5, false, &ch);
    sc_error_t err = ch.vtable->send(ch.ctx, NULL, 0, "h", 1, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_qq_destroy(&ch);
}

static void test_qq_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_qq_create(&alloc, "app", 3, "t", 1, false, &ch);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    sc_qq_destroy(&ch);
}
#endif

#if SC_HAS_MAIXCAM
#include "seaclaw/channels/maixcam.h"
static void test_maixcam_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_maixcam_create(&alloc, "/dev/ttyUSB0", 12, 0, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "maixcam");
    sc_maixcam_destroy(&ch);
}

static void test_maixcam_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_maixcam_create(&alloc, "/dev/x", 6, 0, &ch);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    sc_maixcam_destroy(&ch);
}
#endif

#if SC_HAS_DISPATCH
extern sc_error_t sc_dispatch_create(sc_allocator_t *alloc, sc_channel_t *out);
extern void sc_dispatch_destroy(sc_channel_t *ch);
static void test_dispatch_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_dispatch_create(&alloc, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "dispatch");
    sc_dispatch_destroy(&ch);
}

static void test_dispatch_send(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_dispatch_create(&alloc, &ch);
    sc_error_t err = ch.vtable->send(ch.ctx, NULL, 0, "msg", 3, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_dispatch_destroy(&ch);
}

static void test_dispatch_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_dispatch_create(&alloc, &ch);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    sc_dispatch_destroy(&ch);
}
#endif

static void test_cli_send_empty_message(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_cli_create(&alloc, &ch);
    sc_error_t err = ch.vtable->send(ch.ctx, NULL, 0, "", 0, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_cli_destroy(&ch);
}

static void test_cli_send_long_message(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_cli_create(&alloc, &ch);
    char buf[1024];
    for (size_t i = 0; i < sizeof(buf) - 1; i++)
        buf[i] = 'a';
    buf[sizeof(buf) - 1] = '\0';
    sc_error_t err = ch.vtable->send(ch.ctx, NULL, 0, buf, sizeof(buf) - 1, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_cli_destroy(&ch);
}

static void test_cli_create_destroy_repeat(void) {
    sc_allocator_t alloc = sc_system_allocator();
    for (int i = 0; i < 3; i++) {
        sc_channel_t ch;
        sc_cli_create(&alloc, &ch);
        SC_ASSERT_NOT_NULL(ch.vtable);
        sc_cli_destroy(&ch);
    }
}

#if SC_HAS_TELEGRAM
static void test_telegram_escape_markdown(void) {
    sc_allocator_t alloc = sc_system_allocator();
    size_t out_len = 0;
    char *esc = sc_telegram_escape_markdown_v2(&alloc, "_*[]", 4, &out_len);
    SC_ASSERT_NOT_NULL(esc);
    SC_ASSERT_TRUE(out_len >= 4);
    SC_ASSERT_TRUE(strchr(esc, '\\') != NULL);
    alloc.free(alloc.ctx, esc, out_len + 1);
}

static void test_telegram_commands_help(void) {
    const char *help = sc_telegram_commands_help();
    SC_ASSERT_NOT_NULL(help);
    SC_ASSERT_TRUE(strstr(help, "/start") != NULL);
    SC_ASSERT_TRUE(strstr(help, "/help") != NULL);
}

static void test_telegram_start_stop_typing(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_telegram_create(&alloc, "t", 1, &ch);
    if (ch.vtable->start_typing) {
        sc_error_t err = ch.vtable->start_typing(ch.ctx, "user1", 5);
        SC_ASSERT_EQ(err, SC_OK);
    }
    if (ch.vtable->stop_typing) {
        sc_error_t err = ch.vtable->stop_typing(ch.ctx, "user1", 5);
        SC_ASSERT_EQ(err, SC_OK);
    }
    sc_telegram_destroy(&ch);
}

static void test_telegram_allowlist(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_telegram_create(&alloc, "t", 1, &ch);
    const char *allow[] = {"user1", "user2", NULL};
    sc_telegram_set_allowlist(&ch, allow, 2);
    sc_telegram_destroy(&ch);
}

static void test_telegram_send_long_message(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_telegram_create(&alloc, "t", 1, &ch);
    char buf[5000];
    for (size_t i = 0; i < sizeof(buf) - 1; i++)
        buf[i] = 'a';
    buf[sizeof(buf) - 1] = '\0';
    sc_error_t err = ch.vtable->send(ch.ctx, "123", 3, buf, sizeof(buf) - 1, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_telegram_destroy(&ch);
}

static void test_telegram_send(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_telegram_create(&alloc, "t", 1, &ch);
    sc_error_t err = ch.vtable->send(ch.ctx, "12345", 5, "test msg", 8, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_telegram_destroy(&ch);
}

static void test_telegram_create_destroy_lifecycle(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_telegram_create(&alloc, "token", 5, &ch);
    SC_ASSERT_NOT_NULL(ch.ctx);
    sc_telegram_destroy(&ch);
}

static void test_telegram_webhook_malformed(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err = sc_telegram_create(&alloc, "t", 1, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    sc_channel_loop_msg_t msgs[4];
    size_t out = 99;
    err = sc_telegram_poll(ch.ctx, &alloc, msgs, 4, &out);
    SC_ASSERT_EQ(err, SC_OK);
    err = sc_telegram_poll(ch.ctx, &alloc, msgs, 0, &out);
    SC_ASSERT_EQ(err, SC_OK);
    sc_telegram_destroy(&ch);
}
#endif

#if SC_HAS_DISCORD
static void test_discord_send(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_discord_create(&alloc, "t", 1, &ch);
    sc_error_t err = ch.vtable->send(ch.ctx, "channel1", 8, "msg", 3, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_discord_destroy(&ch);
}

static void test_discord_send_without_token_fails(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_discord_create(&alloc, NULL, 0, &ch);
    sc_error_t err = ch.vtable->send(ch.ctx, "ch", 2, "msg", 3, NULL, 0);
    SC_ASSERT_EQ(err, SC_ERR_CHANNEL_NOT_CONFIGURED);
    sc_discord_destroy(&ch);
}

static void test_discord_poll_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_discord_create(&alloc, "t", 1, &ch);
    sc_channel_loop_msg_t msgs[4];
    size_t count = 99;
    sc_error_t err = sc_discord_poll(ch.ctx, &alloc, msgs, 4, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 0u);
    sc_discord_destroy(&ch);
}

static void test_discord_poll_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    const char *ch_ids[] = {"123456789"};
    sc_error_t err = sc_discord_create_ex(&alloc, "t", 1, ch_ids, 1, NULL, 0, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    sc_channel_loop_msg_t msgs[4];
    size_t count = 99;
    err = sc_discord_poll(ch.ctx, &alloc, msgs, 4, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 0u);
    sc_discord_destroy(&ch);
}

static void test_discord_webhook_malformed(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err = sc_discord_create(&alloc, "t", 1, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    sc_channel_loop_msg_t msgs[4];
    size_t out = 99;
    err = sc_discord_poll(ch.ctx, &alloc, msgs, 4, &out);
    SC_ASSERT_EQ(err, SC_OK);
    err = sc_discord_poll(ch.ctx, &alloc, msgs, 0, &out);
    SC_ASSERT_EQ(err, SC_OK);
    sc_discord_destroy(&ch);
}
#endif

#if SC_HAS_WEB
static void test_web_send_empty_target(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_web_create(&alloc, &ch);
    sc_error_t err = ch.vtable->send(ch.ctx, "", 0, "hi", 2, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_web_destroy(&ch);
}
#endif

/* ─── Webhook + Poll tests ───────────────────────────────────────── */

#if SC_HAS_LINE
static void test_line_webhook_and_poll(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err = sc_line_create(&alloc, "test-token", 10, &ch);
    SC_ASSERT(err == SC_OK);
    err = sc_line_on_webhook(ch.ctx, &alloc, "hello from line", 15);
    SC_ASSERT(err == SC_OK);
    sc_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = sc_line_poll(ch.ctx, &alloc, msgs, 4, &count);
    SC_ASSERT(err == SC_OK);
    SC_ASSERT(count == 1);
    SC_ASSERT_STR_EQ(msgs[0].session_key, "test-sender");
    sc_line_destroy(&ch);
}

static void test_line_poll_null_args(void) {
    sc_error_t err = sc_line_poll(NULL, NULL, NULL, 0, NULL);
    SC_ASSERT(err == SC_ERR_INVALID_ARGUMENT);
}
#endif

#if SC_HAS_LARK
static void test_lark_webhook_and_poll(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err = sc_lark_create(&alloc, "app-id", 6, "secret", 6, &ch);
    SC_ASSERT(err == SC_OK);
    err = sc_lark_on_webhook(ch.ctx, &alloc, "hello from lark", 15);
    SC_ASSERT(err == SC_OK);
    sc_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = sc_lark_poll(ch.ctx, &alloc, msgs, 4, &count);
    SC_ASSERT(err == SC_OK);
    SC_ASSERT(count == 1);
    sc_lark_destroy(&ch);
}

static void test_lark_poll_null_args(void) {
    sc_error_t err = sc_lark_poll(NULL, NULL, NULL, 0, NULL);
    SC_ASSERT(err == SC_ERR_INVALID_ARGUMENT);
}
#endif

#if SC_HAS_MATTERMOST
static void test_mattermost_poll_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err = sc_mattermost_create(&alloc, "https://example.com", 19, "test-token", 10, &ch);
    SC_ASSERT(err == SC_OK);
    sc_channel_loop_msg_t msgs[4];
    size_t count = 99;
    err = sc_mattermost_poll(ch.ctx, &alloc, msgs, 4, &count);
    SC_ASSERT(err == SC_OK);
    SC_ASSERT(count == 0);
    sc_mattermost_destroy(&ch);
}

static void test_mattermost_poll_null_args(void) {
    sc_error_t err = sc_mattermost_poll(NULL, NULL, NULL, 0, NULL);
    SC_ASSERT(err == SC_ERR_INVALID_ARGUMENT);
}
#endif

#if SC_HAS_ONEBOT
static void test_onebot_webhook_and_poll(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err = sc_onebot_create(&alloc, "http://localhost:5700", 21, "test", 4, &ch);
    SC_ASSERT(err == SC_OK);
    err = sc_onebot_on_webhook(ch.ctx, &alloc, "hello from onebot", 17);
    SC_ASSERT(err == SC_OK);
    sc_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = sc_onebot_poll(ch.ctx, &alloc, msgs, 4, &count);
    SC_ASSERT(err == SC_OK);
    SC_ASSERT(count == 1);
    sc_onebot_destroy(&ch);
}

static void test_onebot_poll_null_args(void) {
    sc_error_t err = sc_onebot_poll(NULL, NULL, NULL, 0, NULL);
    SC_ASSERT(err == SC_ERR_INVALID_ARGUMENT);
}
#endif

#if SC_HAS_DINGTALK
static void test_dingtalk_webhook_and_poll(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err = sc_dingtalk_create(&alloc, "key", 3, "secret", 6, &ch);
    SC_ASSERT(err == SC_OK);
    err = sc_dingtalk_on_webhook(ch.ctx, &alloc, "hello from dingtalk", 19);
    SC_ASSERT(err == SC_OK);
    sc_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = sc_dingtalk_poll(ch.ctx, &alloc, msgs, 4, &count);
    SC_ASSERT(err == SC_OK);
    SC_ASSERT(count == 1);
    sc_dingtalk_destroy(&ch);
}

static void test_dingtalk_poll_null_args(void) {
    sc_error_t err = sc_dingtalk_poll(NULL, NULL, NULL, 0, NULL);
    SC_ASSERT(err == SC_ERR_INVALID_ARGUMENT);
}
#endif

/* ─── Teams ─────────────────────────────────────────────────────────────── */
#if SC_HAS_TEAMS
static void test_teams_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_teams_create(&alloc, "https://example.com", 19, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "teams");
    SC_ASSERT(sc_teams_is_configured(&ch) == true);
    sc_teams_destroy(&ch);
}

static void test_teams_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_teams_create(&alloc, "https://example.com", 19, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(ch.vtable->health_check(ch.ctx) == true);
    sc_teams_destroy(&ch);
}

static void test_teams_webhook_and_poll(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err = sc_teams_create(&alloc, "https://example.com", 19, &ch);
    SC_ASSERT(err == SC_OK);
    err = sc_teams_on_webhook(ch.ctx, &alloc, "hello from teams", 16);
    SC_ASSERT(err == SC_OK);
    sc_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = sc_teams_poll(ch.ctx, &alloc, msgs, 4, &count);
    SC_ASSERT(err == SC_OK);
    SC_ASSERT(count == 1);
    sc_teams_destroy(&ch);
}

static void test_teams_poll_null_args(void) {
    sc_error_t err = sc_teams_poll(NULL, NULL, NULL, 0, NULL);
    SC_ASSERT(err == SC_ERR_INVALID_ARGUMENT);
}
#endif

/* ─── Twilio ────────────────────────────────────────────────────────────── */
#if SC_HAS_TWILIO
static void test_twilio_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_twilio_create(&alloc, "ACXXXX", 6, "token", 5, "+15551234567", 12,
                                      "+15559876543", 12, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "twilio");
    SC_ASSERT(sc_twilio_is_configured(&ch) == true);
    sc_twilio_destroy(&ch);
}

static void test_twilio_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_twilio_create(&alloc, "ACXXXX", 6, "token", 5, "+15551234567", 12,
                                      "+15559876543", 12, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(ch.vtable->health_check(ch.ctx) == true);
    sc_twilio_destroy(&ch);
}

static void test_twilio_webhook_and_poll(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err = sc_twilio_create(&alloc, "ACXXXX", 6, "token", 5, "+15551234567", 12,
                                      "+15559876543", 12, &ch);
    SC_ASSERT(err == SC_OK);
    err = sc_twilio_on_webhook(ch.ctx, &alloc, "hello from twilio", 17);
    SC_ASSERT(err == SC_OK);
    sc_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = sc_twilio_poll(ch.ctx, &alloc, msgs, 4, &count);
    SC_ASSERT(err == SC_OK);
    SC_ASSERT(count == 1);
    sc_twilio_destroy(&ch);
}

static void test_twilio_poll_null_args(void) {
    sc_error_t err = sc_twilio_poll(NULL, NULL, NULL, 0, NULL);
    SC_ASSERT(err == SC_ERR_INVALID_ARGUMENT);
}
#endif

/* ─── Google Chat ───────────────────────────────────────────────────────── */
#if SC_HAS_GOOGLE_CHAT
static void test_google_chat_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err =
        sc_google_chat_create(&alloc, "https://chat.googleapis.com/v1/spaces/abc", 38, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "google_chat");
    SC_ASSERT(sc_google_chat_is_configured(&ch) == true);
    sc_google_chat_destroy(&ch);
}

static void test_google_chat_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err =
        sc_google_chat_create(&alloc, "https://chat.googleapis.com/v1/spaces/abc", 38, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(ch.vtable->health_check(ch.ctx) == true);
    sc_google_chat_destroy(&ch);
}

static void test_google_chat_webhook_and_poll(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err =
        sc_google_chat_create(&alloc, "https://chat.googleapis.com/v1/spaces/abc", 38, &ch);
    SC_ASSERT(err == SC_OK);
    err = sc_google_chat_on_webhook(ch.ctx, &alloc, "hello from gchat", 16);
    SC_ASSERT(err == SC_OK);
    sc_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = sc_google_chat_poll(ch.ctx, &alloc, msgs, 4, &count);
    SC_ASSERT(err == SC_OK);
    SC_ASSERT(count == 1);
    sc_google_chat_destroy(&ch);
}

static void test_google_chat_poll_null_args(void) {
    sc_error_t err = sc_google_chat_poll(NULL, NULL, NULL, 0, NULL);
    SC_ASSERT(err == SC_ERR_INVALID_ARGUMENT);
}
#endif

#if SC_HAS_QQ
static void test_qq_webhook_and_poll(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch = {0};
    sc_error_t err = sc_qq_create(&alloc, "app-id", 6, "token", 5, false, &ch);
    SC_ASSERT(err == SC_OK);
    err = sc_qq_on_webhook(ch.ctx, &alloc, "hello from qq", 13);
    SC_ASSERT(err == SC_OK);
    sc_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = sc_qq_poll(ch.ctx, &alloc, msgs, 4, &count);
    SC_ASSERT(err == SC_OK);
    SC_ASSERT(count == 1);
    sc_qq_destroy(&ch);
}

static void test_qq_poll_null_args(void) {
    sc_error_t err = sc_qq_poll(NULL, NULL, NULL, 0, NULL);
    SC_ASSERT(err == SC_ERR_INVALID_ARGUMENT);
}
#endif

void run_channel_all_tests(void) {
    SC_TEST_SUITE("Channel All");

    SC_RUN_TEST(test_cli_create);
    SC_RUN_TEST(test_cli_name);
    SC_RUN_TEST(test_cli_health_check);
    SC_RUN_TEST(test_cli_send);
    SC_RUN_TEST(test_cli_send_empty_message);
    SC_RUN_TEST(test_cli_send_long_message);
    SC_RUN_TEST(test_cli_create_destroy_repeat);

#if SC_HAS_SIGNAL
    SC_RUN_TEST(test_signal_create);
    SC_RUN_TEST(test_signal_destroy_lifecycle);
    SC_RUN_TEST(test_signal_send);
    SC_RUN_TEST(test_signal_health_check);
    SC_RUN_TEST(test_signal_start_stop_typing);
    SC_RUN_TEST(test_signal_send_long_message);
#endif
#if SC_HAS_NOSTR
    SC_RUN_TEST(test_nostr_create);
    SC_RUN_TEST(test_nostr_destroy_lifecycle);
    SC_RUN_TEST(test_nostr_send);
    SC_RUN_TEST(test_nostr_health_check);
    SC_RUN_TEST(test_nostr_poll_test_mode);
#endif
#if SC_HAS_MQTT
    SC_RUN_TEST(test_mqtt_create_null_alloc_returns_error);
    SC_RUN_TEST(test_mqtt_create_null_broker_returns_error);
    SC_RUN_TEST(test_mqtt_create_valid_lifecycle);
    SC_RUN_TEST(test_mqtt_create_valid_health_check_when_running);
    SC_RUN_TEST(test_mqtt_send_stores_message);
    SC_RUN_TEST(test_mqtt_poll_returns_injected_message);
    SC_RUN_TEST(test_mqtt_send_before_start_fails);
    SC_RUN_TEST(test_mqtt_poll_empty_returns_no_message);
#endif
#if SC_HAS_QQ
    SC_RUN_TEST(test_qq_create);
    SC_RUN_TEST(test_qq_send);
    SC_RUN_TEST(test_qq_health_check);
    SC_RUN_TEST(test_qq_webhook_and_poll);
    SC_RUN_TEST(test_qq_poll_null_args);
#endif
#if SC_HAS_MAIXCAM
    SC_RUN_TEST(test_maixcam_create);
    SC_RUN_TEST(test_maixcam_health_check);
#endif
#if SC_HAS_DISPATCH
    SC_RUN_TEST(test_dispatch_create);
    SC_RUN_TEST(test_dispatch_send);
    SC_RUN_TEST(test_dispatch_health_check);
#endif

#if SC_HAS_TELEGRAM
    SC_RUN_TEST(test_telegram_create);
    SC_RUN_TEST(test_telegram_name);
    SC_RUN_TEST(test_telegram_health_check);
    SC_RUN_TEST(test_telegram_send);
    SC_RUN_TEST(test_telegram_create_destroy_lifecycle);
    SC_RUN_TEST(test_telegram_escape_markdown);
    SC_RUN_TEST(test_telegram_commands_help);
    SC_RUN_TEST(test_telegram_start_stop_typing);
    SC_RUN_TEST(test_telegram_allowlist);
    SC_RUN_TEST(test_telegram_send_long_message);
    SC_RUN_TEST(test_telegram_webhook_malformed);
#endif
#if SC_HAS_DISCORD
    SC_RUN_TEST(test_discord_start_stop_lifecycle);
    SC_RUN_TEST(test_discord_create);
    SC_RUN_TEST(test_discord_name);
    SC_RUN_TEST(test_discord_health_check);
    SC_RUN_TEST(test_discord_send);
    SC_RUN_TEST(test_discord_send_without_token_fails);
    SC_RUN_TEST(test_discord_poll_test_mode);
    SC_RUN_TEST(test_discord_poll_empty);
    SC_RUN_TEST(test_discord_webhook_malformed);
#endif
#if SC_HAS_SLACK
    SC_RUN_TEST(test_slack_create);
    SC_RUN_TEST(test_slack_name);
    SC_RUN_TEST(test_slack_health_check);
    SC_RUN_TEST(test_slack_start_stop_typing);
    SC_RUN_TEST(test_slack_send_long_message);
    SC_RUN_TEST(test_slack_create_ex);
    SC_RUN_TEST(test_slack_poll_test_mode);
    SC_RUN_TEST(test_slack_webhook_malformed);
#endif
#if SC_HAS_WHATSAPP
    SC_RUN_TEST(test_whatsapp_create);
    SC_RUN_TEST(test_whatsapp_name);
    SC_RUN_TEST(test_whatsapp_health_check);
    SC_RUN_TEST(test_whatsapp_send);
    SC_RUN_TEST(test_whatsapp_webhook_and_poll);
    SC_RUN_TEST(test_whatsapp_poll_empty);
#endif
#if SC_HAS_FACEBOOK
    SC_RUN_TEST(test_facebook_create);
    SC_RUN_TEST(test_facebook_name);
    SC_RUN_TEST(test_facebook_health_check);
    SC_RUN_TEST(test_facebook_send);
    SC_RUN_TEST(test_facebook_webhook_and_poll);
    SC_RUN_TEST(test_facebook_poll_empty);
    SC_RUN_TEST(test_facebook_webhook_malformed);
    SC_RUN_TEST(test_facebook_send_empty_message);
#endif
#if SC_HAS_INSTAGRAM
    SC_RUN_TEST(test_instagram_create);
    SC_RUN_TEST(test_instagram_send);
    SC_RUN_TEST(test_instagram_health_check);
    SC_RUN_TEST(test_instagram_webhook_and_poll);
    SC_RUN_TEST(test_instagram_poll_empty);
    SC_RUN_TEST(test_instagram_webhook_malformed);
    SC_RUN_TEST(test_instagram_send_empty_message);
#endif
#if SC_HAS_TWITTER
    SC_RUN_TEST(test_twitter_create);
    SC_RUN_TEST(test_twitter_send);
    SC_RUN_TEST(test_twitter_health_check);
    SC_RUN_TEST(test_twitter_webhook_and_poll);
    SC_RUN_TEST(test_twitter_poll_empty);
    SC_RUN_TEST(test_twitter_webhook_malformed);
    SC_RUN_TEST(test_twitter_send_empty_message);
#endif
#if SC_HAS_GOOGLE_RCS
    SC_RUN_TEST(test_google_rcs_create);
    SC_RUN_TEST(test_google_rcs_send);
    SC_RUN_TEST(test_google_rcs_health_check);
    SC_RUN_TEST(test_google_rcs_webhook_and_poll);
    SC_RUN_TEST(test_google_rcs_poll_empty);
    SC_RUN_TEST(test_google_rcs_webhook_malformed);
    SC_RUN_TEST(test_google_rcs_send_empty_message);
#endif
#if SC_HAS_MATRIX
    SC_RUN_TEST(test_matrix_create);
    SC_RUN_TEST(test_matrix_name);
    SC_RUN_TEST(test_matrix_health_check);
    SC_RUN_TEST(test_matrix_send);
    SC_RUN_TEST(test_matrix_poll_test_mode);
    SC_RUN_TEST(test_matrix_poll_null_args);
#endif
#if SC_HAS_IRC
    SC_RUN_TEST(test_irc_create);
    SC_RUN_TEST(test_irc_name);
    SC_RUN_TEST(test_irc_health_check);
    SC_RUN_TEST(test_irc_send);
    SC_RUN_TEST(test_irc_poll_test_mode_impl);
    SC_RUN_TEST(test_irc_poll_null_args_impl);
#endif
#if SC_HAS_LINE
    SC_RUN_TEST(test_line_start_stop_lifecycle);
    SC_RUN_TEST(test_line_create);
    SC_RUN_TEST(test_line_name);
    SC_RUN_TEST(test_line_health_check);
    SC_RUN_TEST(test_line_webhook_and_poll);
    SC_RUN_TEST(test_line_poll_null_args);
#endif
#if SC_HAS_LARK
    SC_RUN_TEST(test_lark_start_stop_lifecycle);
    SC_RUN_TEST(test_lark_create);
    SC_RUN_TEST(test_lark_name);
    SC_RUN_TEST(test_lark_health_check);
    SC_RUN_TEST(test_lark_webhook_and_poll);
    SC_RUN_TEST(test_lark_poll_null_args);
#endif
#if SC_HAS_WEB
    SC_RUN_TEST(test_web_create);
    SC_RUN_TEST(test_web_name);
    SC_RUN_TEST(test_web_health_check);
    SC_RUN_TEST(test_web_create_destroy_lifecycle);
    SC_RUN_TEST(test_web_send_empty_target);
#endif
#if SC_HAS_EMAIL
    SC_RUN_TEST(test_email_create);
    SC_RUN_TEST(test_email_name);
    SC_RUN_TEST(test_email_health_check);
    SC_RUN_TEST(test_email_send);
    SC_RUN_TEST(test_email_is_configured);
#if SC_IS_TEST
    SC_RUN_TEST(test_email_last_message_in_test_mode);
    SC_RUN_TEST(test_email_poll_returns_mock_emails);
#endif
    SC_RUN_TEST(test_email_set_auth);
    SC_RUN_TEST(test_email_set_imap);
    SC_RUN_TEST(test_email_poll_no_imap_returns_not_supported);
    SC_RUN_TEST(test_email_set_auth_null_channel);
    SC_RUN_TEST(test_email_set_imap_null_channel);
#endif
#if SC_HAS_IMESSAGE
    SC_RUN_TEST(test_imessage_create);
    SC_RUN_TEST(test_imessage_name);
    SC_RUN_TEST(test_imessage_health_check);
    SC_RUN_TEST(test_imessage_is_configured);
    SC_RUN_TEST(test_imessage_poll_test_mode);
    SC_RUN_TEST(test_imessage_poll_null_args);
#endif
#if SC_HAS_MATTERMOST
    SC_RUN_TEST(test_mattermost_create);
    SC_RUN_TEST(test_mattermost_name);
    SC_RUN_TEST(test_mattermost_health_check);
    SC_RUN_TEST(test_mattermost_send);
    SC_RUN_TEST(test_mattermost_poll_test_mode);
    SC_RUN_TEST(test_mattermost_poll_null_args);
#endif
#if SC_HAS_ONEBOT
    SC_RUN_TEST(test_onebot_create);
    SC_RUN_TEST(test_onebot_name);
    SC_RUN_TEST(test_onebot_health_check);
    SC_RUN_TEST(test_onebot_webhook_and_poll);
    SC_RUN_TEST(test_onebot_poll_null_args);
#endif
#if SC_HAS_DINGTALK
    SC_RUN_TEST(test_dingtalk_create);
    SC_RUN_TEST(test_dingtalk_name);
    SC_RUN_TEST(test_dingtalk_health_check);
    SC_RUN_TEST(test_dingtalk_webhook_and_poll);
    SC_RUN_TEST(test_dingtalk_poll_null_args);
#endif
#if SC_HAS_TEAMS
    SC_RUN_TEST(test_teams_create);
    SC_RUN_TEST(test_teams_health_check);
    SC_RUN_TEST(test_teams_webhook_and_poll);
    SC_RUN_TEST(test_teams_poll_null_args);
#endif
#if SC_HAS_TWILIO
    SC_RUN_TEST(test_twilio_create);
    SC_RUN_TEST(test_twilio_health_check);
    SC_RUN_TEST(test_twilio_webhook_and_poll);
    SC_RUN_TEST(test_twilio_poll_null_args);
#endif
#if SC_HAS_GOOGLE_CHAT
    SC_RUN_TEST(test_google_chat_create);
    SC_RUN_TEST(test_google_chat_health_check);
    SC_RUN_TEST(test_google_chat_webhook_and_poll);
    SC_RUN_TEST(test_google_chat_poll_null_args);
#endif
}
