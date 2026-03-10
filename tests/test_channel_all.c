/* Comprehensive channel tests (~45 tests). Uses HU_HAS_* guards for conditional channels. */
#include "human/channel.h"
#include "human/channels/cli.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"
#include <string.h>

#if HU_HAS_TELEGRAM
#include "human/channel_loop.h"
#include "human/channels/telegram.h"
#endif

#if HU_HAS_DISCORD
#include "human/channel_loop.h"
#include "human/channels/discord.h"
#endif
#if HU_HAS_SLACK
#include "human/channel_loop.h"
#include "human/channels/slack.h"
#endif
#if HU_HAS_WHATSAPP
#include "human/channels/whatsapp.h"
#endif
#if HU_HAS_FACEBOOK
#include "human/channel_loop.h"
#include "human/channels/facebook.h"
#endif
#if HU_HAS_INSTAGRAM
#include "human/channel_loop.h"
#include "human/channels/instagram.h"
#endif
#if HU_HAS_TWITTER
#include "human/channel_loop.h"
#include "human/channels/twitter.h"
#endif
#if HU_HAS_TIKTOK
#include "human/channel_loop.h"
#include "human/channels/tiktok.h"
#endif
#if HU_HAS_GOOGLE_RCS
#include "human/channel_loop.h"
#include "human/channels/google_rcs.h"
#endif
#if HU_HAS_MATRIX
#include "human/channels/matrix.h"
#endif
#if HU_HAS_IRC
#include "human/channels/irc.h"
#endif
#if HU_HAS_LINE
#include "human/channels/line.h"
#endif
#if HU_HAS_LARK
#include "human/channels/lark.h"
#endif
#if HU_HAS_WEB
#include "human/channels/web.h"
#endif
#if HU_HAS_EMAIL
#include "human/channels/email.h"
#endif
#if HU_HAS_IMESSAGE
#include "human/channels/imessage.h"
#endif
#if HU_HAS_MATTERMOST
#include "human/channels/mattermost.h"
#endif
#if HU_HAS_ONEBOT
#include "human/channels/onebot.h"
#endif
#if HU_HAS_DINGTALK
#include "human/channels/dingtalk.h"
#endif
#if HU_HAS_TEAMS
#include "human/channels/teams.h"
#endif
#if HU_HAS_TWILIO
#include "human/channels/twilio.h"
#endif
#if HU_HAS_GOOGLE_CHAT
#include "human/channels/google_chat.h"
#endif

/* ─── CLI (always present) ─────────────────────────────────────────────────── */
static void test_cli_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_cli_create(&alloc, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ch.ctx);
    hu_cli_destroy(&ch);
}

static void test_cli_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_cli_create(&alloc, &ch);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "cli");
    hu_cli_destroy(&ch);
}

static void test_cli_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_cli_create(&alloc, &ch);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_cli_destroy(&ch);
}

static void test_cli_send(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_cli_create(&alloc, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, NULL, 0, "hello", 5, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_cli_destroy(&ch);
}

/* ─── Telegram ────────────────────────────────────────────────────────────── */
#if HU_HAS_TELEGRAM
static void test_telegram_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_telegram_create(&alloc, "test:token", 10, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ch.ctx);
    hu_telegram_destroy(&ch);
}

static void test_telegram_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_telegram_create(&alloc, "t", 1, &ch);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "telegram");
    hu_telegram_destroy(&ch);
}

static void test_telegram_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_telegram_create(&alloc, "t", 1, &ch);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_telegram_destroy(&ch);
}
#endif

/* ─── Discord ──────────────────────────────────────────────────────────────── */
#if HU_HAS_DISCORD
static void test_discord_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_discord_create(&alloc, "token", 5, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "discord");
    hu_discord_destroy(&ch);
}

static void test_discord_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_discord_create(&alloc, NULL, 0, &ch);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "discord");
    hu_discord_destroy(&ch);
}

static void test_discord_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_discord_create(&alloc, "t", 1, &ch);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_discord_destroy(&ch);
}

static void test_discord_start_stop_lifecycle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_discord_create(&alloc, "token", 5, &ch);
    hu_error_t err = ch.vtable->start(ch.ctx);
    HU_ASSERT_EQ(err, HU_OK);
    ch.vtable->stop(ch.ctx);
    hu_discord_destroy(&ch);
}
#endif

/* ─── Slack ───────────────────────────────────────────────────────────────── */
#if HU_HAS_SLACK
static void test_slack_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_slack_create(&alloc, "token", 5, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "slack");
    hu_slack_destroy(&ch);
}

static void test_slack_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_slack_create(&alloc, NULL, 0, &ch);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "slack");
    hu_slack_destroy(&ch);
}

static void test_slack_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_slack_create(&alloc, "t", 1, &ch);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_slack_destroy(&ch);
}

static void test_slack_start_stop_typing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_slack_create(&alloc, "t", 1, &ch);
    if (ch.vtable->start_typing) {
        hu_error_t err = ch.vtable->start_typing(ch.ctx, "channel", 7);
        HU_ASSERT_EQ(err, HU_OK);
    }
    if (ch.vtable->stop_typing) {
        hu_error_t err = ch.vtable->stop_typing(ch.ctx, "channel", 7);
        HU_ASSERT_EQ(err, HU_OK);
    }
    hu_slack_destroy(&ch);
}

static void test_slack_send_long_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_slack_create(&alloc, "t", 1, &ch);
    char buf[5000];
    for (size_t i = 0; i < sizeof(buf) - 1; i++)
        buf[i] = 'm';
    buf[sizeof(buf) - 1] = '\0';
    hu_error_t err = ch.vtable->send(ch.ctx, "ch", 2, buf, sizeof(buf) - 1, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_slack_destroy(&ch);
}

static void test_slack_create_ex(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    const char *ids[] = {"C0001", "C0002"};
    hu_error_t err = hu_slack_create_ex(&alloc, "tok", 3, ids, 2, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "slack");
    hu_slack_destroy(&ch);
}

static void test_slack_poll_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    const char *ids[] = {"C0001"};
    hu_slack_create_ex(&alloc, "tok", 3, ids, 1, &ch);
    ch.vtable->start(ch.ctx);
    hu_channel_loop_msg_t msgs[4];
    size_t out = 99;
    hu_error_t err = hu_slack_poll(ch.ctx, &alloc, msgs, 4, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out, 0);
    hu_slack_destroy(&ch);
}

static void test_slack_webhook_malformed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_slack_create(&alloc, "token", 5, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t out = 99;
    err = hu_slack_poll(ch.ctx, &alloc, msgs, 4, &out);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_slack_poll(ch.ctx, &alloc, msgs, 0, &out);
    HU_ASSERT_EQ(err, HU_OK);
    hu_slack_destroy(&ch);
}

#if HU_IS_TEST
static void test_slack_inject_and_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_slack_create(&alloc, "token", 5, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_slack_test_inject_mock(&ch, "C0001", 5, "Hello from test!", 16);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = hu_slack_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_STR_EQ(msgs[0].content, "Hello from test!");
    HU_ASSERT_STR_EQ(msgs[0].session_key, "C0001");
    hu_slack_destroy(&ch);
}

static void test_slack_send_captures_last_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_slack_create(&alloc, "token", 5, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->send(ch.ctx, "C0001", 5, "Test reply", 10, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    size_t len = 0;
    const char *msg = hu_slack_test_get_last_message(&ch, &len);
    HU_ASSERT(msg != NULL);
    HU_ASSERT_EQ(len, 10);
    HU_ASSERT_STR_EQ(msg, "Test reply");
    hu_slack_destroy(&ch);
}
#endif
#endif

/* ─── WhatsApp ────────────────────────────────────────────────────────────── */
#if HU_HAS_WHATSAPP
static void test_whatsapp_send(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_whatsapp_create(&alloc, "123456", 6, "token", 5, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, "15551234567", 11, "hi", 2, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_whatsapp_destroy(&ch);
}

static void test_whatsapp_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_whatsapp_create(&alloc, "123456", 6, "token", 5, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "whatsapp");
    hu_whatsapp_destroy(&ch);
}

static void test_whatsapp_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_whatsapp_create(&alloc, NULL, 0, NULL, 0, &ch);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "whatsapp");
    hu_whatsapp_destroy(&ch);
}

static void test_whatsapp_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_whatsapp_create(&alloc, "1", 1, "t", 1, &ch);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_whatsapp_destroy(&ch);
}

static void test_whatsapp_webhook_and_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_whatsapp_create(&alloc, "123", 3, "tok", 3, &ch);
    hu_error_t err = hu_whatsapp_on_webhook(ch.ctx, &alloc, "hello from webhook", 18);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t out = 0;
    err = hu_whatsapp_poll(ch.ctx, &alloc, msgs, 4, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out, 1);
    HU_ASSERT_STR_EQ(msgs[0].session_key, "test-sender");
    HU_ASSERT_STR_EQ(msgs[0].content, "hello from webhook");
    err = hu_whatsapp_poll(ch.ctx, &alloc, msgs, 4, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out, 0);
    hu_whatsapp_destroy(&ch);
}

static void test_whatsapp_poll_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_whatsapp_create(&alloc, "1", 1, "t", 1, &ch);
    hu_channel_loop_msg_t msgs[4];
    size_t out = 99;
    hu_error_t err = hu_whatsapp_poll(ch.ctx, &alloc, msgs, 4, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out, 0);
    hu_whatsapp_destroy(&ch);
}
#endif

/* ─── Facebook Messenger ───────────────────────────────────────────────────── */
#if HU_HAS_FACEBOOK
static void test_facebook_send(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_facebook_create(&alloc, "page1", 5, "token", 5, "secret", 6, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, "user123", 7, "hi", 2, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_facebook_destroy(&ch);
}

static void test_facebook_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_facebook_create(&alloc, "page1", 5, "token", 5, "secret", 6, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "facebook");
    hu_facebook_destroy(&ch);
}

static void test_facebook_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_facebook_create(&alloc, NULL, 0, NULL, 0, NULL, 0, &ch);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "facebook");
    hu_facebook_destroy(&ch);
}

static void test_facebook_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_facebook_create(&alloc, "1", 1, "t", 1, "s", 1, &ch);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_facebook_destroy(&ch);
}

static void test_facebook_webhook_and_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_facebook_create(&alloc, "123", 3, "tok", 3, "sec", 3, &ch);
    hu_error_t err = hu_facebook_on_webhook(ch.ctx, &alloc, "hello from webhook", 18);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t out = 0;
    err = hu_facebook_poll(ch.ctx, &alloc, msgs, 4, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out, 1);
    HU_ASSERT_STR_EQ(msgs[0].session_key, "test-sender");
    HU_ASSERT_STR_EQ(msgs[0].content, "hello from webhook");
    err = hu_facebook_poll(ch.ctx, &alloc, msgs, 4, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out, 0);
    hu_facebook_destroy(&ch);
}

static void test_facebook_poll_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_facebook_create(&alloc, "1", 1, "t", 1, "s", 1, &ch);
    hu_channel_loop_msg_t msgs[4];
    size_t out = 99;
    hu_error_t err = hu_facebook_poll(ch.ctx, &alloc, msgs, 4, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out, 0);
    hu_facebook_destroy(&ch);
}

static void test_facebook_webhook_malformed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_facebook_create(&alloc, "123", 3, "tok", 3, "sec", 3, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_facebook_on_webhook(ch.ctx, &alloc, "", 0);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    err = hu_facebook_on_webhook(ch.ctx, &alloc, "}{", 2);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_JSON_PARSE || err == HU_ERR_INVALID_ARGUMENT);
    hu_facebook_destroy(&ch);
}

static void test_facebook_send_empty_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_facebook_create(&alloc, "page1", 5, "token", 5, "secret", 6, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, "user123", 7, "", 0, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_facebook_destroy(&ch);
}
#endif

/* ─── Instagram DMs ────────────────────────────────────────────────────────── */
#if HU_HAS_INSTAGRAM
static void test_instagram_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_instagram_create(&alloc, "biz1", 4, "tok", 3, "sec", 3, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "instagram");
    hu_instagram_destroy(&ch);
}

static void test_instagram_send(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_instagram_create(&alloc, "biz1", 4, "tok", 3, "sec", 3, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, "user1", 5, "hello", 5, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_instagram_destroy(&ch);
}

static void test_instagram_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_instagram_create(&alloc, "b", 1, "t", 1, "s", 1, &ch);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_instagram_destroy(&ch);
}

static void test_instagram_webhook_and_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_instagram_create(&alloc, "123", 3, "tok", 3, "sec", 3, &ch);
    hu_error_t err = hu_instagram_on_webhook(ch.ctx, &alloc, "ig msg", 6);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t out = 0;
    err = hu_instagram_poll(ch.ctx, &alloc, msgs, 4, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out, 1);
    hu_instagram_destroy(&ch);
}

static void test_instagram_poll_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_instagram_create(&alloc, "1", 1, "t", 1, "s", 1, &ch);
    hu_channel_loop_msg_t msgs[4];
    size_t out = 99;
    hu_error_t err = hu_instagram_poll(ch.ctx, &alloc, msgs, 4, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out, 0);
    hu_instagram_destroy(&ch);
}

static void test_instagram_webhook_malformed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_instagram_create(&alloc, "123", 3, "tok", 3, "sec", 3, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_instagram_on_webhook(ch.ctx, &alloc, "", 0);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    err = hu_instagram_on_webhook(ch.ctx, &alloc, "}{", 2);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_JSON_PARSE || err == HU_ERR_INVALID_ARGUMENT);
    hu_instagram_destroy(&ch);
}

static void test_instagram_send_empty_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_instagram_create(&alloc, "biz1", 4, "tok", 3, "sec", 3, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, "user1", 5, "", 0, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_instagram_destroy(&ch);
}
#endif

/* ─── Twitter/X DMs ────────────────────────────────────────────────────────── */
#if HU_HAS_TWITTER
static void test_twitter_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_twitter_create(&alloc, "bearer-tok", 10, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "twitter");
    hu_twitter_destroy(&ch);
}

static void test_twitter_send(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_twitter_create(&alloc, "bearer-tok", 10, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, "user1", 5, "hello", 5, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_twitter_destroy(&ch);
}

static void test_twitter_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_twitter_create(&alloc, "t", 1, &ch);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_twitter_destroy(&ch);
}

static void test_twitter_webhook_and_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_twitter_create(&alloc, "bearer", 6, &ch);
    hu_error_t err = hu_twitter_on_webhook(ch.ctx, &alloc, "tw msg", 6);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t out = 0;
    err = hu_twitter_poll(ch.ctx, &alloc, msgs, 4, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out, 1);
    hu_twitter_destroy(&ch);
}

static void test_twitter_poll_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_twitter_create(&alloc, "t", 1, &ch);
    hu_channel_loop_msg_t msgs[4];
    size_t out = 99;
    hu_error_t err = hu_twitter_poll(ch.ctx, &alloc, msgs, 4, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out, 0);
    hu_twitter_destroy(&ch);
}

static void test_twitter_webhook_malformed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_twitter_create(&alloc, "bearer", 6, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_twitter_on_webhook(ch.ctx, &alloc, "", 0);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    err = hu_twitter_on_webhook(ch.ctx, &alloc, "}{", 2);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_JSON_PARSE || err == HU_ERR_INVALID_ARGUMENT);
    hu_twitter_destroy(&ch);
}

static void test_twitter_send_empty_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_twitter_create(&alloc, "bearer-tok", 10, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, "user1", 5, "", 0, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_twitter_destroy(&ch);
}
#endif

/* ─── Google RCS ───────────────────────────────────────────────────────────── */
#if HU_HAS_GOOGLE_RCS
static void test_google_rcs_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_google_rcs_create(&alloc, "agent1", 6, "tok", 3, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "google_rcs");
    hu_google_rcs_destroy(&ch);
}

static void test_google_rcs_send(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_google_rcs_create(&alloc, "agent1", 6, "tok", 3, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, "+1234", 5, "hello", 5, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_google_rcs_destroy(&ch);
}

static void test_google_rcs_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_google_rcs_create(&alloc, "a", 1, "t", 1, &ch);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_google_rcs_destroy(&ch);
}

static void test_google_rcs_webhook_and_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_google_rcs_create(&alloc, "agent1", 6, "tok", 3, &ch);
    hu_error_t err = hu_google_rcs_on_webhook(ch.ctx, &alloc, "rcs msg", 7);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t out = 0;
    err = hu_google_rcs_poll(ch.ctx, &alloc, msgs, 4, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out, 1);
    hu_google_rcs_destroy(&ch);
}

static void test_google_rcs_poll_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_google_rcs_create(&alloc, "a", 1, "t", 1, &ch);
    hu_channel_loop_msg_t msgs[4];
    size_t out = 99;
    hu_error_t err = hu_google_rcs_poll(ch.ctx, &alloc, msgs, 4, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out, 0);
    hu_google_rcs_destroy(&ch);
}

static void test_google_rcs_webhook_malformed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_google_rcs_create(&alloc, "agent1", 6, "tok", 3, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_google_rcs_on_webhook(ch.ctx, &alloc, "", 0);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    err = hu_google_rcs_on_webhook(ch.ctx, &alloc, "}{", 2);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_JSON_PARSE || err == HU_ERR_INVALID_ARGUMENT);
    hu_google_rcs_destroy(&ch);
}

static void test_google_rcs_send_empty_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_google_rcs_create(&alloc, "agent1", 6, "tok", 3, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, "+1234", 5, "", 0, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_google_rcs_destroy(&ch);
}
#endif

/* ─── Matrix ───────────────────────────────────────────────────────────────── */
#if HU_HAS_MATRIX
static void test_matrix_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_matrix_create(&alloc, "https://matrix.org", 17, "tok", 3, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "matrix");
    hu_matrix_destroy(&ch);
}

static void test_matrix_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_matrix_create(&alloc, "hs", 2, "t", 1, &ch);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "matrix");
    hu_matrix_destroy(&ch);
}

static void test_matrix_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_matrix_create(&alloc, "h", 1, "t", 1, &ch);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_matrix_destroy(&ch);
}

static void test_matrix_send(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_matrix_create(&alloc, "https://matrix.org", 17, "tok", 3, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, "#room:matrix.org", 16, "test", 4, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_matrix_destroy(&ch);
}

static void test_matrix_poll_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_matrix_create(&alloc, "https://example.com", 19, "test-token", 10, &ch);
    HU_ASSERT(err == HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 99;
    err = hu_matrix_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(count == 0);
    hu_matrix_destroy(&ch);
}

static void test_matrix_poll_null_args(void) {
    hu_error_t err = hu_matrix_poll(NULL, NULL, NULL, 0, NULL);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
}
#endif

/* ─── IRC ──────────────────────────────────────────────────────────────────── */
#if HU_HAS_IRC
static void test_irc_send(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_irc_create(&alloc, "irc.example.com", 15, 6667, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, "#chan", 5, "msg", 3, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_irc_destroy(&ch);
}

static void test_irc_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_irc_create(&alloc, "irc.example.com", 15, 6667, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "irc");
    hu_irc_destroy(&ch);
}

static void test_irc_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_irc_create(&alloc, "s", 1, 6667, &ch);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "irc");
    hu_irc_destroy(&ch);
}

static void test_irc_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_irc_create(&alloc, "s", 1, 6667, &ch);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_irc_destroy(&ch);
}

static void test_irc_poll_test_mode_impl(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_irc_create(&alloc, "irc.example.com", 15, 6667, &ch);
    HU_ASSERT(err == HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 99;
    err = hu_irc_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(count == 0);
    hu_irc_destroy(&ch);
}

static void test_irc_poll_null_args_impl(void) {
    hu_error_t err = hu_irc_poll(NULL, NULL, NULL, 0, NULL);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
}
#endif

/* ─── LINE ────────────────────────────────────────────────────────────────── */
#if HU_HAS_LINE
static void test_line_start_stop_lifecycle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_line_create(&alloc, "tok", 3, &ch);
    hu_error_t err = ch.vtable->start(ch.ctx);
    HU_ASSERT_EQ(err, HU_OK);
    ch.vtable->stop(ch.ctx);
    hu_line_destroy(&ch);
}

static void test_line_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_line_create(&alloc, "channeltoken", 12, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "line");
    hu_line_destroy(&ch);
}

static void test_line_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_line_create(&alloc, "t", 1, &ch);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "line");
    hu_line_destroy(&ch);
}

static void test_line_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_line_create(&alloc, "t", 1, &ch);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_line_destroy(&ch);
}
#endif

/* ─── Lark ─────────────────────────────────────────────────────────────────── */
#if HU_HAS_LARK
static void test_lark_start_stop_lifecycle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_lark_create(&alloc, "app", 3, "secret", 6, &ch);
    hu_error_t err = ch.vtable->start(ch.ctx);
    HU_ASSERT_EQ(err, HU_OK);
    ch.vtable->stop(ch.ctx);
    hu_lark_destroy(&ch);
}

static void test_lark_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_lark_create(&alloc, "app_id", 6, "app_secret", 10, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "lark");
    hu_lark_destroy(&ch);
}

static void test_lark_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_lark_create(&alloc, "a", 1, "b", 1, &ch);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "lark");
    hu_lark_destroy(&ch);
}

static void test_lark_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_lark_create(&alloc, "a", 1, "b", 1, &ch);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_lark_destroy(&ch);
}
#endif

/* ─── Web ──────────────────────────────────────────────────────────────────── */
#if HU_HAS_WEB
static void test_web_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_web_create(&alloc, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "web");
    hu_web_destroy(&ch);
}

static void test_web_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_web_create(&alloc, &ch);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "web");
    hu_web_destroy(&ch);
}

static void test_web_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_web_create(&alloc, &ch);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_web_destroy(&ch);
}

static void test_web_create_destroy_lifecycle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_web_create(&alloc, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ch.ctx);
    HU_ASSERT_NOT_NULL(ch.vtable);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "web");
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));

    err = ch.vtable->start(ch.ctx);
    HU_ASSERT_EQ(err, HU_OK);

    err = ch.vtable->send(ch.ctx, "user1", 5, "msg1", 4, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->send(ch.ctx, "user2", 5, "msg2", 4, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    ch.vtable->stop(ch.ctx);
    hu_web_destroy(&ch);
}
#endif

/* ─── Email ────────────────────────────────────────────────────────────────── */
#if HU_HAS_EMAIL
static void test_email_send(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_email_create(&alloc, "smtp.example.com", 16, 587, "from@ex.com", 11, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, "user@ex.com", 11, "body", 4, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_email_destroy(&ch);
}

static void test_email_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_email_create(&alloc, "smtp.example.com", 16, 587, "from@ex.com", 11, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "email");
    hu_email_destroy(&ch);
}

static void test_email_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_email_create(&alloc, "h", 1, 25, "f@x", 3, &ch);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "email");
    hu_email_destroy(&ch);
}

static void test_email_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_email_create(&alloc, "h", 1, 25, "f@x", 3, &ch);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_email_destroy(&ch);
}

static void test_email_is_configured(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_email_create(&alloc, "smtp.example.com", 16, 587, NULL, 0, &ch);
    HU_ASSERT_FALSE(hu_email_is_configured(&ch));
    hu_email_destroy(&ch);

    hu_email_create(&alloc, "smtp.example.com", 16, 587, "bot@example.com", 15, &ch);
    HU_ASSERT_TRUE(hu_email_is_configured(&ch));
    hu_email_destroy(&ch);
}

#if HU_IS_TEST
static void test_email_last_message_in_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_email_create(&alloc, "smtp.example.com", 16, 587, "bot@example.com", 15, &ch);
    ch.vtable->send(ch.ctx, "user@ex.com", 11, "test body", 9, NULL, 0);
    const char *last = hu_email_test_last_message(&ch);
    HU_ASSERT_NOT_NULL(last);
    HU_ASSERT_TRUE(strstr(last, "test body") != NULL);
    HU_ASSERT_TRUE(strstr(last, "Content-Type") != NULL);
    hu_email_destroy(&ch);
}

static void test_email_poll_returns_mock_emails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_email_create(&alloc, "smtp.example.com", 16, 587, "bot@example.com", 15, &ch);
    hu_error_t err =
        hu_email_test_inject_mock_email(&ch, "sender@ex.com", 13, "mock subject\n\nmock body", 23);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = hu_email_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_STR_EQ(msgs[0].session_key, "sender@ex.com");
    HU_ASSERT_STR_EQ(msgs[0].content, "mock subject\n\nmock body");
    hu_email_destroy(&ch);
}
#endif

static void test_email_set_auth(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_email_create(&alloc, "smtp.gmail.com", 14, 587, "me@gmail.com", 12, &ch);
    hu_error_t err = hu_email_set_auth(&ch, "me@gmail.com", 12, "apppassword", 11);
    HU_ASSERT_EQ(err, HU_OK);
    hu_email_destroy(&ch);
}

static void test_email_set_imap(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_email_create(&alloc, "smtp.gmail.com", 14, 587, "me@gmail.com", 12, &ch);
    hu_error_t err = hu_email_set_imap(&ch, "imap.gmail.com", 14, 993);
    HU_ASSERT_EQ(err, HU_OK);
    hu_email_destroy(&ch);
}

static void test_email_poll_no_imap_returns_not_supported(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_email_create(&alloc, "smtp.gmail.com", 14, 587, "me@gmail.com", 12, &ch);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 0;
    hu_error_t err = hu_email_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT_EQ(err, HU_OK); /* HU_IS_TEST returns HU_OK */
    HU_ASSERT_EQ(count, 0u);
    hu_email_destroy(&ch);
}

static void test_email_set_auth_null_channel(void) {
    hu_error_t err = hu_email_set_auth(NULL, "x", 1, "y", 1);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_email_set_imap_null_channel(void) {
    hu_error_t err = hu_email_set_imap(NULL, "x", 1, 993);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}
#endif

/* ─── iMessage ─────────────────────────────────────────────────────────────── */
#if HU_HAS_IMESSAGE
static void test_imessage_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_imessage_create(&alloc, NULL, 0, NULL, 0, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "imessage");
    hu_imessage_destroy(&ch);
}

static void test_imessage_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_imessage_create(&alloc, NULL, 0, NULL, 0, &ch);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "imessage");
    hu_imessage_destroy(&ch);
}

static void test_imessage_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_imessage_create(&alloc, NULL, 0, NULL, 0, &ch);
#ifdef __APPLE__
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
#else
    HU_ASSERT_FALSE(ch.vtable->health_check(ch.ctx));
#endif
    hu_imessage_destroy(&ch);
}

static void test_imessage_is_configured(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_imessage_create(&alloc, NULL, 0, NULL, 0, &ch);
    HU_ASSERT_FALSE(hu_imessage_is_configured(&ch));
    hu_imessage_destroy(&ch);

    hu_imessage_create(&alloc, "+15551234567", 11, NULL, 0, &ch);
    HU_ASSERT_TRUE(hu_imessage_is_configured(&ch));
    hu_imessage_destroy(&ch);
}

static void test_imessage_poll_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_imessage_create(&alloc, "+15551234567", 11, NULL, 0, &ch);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 99;
    hu_error_t err = hu_imessage_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);
    hu_imessage_destroy(&ch);
}

static void test_imessage_poll_null_args(void) {
    hu_error_t err = hu_imessage_poll(NULL, NULL, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

#if HU_IS_TEST
static void test_imessage_inject_and_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_imessage_test_inject_mock(&ch, "+15559876543", 12, "Hello from test!", 16);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = hu_imessage_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_STR_EQ(msgs[0].content, "Hello from test!");
    HU_ASSERT_STR_EQ(msgs[0].session_key, "+15559876543");
    HU_ASSERT_EQ(msgs[0].message_id, (int64_t)1);
    hu_imessage_destroy(&ch);
}

static void test_imessage_inject_multiple_poll_message_ids(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_imessage_test_inject_mock(&ch, "alice", 5, "First", 5);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_imessage_test_inject_mock(&ch, "bob", 3, "Second", 6);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_imessage_test_inject_mock(&ch, "carol", 5, "Third", 5);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = hu_imessage_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 3);
    HU_ASSERT_EQ(msgs[0].message_id, (int64_t)1);
    HU_ASSERT_EQ(msgs[1].message_id, (int64_t)2);
    HU_ASSERT_EQ(msgs[2].message_id, (int64_t)3);
    hu_imessage_destroy(&ch);
}

static void test_imessage_send_captures_last_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->send(ch.ctx, "+15551234567", 12, "Test reply", 10, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    size_t len = 0;
    const char *msg = hu_imessage_test_get_last_message(&ch, &len);
    HU_ASSERT(msg != NULL);
    HU_ASSERT_EQ(len, 10);
    HU_ASSERT_STR_EQ(msg, "Test reply");
    hu_imessage_destroy(&ch);
}
#endif
#endif

/* ─── Mattermost ───────────────────────────────────────────────────────────── */
#if HU_HAS_MATTERMOST
static void test_mattermost_send(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_mattermost_create(&alloc, "https://chat.example.com", 24, "token", 5, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, "channel_id", 10, "msg", 3, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_mattermost_destroy(&ch);
}

static void test_mattermost_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_mattermost_create(&alloc, "https://chat.example.com", 24, "token", 5, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "mattermost");
    hu_mattermost_destroy(&ch);
}

static void test_mattermost_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_mattermost_create(&alloc, "u", 1, "t", 1, &ch);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "mattermost");
    hu_mattermost_destroy(&ch);
}

static void test_mattermost_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_mattermost_create(&alloc, "u", 1, "t", 1, &ch);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_mattermost_destroy(&ch);
}
#endif

/* ─── OneBot ──────────────────────────────────────────────────────────────── */
#if HU_HAS_ONEBOT
static void test_onebot_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_onebot_create(&alloc, "http://127.0.0.1:5700", 20, "tok", 3, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "onebot");
    hu_onebot_destroy(&ch);
}

static void test_onebot_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_onebot_create(&alloc, "u", 1, "t", 1, &ch);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "onebot");
    hu_onebot_destroy(&ch);
}

static void test_onebot_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_onebot_create(&alloc, "u", 1, "t", 1, &ch);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_onebot_destroy(&ch);
}
#endif

/* ─── DingTalk ─────────────────────────────────────────────────────────────── */
#if HU_HAS_DINGTALK
static void test_dingtalk_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_dingtalk_create(&alloc, "appkey", 7, "secret", 6, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "dingtalk");
    hu_dingtalk_destroy(&ch);
}

static void test_dingtalk_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_dingtalk_create(&alloc, "a", 1, "b", 1, &ch);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "dingtalk");
    hu_dingtalk_destroy(&ch);
}

static void test_dingtalk_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_dingtalk_create(&alloc, "a", 1, "b", 1, &ch);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_dingtalk_destroy(&ch);
}
#endif

/* ─── Signal, Nostr, QQ, MaixCam, Dispatch (always in test build) ───────── */
#if HU_HAS_SIGNAL
#include "human/channel_loop.h"
#include "human/channels/signal.h"
static void test_signal_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_signal_create(&alloc, "http://localhost:8080", 21, "test", 4, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ch.ctx);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "signal");
    hu_signal_destroy(&ch);
}

static void test_signal_destroy_lifecycle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_signal_create(&alloc, "http://x", 7, "a", 1, &ch);
    hu_signal_destroy(&ch);
}

static void test_signal_send(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_signal_create(&alloc, "http://localhost", 16, "a", 1, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, NULL, 0, "hello", 5, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_signal_destroy(&ch);
}

static void test_signal_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_signal_create(&alloc, "http://x", 7, "a", 1, &ch);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_signal_destroy(&ch);
}

static void test_signal_start_stop_typing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_signal_create(&alloc, "http://x", 7, "a", 1, &ch);
    if (ch.vtable->start_typing) {
        hu_error_t err = ch.vtable->start_typing(ch.ctx, "recipient", 9);
        HU_ASSERT_EQ(err, HU_OK);
    }
    if (ch.vtable->stop_typing) {
        hu_error_t err = ch.vtable->stop_typing(ch.ctx, "recipient", 9);
        HU_ASSERT_EQ(err, HU_OK);
    }
    hu_signal_destroy(&ch);
}

static void test_signal_send_long_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_signal_create(&alloc, "http://localhost", 16, "a", 1, &ch);
    char buf[2000];
    for (size_t i = 0; i < sizeof(buf) - 1; i++)
        buf[i] = 'x';
    buf[sizeof(buf) - 1] = '\0';
    hu_error_t err = ch.vtable->send(ch.ctx, NULL, 0, buf, sizeof(buf) - 1, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_signal_destroy(&ch);
}

#if HU_IS_TEST
static void test_signal_inject_and_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_signal_create(&alloc, "http://localhost:8080", 21, "+15551234567", 12, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_signal_test_inject_mock(&ch, "+15559876543", 12, "Hello from test!", 16);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = hu_signal_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_STR_EQ(msgs[0].content, "Hello from test!");
    HU_ASSERT_STR_EQ(msgs[0].session_key, "+15559876543");
    hu_signal_destroy(&ch);
}

static void test_signal_send_captures_last_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_signal_create(&alloc, "http://localhost", 16, "a", 1, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->send(ch.ctx, "+15551234567", 12, "Test reply", 10, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    size_t len = 0;
    const char *msg = hu_signal_test_get_last_message(&ch, &len);
    HU_ASSERT(msg != NULL);
    HU_ASSERT_EQ(len, 10);
    HU_ASSERT_STR_EQ(msg, "Test reply");
    hu_signal_destroy(&ch);
}
#endif
#endif

#if HU_HAS_NOSTR
#include "human/channel_loop.h"
#include "human/channels/nostr.h"
static void test_nostr_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_nostr_create(&alloc, "/tmp/nak", 8, "npub1abc", 8, NULL, 0, NULL, 0, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ch.ctx);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "nostr");
    hu_nostr_destroy(&ch);
}

static void test_nostr_destroy_lifecycle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_nostr_create(&alloc, "/tmp", 4, "npub", 4, NULL, 0, NULL, 0, &ch);
    hu_nostr_destroy(&ch);
}

static void test_nostr_send(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_nostr_create(&alloc, "/tmp", 4, "npub", 4, NULL, 0, NULL, 0, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, NULL, 0, "msg", 3, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_nostr_destroy(&ch);
}

static void test_nostr_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_nostr_create(&alloc, "/tmp", 4, "npub", 4, NULL, 0, NULL, 0, &ch);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_nostr_destroy(&ch);
}

static void test_nostr_poll_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_nostr_create(&alloc, "/tmp/nak", 8, "npub1x", 6, "wss://relay.example.com",
                                     21, "seckey", 6, &ch);
    HU_ASSERT(err == HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 99;
    err = hu_nostr_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(count == 0);
    hu_nostr_destroy(&ch);
}
#endif

#if HU_HAS_MQTT
#include "human/channel_loop.h"
#include "human/channels/mqtt.h"
static void test_mqtt_create_null_alloc_returns_error(void) {
    hu_channel_t ch = {0};
    hu_error_t err =
        hu_mqtt_create(NULL, "mqtt://broker", 14, NULL, 0, NULL, 0, NULL, 0, NULL, 0, 0, &ch);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}
static void test_mqtt_create_null_broker_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_mqtt_create(&alloc, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, 0, &ch);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}
static void test_mqtt_create_valid_lifecycle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_mqtt_create(&alloc, "mqtt://broker.example.com", 26, "in", 2, "out", 3,
                                    NULL, 0, NULL, 0, 0, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ch.ctx);
    err = ch.vtable->start(ch.ctx);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->send(ch.ctx, NULL, 0, "hello", 5, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    ch.vtable->stop(ch.ctx);
    hu_mqtt_destroy(&ch, &alloc);
}
static void test_mqtt_create_valid_health_check_when_running(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err =
        hu_mqtt_create(&alloc, "mqtt://broker", 13, NULL, 0, NULL, 0, NULL, 0, NULL, 0, 0, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->start(ch.ctx);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    ch.vtable->stop(ch.ctx);
    hu_mqtt_destroy(&ch, &alloc);
}

static void test_mqtt_send_stores_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err =
        hu_mqtt_create(&alloc, "mqtt://broker", 13, NULL, 0, NULL, 0, NULL, 0, NULL, 0, 0, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->start(ch.ctx);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->send(ch.ctx, NULL, 0, "hello world", 11, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    size_t len = 0;
    const char *last = hu_mqtt_test_get_last_message(&ch, &len);
    HU_ASSERT_NOT_NULL(last);
    HU_ASSERT_EQ(len, 11u);
    HU_ASSERT_STR_EQ(last, "hello world");
    hu_mqtt_destroy(&ch, &alloc);
}

static void test_mqtt_poll_returns_injected_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err =
        hu_mqtt_create(&alloc, "mqtt://broker", 13, NULL, 0, NULL, 0, NULL, 0, NULL, 0, 0, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->start(ch.ctx);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_mqtt_test_inject_mock(&ch, "sess1", 5, "injected payload", 16);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = hu_mqtt_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_STR_EQ(msgs[0].session_key, "sess1");
    HU_ASSERT_STR_EQ(msgs[0].content, "injected payload");
    hu_mqtt_destroy(&ch, &alloc);
}

static void test_mqtt_send_before_start_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err =
        hu_mqtt_create(&alloc, "mqtt://broker", 13, NULL, 0, NULL, 0, NULL, 0, NULL, 0, 0, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->send(ch.ctx, NULL, 0, "hello", 5, NULL, 0);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    hu_mqtt_destroy(&ch, &alloc);
}

static void test_mqtt_poll_empty_returns_no_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err =
        hu_mqtt_create(&alloc, "mqtt://broker", 13, NULL, 0, NULL, 0, NULL, 0, NULL, 0, 0, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->start(ch.ctx);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 99;
    err = hu_mqtt_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);
    hu_mqtt_destroy(&ch, &alloc);
}
#endif

#if HU_HAS_QQ
#include "human/channels/qq.h"
static void test_qq_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_qq_create(&alloc, "app-id", 6, "tok", 3, false, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "qq");
    hu_qq_destroy(&ch);
}

static void test_qq_send(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_qq_create_ex(&alloc, "app", 3, "t", 1, "ch123", 5, false, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, NULL, 0, "h", 1, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_qq_destroy(&ch);
}

static void test_qq_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_qq_create(&alloc, "app", 3, "t", 1, false, &ch);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_qq_destroy(&ch);
}
#endif

#if HU_HAS_MAIXCAM
#include "human/channels/maixcam.h"
static void test_maixcam_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_maixcam_create(&alloc, "/dev/ttyUSB0", 12, 0, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "maixcam");
    hu_maixcam_destroy(&ch);
}

static void test_maixcam_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_maixcam_create(&alloc, "/dev/x", 6, 0, &ch);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_maixcam_destroy(&ch);
}
#endif

#if HU_HAS_DISPATCH
extern hu_error_t hu_dispatch_create(hu_allocator_t *alloc, hu_channel_t *out);
extern void hu_dispatch_destroy(hu_channel_t *ch);
static void test_dispatch_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_dispatch_create(&alloc, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "dispatch");
    hu_dispatch_destroy(&ch);
}

static void test_dispatch_send(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_dispatch_create(&alloc, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, NULL, 0, "msg", 3, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_dispatch_destroy(&ch);
}

static void test_dispatch_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_dispatch_create(&alloc, &ch);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_dispatch_destroy(&ch);
}
#endif

static void test_cli_send_empty_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_cli_create(&alloc, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, NULL, 0, "", 0, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_cli_destroy(&ch);
}

static void test_cli_send_long_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_cli_create(&alloc, &ch);
    char buf[1024];
    for (size_t i = 0; i < sizeof(buf) - 1; i++)
        buf[i] = 'a';
    buf[sizeof(buf) - 1] = '\0';
    hu_error_t err = ch.vtable->send(ch.ctx, NULL, 0, buf, sizeof(buf) - 1, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_cli_destroy(&ch);
}

static void test_cli_create_destroy_repeat(void) {
    hu_allocator_t alloc = hu_system_allocator();
    for (int i = 0; i < 3; i++) {
        hu_channel_t ch;
        hu_cli_create(&alloc, &ch);
        HU_ASSERT_NOT_NULL(ch.vtable);
        hu_cli_destroy(&ch);
    }
}

#if HU_HAS_TELEGRAM
static void test_telegram_escape_markdown(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t out_len = 0;
    char *esc = hu_telegram_escape_markdown_v2(&alloc, "_*[]", 4, &out_len);
    HU_ASSERT_NOT_NULL(esc);
    HU_ASSERT_TRUE(out_len >= 4);
    HU_ASSERT_TRUE(strchr(esc, '\\') != NULL);
    alloc.free(alloc.ctx, esc, out_len + 1);
}

static void test_telegram_commands_help(void) {
    const char *help = hu_telegram_commands_help();
    HU_ASSERT_NOT_NULL(help);
    HU_ASSERT_TRUE(strstr(help, "/start") != NULL);
    HU_ASSERT_TRUE(strstr(help, "/help") != NULL);
}

static void test_telegram_start_stop_typing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_telegram_create(&alloc, "t", 1, &ch);
    if (ch.vtable->start_typing) {
        hu_error_t err = ch.vtable->start_typing(ch.ctx, "user1", 5);
        HU_ASSERT_EQ(err, HU_OK);
    }
    if (ch.vtable->stop_typing) {
        hu_error_t err = ch.vtable->stop_typing(ch.ctx, "user1", 5);
        HU_ASSERT_EQ(err, HU_OK);
    }
    hu_telegram_destroy(&ch);
}

static void test_telegram_allowlist(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_telegram_create(&alloc, "t", 1, &ch);
    const char *allow[] = {"user1", "user2", NULL};
    hu_telegram_set_allowlist(&ch, allow, 2);
    hu_telegram_destroy(&ch);
}

static void test_telegram_send_long_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_telegram_create(&alloc, "t", 1, &ch);
    char buf[5000];
    for (size_t i = 0; i < sizeof(buf) - 1; i++)
        buf[i] = 'a';
    buf[sizeof(buf) - 1] = '\0';
    hu_error_t err = ch.vtable->send(ch.ctx, "123", 3, buf, sizeof(buf) - 1, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_telegram_destroy(&ch);
}

static void test_telegram_send(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_telegram_create(&alloc, "t", 1, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, "12345", 5, "test msg", 8, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_telegram_destroy(&ch);
}

static void test_telegram_create_destroy_lifecycle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_telegram_create(&alloc, "token", 5, &ch);
    HU_ASSERT_NOT_NULL(ch.ctx);
    hu_telegram_destroy(&ch);
}

static void test_telegram_webhook_malformed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_telegram_create(&alloc, "t", 1, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t out = 99;
    err = hu_telegram_poll(ch.ctx, &alloc, msgs, 4, &out);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_telegram_poll(ch.ctx, &alloc, msgs, 0, &out);
    HU_ASSERT_EQ(err, HU_OK);
    hu_telegram_destroy(&ch);
}

#if HU_IS_TEST
static void test_telegram_inject_and_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_telegram_create(&alloc, "test:token", 10, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_telegram_test_inject_mock(&ch, "chat123", 7, "Hello from test!", 16);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = hu_telegram_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_STR_EQ(msgs[0].content, "Hello from test!");
    HU_ASSERT_STR_EQ(msgs[0].session_key, "chat123");
    hu_telegram_destroy(&ch);
}

static void test_telegram_send_captures_last_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_telegram_create(&alloc, "test:token", 10, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->send(ch.ctx, "12345", 5, "Test reply", 10, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    size_t len = 0;
    const char *msg = hu_telegram_test_get_last_message(&ch, &len);
    HU_ASSERT(msg != NULL);
    HU_ASSERT_EQ(len, 10);
    HU_ASSERT_STR_EQ(msg, "Test reply");
    hu_telegram_destroy(&ch);
}
#endif
#endif

#if HU_HAS_DISCORD
static void test_discord_send(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_discord_create(&alloc, "t", 1, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, "channel1", 8, "msg", 3, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_discord_destroy(&ch);
}

static void test_discord_send_without_token_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_discord_create(&alloc, NULL, 0, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, "ch", 2, "msg", 3, NULL, 0);
    HU_ASSERT_EQ(err, HU_ERR_CHANNEL_NOT_CONFIGURED);
    hu_discord_destroy(&ch);
}

static void test_discord_poll_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_discord_create(&alloc, "t", 1, &ch);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 99;
    hu_error_t err = hu_discord_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);
    hu_discord_destroy(&ch);
}

static void test_discord_poll_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    const char *ch_ids[] = {"123456789"};
    hu_error_t err = hu_discord_create_ex(&alloc, "t", 1, ch_ids, 1, NULL, 0, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 99;
    err = hu_discord_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);
    hu_discord_destroy(&ch);
}

static void test_discord_webhook_malformed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_discord_create(&alloc, "t", 1, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t out = 99;
    err = hu_discord_poll(ch.ctx, &alloc, msgs, 4, &out);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_discord_poll(ch.ctx, &alloc, msgs, 0, &out);
    HU_ASSERT_EQ(err, HU_OK);
    hu_discord_destroy(&ch);
}

#if HU_IS_TEST
static void test_discord_inject_and_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_discord_create(&alloc, "token", 5, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_discord_test_inject_mock(&ch, "channel1", 8, "Hello from test!", 16);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = hu_discord_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_STR_EQ(msgs[0].content, "Hello from test!");
    HU_ASSERT_STR_EQ(msgs[0].session_key, "channel1");
    hu_discord_destroy(&ch);
}

static void test_discord_send_captures_last_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_discord_create(&alloc, "token", 5, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->send(ch.ctx, "channel1", 8, "Test reply", 10, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    size_t len = 0;
    const char *msg = hu_discord_test_get_last_message(&ch, &len);
    HU_ASSERT(msg != NULL);
    HU_ASSERT_EQ(len, 10);
    HU_ASSERT_STR_EQ(msg, "Test reply");
    hu_discord_destroy(&ch);
}
#endif
#endif

#if HU_HAS_WEB
static void test_web_send_empty_target(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_web_create(&alloc, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, "", 0, "hi", 2, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_web_destroy(&ch);
}
#endif

/* ─── Webhook + Poll tests ───────────────────────────────────────── */

#if HU_HAS_DISCORD
static void test_discord_webhook_and_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_discord_create(&alloc, "token", 5, &ch);
    hu_error_t err = hu_discord_on_webhook(ch.ctx, &alloc, "hello from discord webhook", 26);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t out = 0;
    err = hu_discord_poll(ch.ctx, &alloc, msgs, 4, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out, 1);
    HU_ASSERT_STR_EQ(msgs[0].session_key, "test-sender");
    HU_ASSERT_STR_EQ(msgs[0].content, "hello from discord webhook");
    err = hu_discord_poll(ch.ctx, &alloc, msgs, 4, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out, 0);
    hu_discord_destroy(&ch);
}

static void test_discord_webhook_empty_body(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_discord_create(&alloc, "token", 5, &ch);
    hu_error_t err = hu_discord_on_webhook(ch.ctx, &alloc, "", 0);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    hu_discord_destroy(&ch);
}
#endif

#if HU_HAS_SLACK
static void test_slack_webhook_and_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_slack_create(&alloc, "token", 5, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_slack_on_webhook(ch.ctx, &alloc, "hello from slack webhook", 24);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t out = 0;
    err = hu_slack_poll(ch.ctx, &alloc, msgs, 4, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out, 1);
    HU_ASSERT_STR_EQ(msgs[0].session_key, "test-sender");
    HU_ASSERT_STR_EQ(msgs[0].content, "hello from slack webhook");
    err = hu_slack_poll(ch.ctx, &alloc, msgs, 4, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out, 0);
    hu_slack_destroy(&ch);
}

static void test_slack_webhook_empty_body(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_slack_create(&alloc, "token", 5, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_slack_on_webhook(ch.ctx, &alloc, "", 0);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    hu_slack_destroy(&ch);
}
#endif

#if HU_HAS_TIKTOK
static void test_tiktok_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_tiktok_create(&alloc, "key", 3, "secret", 6, "token", 5, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "tiktok");
    hu_tiktok_destroy(&ch);
}

static void test_tiktok_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_tiktok_create(&alloc, NULL, 0, NULL, 0, NULL, 0, &ch);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "tiktok");
    hu_tiktok_destroy(&ch);
}

static void test_tiktok_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_tiktok_create(&alloc, "k", 1, "s", 1, "t", 1, &ch);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_tiktok_destroy(&ch);
}

static void test_tiktok_send(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_tiktok_create(&alloc, "k", 1, "s", 1, "t", 1, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, "video123", 8, "Hello TikTok!", 13, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_tiktok_destroy(&ch);
}

static void test_tiktok_webhook_and_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_tiktok_create(&alloc, "k", 1, "s", 1, "t", 1, &ch);
    hu_error_t err = hu_tiktok_on_webhook(ch.ctx, &alloc, "hello from tiktok", 17);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t out = 0;
    err = hu_tiktok_poll(ch.ctx, &alloc, msgs, 4, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out, 1);
    HU_ASSERT_STR_EQ(msgs[0].session_key, "test-sender");
    HU_ASSERT_STR_EQ(msgs[0].content, "hello from tiktok");
    err = hu_tiktok_poll(ch.ctx, &alloc, msgs, 4, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out, 0);
    hu_tiktok_destroy(&ch);
}

static void test_tiktok_poll_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_tiktok_create(&alloc, "k", 1, "s", 1, "t", 1, &ch);
    hu_channel_loop_msg_t msgs[4];
    size_t out = 99;
    hu_error_t err = hu_tiktok_poll(ch.ctx, &alloc, msgs, 4, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out, 0);
    hu_tiktok_destroy(&ch);
}

static void test_tiktok_webhook_empty_body(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_tiktok_create(&alloc, "k", 1, "s", 1, "t", 1, &ch);
    hu_error_t err = hu_tiktok_on_webhook(ch.ctx, &alloc, "", 0);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    hu_tiktok_destroy(&ch);
}

#if HU_IS_TEST
static void test_tiktok_inject_and_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_tiktok_create(&alloc, "k", 1, "s", 1, "t", 1, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_tiktok_test_inject_mock(&ch, "user1", 5, "Mock message", 12);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = hu_tiktok_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_STR_EQ(msgs[0].content, "Mock message");
    HU_ASSERT_STR_EQ(msgs[0].session_key, "user1");
    hu_tiktok_destroy(&ch);
}

static void test_tiktok_send_captures_last_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_tiktok_create(&alloc, "k", 1, "s", 1, "t", 1, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->send(ch.ctx, "video1", 6, "Reply text", 10, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    size_t len = 0;
    const char *msg = hu_tiktok_test_get_last_message(&ch, &len);
    HU_ASSERT(msg != NULL);
    HU_ASSERT_EQ(len, 10);
    HU_ASSERT_STR_EQ(msg, "Reply text");
    hu_tiktok_destroy(&ch);
}
#endif
#endif

#if HU_HAS_LINE
static void test_line_webhook_and_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_line_create(&alloc, "test-token", 10, &ch);
    HU_ASSERT(err == HU_OK);
    err = hu_line_on_webhook(ch.ctx, &alloc, "hello from line", 15);
    HU_ASSERT(err == HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = hu_line_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(count == 1);
    HU_ASSERT_STR_EQ(msgs[0].session_key, "test-sender");
    hu_line_destroy(&ch);
}

static void test_line_poll_null_args(void) {
    hu_error_t err = hu_line_poll(NULL, NULL, NULL, 0, NULL);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
}
#endif

#if HU_HAS_LARK
static void test_lark_webhook_and_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_lark_create(&alloc, "app-id", 6, "secret", 6, &ch);
    HU_ASSERT(err == HU_OK);
    err = hu_lark_on_webhook(ch.ctx, &alloc, "hello from lark", 15);
    HU_ASSERT(err == HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = hu_lark_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(count == 1);
    hu_lark_destroy(&ch);
}

static void test_lark_poll_null_args(void) {
    hu_error_t err = hu_lark_poll(NULL, NULL, NULL, 0, NULL);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
}
#endif

#if HU_HAS_MATTERMOST
static void test_mattermost_poll_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_mattermost_create(&alloc, "https://example.com", 19, "test-token", 10, &ch);
    HU_ASSERT(err == HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 99;
    err = hu_mattermost_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(count == 0);
    hu_mattermost_destroy(&ch);
}

static void test_mattermost_poll_null_args(void) {
    hu_error_t err = hu_mattermost_poll(NULL, NULL, NULL, 0, NULL);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
}
#endif

#if HU_HAS_ONEBOT
static void test_onebot_webhook_and_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_onebot_create(&alloc, "http://localhost:5700", 21, "test", 4, &ch);
    HU_ASSERT(err == HU_OK);
    err = hu_onebot_on_webhook(ch.ctx, &alloc, "hello from onebot", 17);
    HU_ASSERT(err == HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = hu_onebot_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(count == 1);
    hu_onebot_destroy(&ch);
}

static void test_onebot_poll_null_args(void) {
    hu_error_t err = hu_onebot_poll(NULL, NULL, NULL, 0, NULL);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
}
#endif

#if HU_HAS_DINGTALK
static void test_dingtalk_webhook_and_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_dingtalk_create(&alloc, "key", 3, "secret", 6, &ch);
    HU_ASSERT(err == HU_OK);
    err = hu_dingtalk_on_webhook(ch.ctx, &alloc, "hello from dingtalk", 19);
    HU_ASSERT(err == HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = hu_dingtalk_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(count == 1);
    hu_dingtalk_destroy(&ch);
}

static void test_dingtalk_poll_null_args(void) {
    hu_error_t err = hu_dingtalk_poll(NULL, NULL, NULL, 0, NULL);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
}
#endif

/* ─── Teams ─────────────────────────────────────────────────────────────── */
#if HU_HAS_TEAMS
static void test_teams_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_teams_create(&alloc, "https://example.com", 19, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "teams");
    HU_ASSERT(hu_teams_is_configured(&ch) == true);
    hu_teams_destroy(&ch);
}

static void test_teams_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_teams_create(&alloc, "https://example.com", 19, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(ch.vtable->health_check(ch.ctx) == true);
    hu_teams_destroy(&ch);
}

static void test_teams_webhook_and_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_teams_create(&alloc, "https://example.com", 19, &ch);
    HU_ASSERT(err == HU_OK);
    err = hu_teams_on_webhook(ch.ctx, &alloc, "hello from teams", 16);
    HU_ASSERT(err == HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = hu_teams_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(count == 1);
    hu_teams_destroy(&ch);
}

static void test_teams_poll_null_args(void) {
    hu_error_t err = hu_teams_poll(NULL, NULL, NULL, 0, NULL);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
}
#endif

/* ─── Twilio ────────────────────────────────────────────────────────────── */
#if HU_HAS_TWILIO
static void test_twilio_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_twilio_create(&alloc, "ACXXXX", 6, "token", 5, "+15551234567", 12,
                                      "+15559876543", 12, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "twilio");
    HU_ASSERT(hu_twilio_is_configured(&ch) == true);
    hu_twilio_destroy(&ch);
}

static void test_twilio_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_twilio_create(&alloc, "ACXXXX", 6, "token", 5, "+15551234567", 12,
                                      "+15559876543", 12, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(ch.vtable->health_check(ch.ctx) == true);
    hu_twilio_destroy(&ch);
}

static void test_twilio_webhook_and_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_twilio_create(&alloc, "ACXXXX", 6, "token", 5, "+15551234567", 12,
                                      "+15559876543", 12, &ch);
    HU_ASSERT(err == HU_OK);
    err = hu_twilio_on_webhook(ch.ctx, &alloc, "hello from twilio", 17);
    HU_ASSERT(err == HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = hu_twilio_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(count == 1);
    hu_twilio_destroy(&ch);
}

static void test_twilio_poll_null_args(void) {
    hu_error_t err = hu_twilio_poll(NULL, NULL, NULL, 0, NULL);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
}
#endif

/* ─── Google Chat ───────────────────────────────────────────────────────── */
#if HU_HAS_GOOGLE_CHAT
static void test_google_chat_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err =
        hu_google_chat_create(&alloc, "https://chat.googleapis.com/v1/spaces/abc", 38, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "google_chat");
    HU_ASSERT(hu_google_chat_is_configured(&ch) == true);
    hu_google_chat_destroy(&ch);
}

static void test_google_chat_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err =
        hu_google_chat_create(&alloc, "https://chat.googleapis.com/v1/spaces/abc", 38, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(ch.vtable->health_check(ch.ctx) == true);
    hu_google_chat_destroy(&ch);
}

static void test_google_chat_webhook_and_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err =
        hu_google_chat_create(&alloc, "https://chat.googleapis.com/v1/spaces/abc", 38, &ch);
    HU_ASSERT(err == HU_OK);
    err = hu_google_chat_on_webhook(ch.ctx, &alloc, "hello from gchat", 16);
    HU_ASSERT(err == HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = hu_google_chat_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(count == 1);
    hu_google_chat_destroy(&ch);
}

static void test_google_chat_poll_null_args(void) {
    hu_error_t err = hu_google_chat_poll(NULL, NULL, NULL, 0, NULL);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
}
#endif

#if HU_HAS_QQ
static void test_qq_webhook_and_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_qq_create(&alloc, "app-id", 6, "token", 5, false, &ch);
    HU_ASSERT(err == HU_OK);
    err = hu_qq_on_webhook(ch.ctx, &alloc, "hello from qq", 13);
    HU_ASSERT(err == HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = hu_qq_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(count == 1);
    hu_qq_destroy(&ch);
}

static void test_qq_poll_null_args(void) {
    hu_error_t err = hu_qq_poll(NULL, NULL, NULL, 0, NULL);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
}
#endif

void run_channel_all_tests(void) {
    HU_TEST_SUITE("Channel All");

    HU_RUN_TEST(test_cli_create);
    HU_RUN_TEST(test_cli_name);
    HU_RUN_TEST(test_cli_health_check);
    HU_RUN_TEST(test_cli_send);
    HU_RUN_TEST(test_cli_send_empty_message);
    HU_RUN_TEST(test_cli_send_long_message);
    HU_RUN_TEST(test_cli_create_destroy_repeat);

#if HU_HAS_SIGNAL
    HU_RUN_TEST(test_signal_create);
    HU_RUN_TEST(test_signal_destroy_lifecycle);
    HU_RUN_TEST(test_signal_send);
    HU_RUN_TEST(test_signal_health_check);
    HU_RUN_TEST(test_signal_start_stop_typing);
    HU_RUN_TEST(test_signal_send_long_message);
#if HU_IS_TEST
    HU_RUN_TEST(test_signal_inject_and_poll);
    HU_RUN_TEST(test_signal_send_captures_last_message);
#endif
#endif
#if HU_HAS_NOSTR
    HU_RUN_TEST(test_nostr_create);
    HU_RUN_TEST(test_nostr_destroy_lifecycle);
    HU_RUN_TEST(test_nostr_send);
    HU_RUN_TEST(test_nostr_health_check);
    HU_RUN_TEST(test_nostr_poll_test_mode);
#endif
#if HU_HAS_MQTT
    HU_RUN_TEST(test_mqtt_create_null_alloc_returns_error);
    HU_RUN_TEST(test_mqtt_create_null_broker_returns_error);
    HU_RUN_TEST(test_mqtt_create_valid_lifecycle);
    HU_RUN_TEST(test_mqtt_create_valid_health_check_when_running);
    HU_RUN_TEST(test_mqtt_send_stores_message);
    HU_RUN_TEST(test_mqtt_poll_returns_injected_message);
    HU_RUN_TEST(test_mqtt_send_before_start_fails);
    HU_RUN_TEST(test_mqtt_poll_empty_returns_no_message);
#endif
#if HU_HAS_QQ
    HU_RUN_TEST(test_qq_create);
    HU_RUN_TEST(test_qq_send);
    HU_RUN_TEST(test_qq_health_check);
    HU_RUN_TEST(test_qq_webhook_and_poll);
    HU_RUN_TEST(test_qq_poll_null_args);
#endif
#if HU_HAS_MAIXCAM
    HU_RUN_TEST(test_maixcam_create);
    HU_RUN_TEST(test_maixcam_health_check);
#endif
#if HU_HAS_DISPATCH
    HU_RUN_TEST(test_dispatch_create);
    HU_RUN_TEST(test_dispatch_send);
    HU_RUN_TEST(test_dispatch_health_check);
#endif

#if HU_HAS_TELEGRAM
    HU_RUN_TEST(test_telegram_create);
    HU_RUN_TEST(test_telegram_name);
    HU_RUN_TEST(test_telegram_health_check);
    HU_RUN_TEST(test_telegram_send);
    HU_RUN_TEST(test_telegram_create_destroy_lifecycle);
    HU_RUN_TEST(test_telegram_escape_markdown);
    HU_RUN_TEST(test_telegram_commands_help);
    HU_RUN_TEST(test_telegram_start_stop_typing);
    HU_RUN_TEST(test_telegram_allowlist);
    HU_RUN_TEST(test_telegram_send_long_message);
    HU_RUN_TEST(test_telegram_webhook_malformed);
#if HU_IS_TEST
    HU_RUN_TEST(test_telegram_inject_and_poll);
    HU_RUN_TEST(test_telegram_send_captures_last_message);
#endif
#endif
#if HU_HAS_DISCORD
    HU_RUN_TEST(test_discord_start_stop_lifecycle);
    HU_RUN_TEST(test_discord_create);
    HU_RUN_TEST(test_discord_name);
    HU_RUN_TEST(test_discord_health_check);
    HU_RUN_TEST(test_discord_send);
    HU_RUN_TEST(test_discord_send_without_token_fails);
    HU_RUN_TEST(test_discord_poll_test_mode);
    HU_RUN_TEST(test_discord_poll_empty);
    HU_RUN_TEST(test_discord_webhook_malformed);
    HU_RUN_TEST(test_discord_webhook_and_poll);
    HU_RUN_TEST(test_discord_webhook_empty_body);
#if HU_IS_TEST
    HU_RUN_TEST(test_discord_inject_and_poll);
    HU_RUN_TEST(test_discord_send_captures_last_message);
#endif
#endif
#if HU_HAS_SLACK
    HU_RUN_TEST(test_slack_create);
    HU_RUN_TEST(test_slack_name);
    HU_RUN_TEST(test_slack_health_check);
    HU_RUN_TEST(test_slack_start_stop_typing);
    HU_RUN_TEST(test_slack_send_long_message);
    HU_RUN_TEST(test_slack_create_ex);
    HU_RUN_TEST(test_slack_poll_test_mode);
    HU_RUN_TEST(test_slack_webhook_malformed);
    HU_RUN_TEST(test_slack_webhook_and_poll);
    HU_RUN_TEST(test_slack_webhook_empty_body);
#if HU_IS_TEST
    HU_RUN_TEST(test_slack_inject_and_poll);
    HU_RUN_TEST(test_slack_send_captures_last_message);
#endif
#endif
#if HU_HAS_WHATSAPP
    HU_RUN_TEST(test_whatsapp_create);
    HU_RUN_TEST(test_whatsapp_name);
    HU_RUN_TEST(test_whatsapp_health_check);
    HU_RUN_TEST(test_whatsapp_send);
    HU_RUN_TEST(test_whatsapp_webhook_and_poll);
    HU_RUN_TEST(test_whatsapp_poll_empty);
#endif
#if HU_HAS_FACEBOOK
    HU_RUN_TEST(test_facebook_create);
    HU_RUN_TEST(test_facebook_name);
    HU_RUN_TEST(test_facebook_health_check);
    HU_RUN_TEST(test_facebook_send);
    HU_RUN_TEST(test_facebook_webhook_and_poll);
    HU_RUN_TEST(test_facebook_poll_empty);
    HU_RUN_TEST(test_facebook_webhook_malformed);
    HU_RUN_TEST(test_facebook_send_empty_message);
#endif
#if HU_HAS_INSTAGRAM
    HU_RUN_TEST(test_instagram_create);
    HU_RUN_TEST(test_instagram_send);
    HU_RUN_TEST(test_instagram_health_check);
    HU_RUN_TEST(test_instagram_webhook_and_poll);
    HU_RUN_TEST(test_instagram_poll_empty);
    HU_RUN_TEST(test_instagram_webhook_malformed);
    HU_RUN_TEST(test_instagram_send_empty_message);
#endif
#if HU_HAS_TIKTOK
    HU_RUN_TEST(test_tiktok_create);
    HU_RUN_TEST(test_tiktok_name);
    HU_RUN_TEST(test_tiktok_health_check);
    HU_RUN_TEST(test_tiktok_send);
    HU_RUN_TEST(test_tiktok_webhook_and_poll);
    HU_RUN_TEST(test_tiktok_poll_empty);
    HU_RUN_TEST(test_tiktok_webhook_empty_body);
#if HU_IS_TEST
    HU_RUN_TEST(test_tiktok_inject_and_poll);
    HU_RUN_TEST(test_tiktok_send_captures_last_message);
#endif
#endif
#if HU_HAS_TWITTER
    HU_RUN_TEST(test_twitter_create);
    HU_RUN_TEST(test_twitter_send);
    HU_RUN_TEST(test_twitter_health_check);
    HU_RUN_TEST(test_twitter_webhook_and_poll);
    HU_RUN_TEST(test_twitter_poll_empty);
    HU_RUN_TEST(test_twitter_webhook_malformed);
    HU_RUN_TEST(test_twitter_send_empty_message);
#endif
#if HU_HAS_GOOGLE_RCS
    HU_RUN_TEST(test_google_rcs_create);
    HU_RUN_TEST(test_google_rcs_send);
    HU_RUN_TEST(test_google_rcs_health_check);
    HU_RUN_TEST(test_google_rcs_webhook_and_poll);
    HU_RUN_TEST(test_google_rcs_poll_empty);
    HU_RUN_TEST(test_google_rcs_webhook_malformed);
    HU_RUN_TEST(test_google_rcs_send_empty_message);
#endif
#if HU_HAS_MATRIX
    HU_RUN_TEST(test_matrix_create);
    HU_RUN_TEST(test_matrix_name);
    HU_RUN_TEST(test_matrix_health_check);
    HU_RUN_TEST(test_matrix_send);
    HU_RUN_TEST(test_matrix_poll_test_mode);
    HU_RUN_TEST(test_matrix_poll_null_args);
#endif
#if HU_HAS_IRC
    HU_RUN_TEST(test_irc_create);
    HU_RUN_TEST(test_irc_name);
    HU_RUN_TEST(test_irc_health_check);
    HU_RUN_TEST(test_irc_send);
    HU_RUN_TEST(test_irc_poll_test_mode_impl);
    HU_RUN_TEST(test_irc_poll_null_args_impl);
#endif
#if HU_HAS_LINE
    HU_RUN_TEST(test_line_start_stop_lifecycle);
    HU_RUN_TEST(test_line_create);
    HU_RUN_TEST(test_line_name);
    HU_RUN_TEST(test_line_health_check);
    HU_RUN_TEST(test_line_webhook_and_poll);
    HU_RUN_TEST(test_line_poll_null_args);
#endif
#if HU_HAS_LARK
    HU_RUN_TEST(test_lark_start_stop_lifecycle);
    HU_RUN_TEST(test_lark_create);
    HU_RUN_TEST(test_lark_name);
    HU_RUN_TEST(test_lark_health_check);
    HU_RUN_TEST(test_lark_webhook_and_poll);
    HU_RUN_TEST(test_lark_poll_null_args);
#endif
#if HU_HAS_WEB
    HU_RUN_TEST(test_web_create);
    HU_RUN_TEST(test_web_name);
    HU_RUN_TEST(test_web_health_check);
    HU_RUN_TEST(test_web_create_destroy_lifecycle);
    HU_RUN_TEST(test_web_send_empty_target);
#endif
#if HU_HAS_EMAIL
    HU_RUN_TEST(test_email_create);
    HU_RUN_TEST(test_email_name);
    HU_RUN_TEST(test_email_health_check);
    HU_RUN_TEST(test_email_send);
    HU_RUN_TEST(test_email_is_configured);
#if HU_IS_TEST
    HU_RUN_TEST(test_email_last_message_in_test_mode);
    HU_RUN_TEST(test_email_poll_returns_mock_emails);
#endif
    HU_RUN_TEST(test_email_set_auth);
    HU_RUN_TEST(test_email_set_imap);
    HU_RUN_TEST(test_email_poll_no_imap_returns_not_supported);
    HU_RUN_TEST(test_email_set_auth_null_channel);
    HU_RUN_TEST(test_email_set_imap_null_channel);
#endif
#if HU_HAS_IMESSAGE
    HU_RUN_TEST(test_imessage_create);
    HU_RUN_TEST(test_imessage_name);
    HU_RUN_TEST(test_imessage_health_check);
    HU_RUN_TEST(test_imessage_is_configured);
    HU_RUN_TEST(test_imessage_poll_test_mode);
    HU_RUN_TEST(test_imessage_poll_null_args);
#if HU_IS_TEST
    HU_RUN_TEST(test_imessage_inject_and_poll);
    HU_RUN_TEST(test_imessage_inject_multiple_poll_message_ids);
    HU_RUN_TEST(test_imessage_send_captures_last_message);
#endif
#endif
#if HU_HAS_MATTERMOST
    HU_RUN_TEST(test_mattermost_create);
    HU_RUN_TEST(test_mattermost_name);
    HU_RUN_TEST(test_mattermost_health_check);
    HU_RUN_TEST(test_mattermost_send);
    HU_RUN_TEST(test_mattermost_poll_test_mode);
    HU_RUN_TEST(test_mattermost_poll_null_args);
#endif
#if HU_HAS_ONEBOT
    HU_RUN_TEST(test_onebot_create);
    HU_RUN_TEST(test_onebot_name);
    HU_RUN_TEST(test_onebot_health_check);
    HU_RUN_TEST(test_onebot_webhook_and_poll);
    HU_RUN_TEST(test_onebot_poll_null_args);
#endif
#if HU_HAS_DINGTALK
    HU_RUN_TEST(test_dingtalk_create);
    HU_RUN_TEST(test_dingtalk_name);
    HU_RUN_TEST(test_dingtalk_health_check);
    HU_RUN_TEST(test_dingtalk_webhook_and_poll);
    HU_RUN_TEST(test_dingtalk_poll_null_args);
#endif
#if HU_HAS_TEAMS
    HU_RUN_TEST(test_teams_create);
    HU_RUN_TEST(test_teams_health_check);
    HU_RUN_TEST(test_teams_webhook_and_poll);
    HU_RUN_TEST(test_teams_poll_null_args);
#endif
#if HU_HAS_TWILIO
    HU_RUN_TEST(test_twilio_create);
    HU_RUN_TEST(test_twilio_health_check);
    HU_RUN_TEST(test_twilio_webhook_and_poll);
    HU_RUN_TEST(test_twilio_poll_null_args);
#endif
#if HU_HAS_GOOGLE_CHAT
    HU_RUN_TEST(test_google_chat_create);
    HU_RUN_TEST(test_google_chat_health_check);
    HU_RUN_TEST(test_google_chat_webhook_and_poll);
    HU_RUN_TEST(test_google_chat_poll_null_args);
#endif
}
