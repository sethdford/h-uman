#include "test_framework.h"
#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/channels/cli.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <string.h>

#if SC_HAS_TELEGRAM
#include "seaclaw/channels/telegram.h"
#endif
#if SC_HAS_SIGNAL
#include "seaclaw/channels/signal.h"
#endif
#if SC_HAS_NOSTR
#include "seaclaw/channels/nostr.h"
#endif
#if SC_HAS_QQ
#include "seaclaw/channels/qq.h"
#endif
#if SC_HAS_MAIXCAM
#include "seaclaw/channels/maixcam.h"
#endif
#if SC_HAS_DISPATCH
#include "seaclaw/channels/dispatch.h"
#endif

static void test_cli_create_succeeds(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_cli_create(&alloc, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(ch.ctx);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "cli");
    sc_cli_destroy(&ch);
}

static void test_cli_start_stop(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_cli_create(&alloc, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    err = ch.vtable->start(ch.ctx);
    SC_ASSERT_EQ(err, SC_OK);
    ch.vtable->stop(ch.ctx);
    sc_cli_destroy(&ch);
}

static void test_cli_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_cli_create(&alloc, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    sc_cli_destroy(&ch);
}

static void test_cli_send(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_cli_create(&alloc, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    err = ch.vtable->send(ch.ctx, NULL, 0, "hello", 5, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_cli_destroy(&ch);
}

static void test_cli_is_quit_exit(void) {
    SC_ASSERT_TRUE(sc_cli_is_quit_command("exit", 4));
    SC_ASSERT_TRUE(sc_cli_is_quit_command("  exit  ", 8));
}

static void test_cli_is_quit_quit(void) {
    SC_ASSERT_TRUE(sc_cli_is_quit_command("quit", 4));
}

static void test_cli_is_quit_colon_q(void) {
    SC_ASSERT_TRUE(sc_cli_is_quit_command(":q", 2));
}

static void test_cli_is_not_quit(void) {
    SC_ASSERT_FALSE(sc_cli_is_quit_command("hello", 5));
    SC_ASSERT_FALSE(sc_cli_is_quit_command("", 0));
}

#if SC_HAS_TELEGRAM
static void test_telegram_create_succeeds(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_telegram_create(&alloc, "test:token", 10, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(ch.ctx);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "telegram");
    sc_telegram_destroy(&ch);
}

static void test_telegram_send_in_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_telegram_create(&alloc, "test:token", 10, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    err = ch.vtable->send(ch.ctx, "123", 3, "hello", 5, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_telegram_destroy(&ch);
}

static void test_telegram_create_ex_with_allowlist(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    const char *allow[] = {"*"};
    sc_error_t err = sc_telegram_create_ex(&alloc, "tok", 3, allow, 1, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(ch.ctx);
    sc_telegram_destroy(&ch);
}

static void test_telegram_commands_help(void) {
    const char *help = sc_telegram_commands_help();
    SC_ASSERT_NOT_NULL(help);
    SC_ASSERT_TRUE(strstr(help, "/help") != NULL);
    SC_ASSERT_TRUE(strstr(help, "/start") != NULL);
}

static void test_telegram_escape_markdown_v2(void) {
    sc_allocator_t alloc = sc_system_allocator();
    size_t out_len = 0;
    char *esc = sc_telegram_escape_markdown_v2(&alloc, "a_b", 3, &out_len);
    SC_ASSERT_NOT_NULL(esc);
    SC_ASSERT_TRUE(strstr(esc, "\\_") != NULL);
    alloc.free(alloc.ctx, esc, out_len + 1);
}

static void test_telegram_poll_in_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_telegram_create(&alloc, "t", 1, &ch);
    sc_channel_loop_msg_t msgs[4];
    size_t count = 0;
    sc_error_t err = sc_telegram_poll(ch.ctx, &alloc, msgs, 4, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 0);
    sc_telegram_destroy(&ch);
}

static void test_telegram_start_stop_typing(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_telegram_create(&alloc, "t", 1, &ch);
    if (ch.vtable->start_typing) {
        sc_error_t err = ch.vtable->start_typing(ch.ctx, "123", 3);
        SC_ASSERT_EQ(err, SC_OK);
    }
    if (ch.vtable->stop_typing) {
        sc_error_t err = ch.vtable->stop_typing(ch.ctx, "123", 3);
        SC_ASSERT_EQ(err, SC_OK);
    }
    sc_telegram_destroy(&ch);
}
#endif

#if SC_HAS_SIGNAL
static void test_signal_create_succeeds(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_signal_create(&alloc, "http://localhost", 16, "test", 4, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(ch.ctx);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "signal");
    sc_signal_destroy(&ch);
}
static void test_signal_send_in_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_signal_create(&alloc, "http://localhost", 16, "test", 4, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    err = ch.vtable->send(ch.ctx, "recipient", 9, "hello", 5, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_signal_destroy(&ch);
}
static void test_signal_create_ex_with_policy(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    const char *allow[] = {"+111", "*"};
    const char *group_allow[] = {"+222"};
    sc_error_t err = sc_signal_create_ex(&alloc, "http://localhost:8080", 21,
        "+1234567890", 11, allow, 2, group_allow, 1,
        SC_SIGNAL_GROUP_POLICY_ALLOWLIST, strlen(SC_SIGNAL_GROUP_POLICY_ALLOWLIST), &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(ch.ctx);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "signal");
    sc_signal_destroy(&ch);
}
static void test_signal_health_check_in_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_signal_create(&alloc, "http://localhost", 16, "test", 4, &ch);
    SC_ASSERT_TRUE(ch.vtable->start(ch.ctx) == SC_OK);
    SC_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    ch.vtable->stop(ch.ctx);
    sc_signal_destroy(&ch);
}
static void test_signal_poll_in_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_signal_create(&alloc, "http://localhost", 16, "test", 4, &ch);
    sc_channel_loop_msg_t msgs[4];
    size_t count = 0;
    sc_error_t err = sc_signal_poll(ch.ctx, &alloc, msgs, 4, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 0);
    sc_signal_destroy(&ch);
}
static void test_signal_start_stop_typing(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_signal_create(&alloc, "http://localhost", 16, "test", 4, &ch);
    if (ch.vtable->start_typing) {
        SC_ASSERT_EQ(ch.vtable->start_typing(ch.ctx, "recipient", 9), SC_OK);
    }
    if (ch.vtable->stop_typing) {
        SC_ASSERT_EQ(ch.vtable->stop_typing(ch.ctx, "recipient", 9), SC_OK);
    }
    sc_signal_destroy(&ch);
}
#endif

#if SC_HAS_NOSTR
static void test_nostr_create_succeeds(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_nostr_create(&alloc, "/tmp/nak", 8, "npub1abc", 8, NULL, 0, NULL, 0, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(ch.ctx);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "nostr");
    sc_nostr_destroy(&ch);
}
static void test_nostr_send_in_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_nostr_create(&alloc, "/tmp/nak", 8, "npub1abc", 8, NULL, 0, NULL, 0, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    err = ch.vtable->send(ch.ctx, NULL, 0, "hello", 5, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_nostr_destroy(&ch);
}

static void test_nostr_is_configured(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_nostr_create(&alloc, "/tmp/nak", 8, "npub1", 5, NULL, 0, NULL, 0, &ch);
    SC_ASSERT_FALSE(sc_nostr_is_configured(&ch));
    sc_nostr_destroy(&ch);

    sc_nostr_create(&alloc, "/tmp/nak", 8, "npub1", 5, "wss://relay.example", 17, "aabbcc", 6, &ch);
    SC_ASSERT_TRUE(sc_nostr_is_configured(&ch));
    sc_nostr_destroy(&ch);
}

static void test_nostr_last_message_in_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_nostr_create(&alloc, "/tmp/nak", 8, "npub1", 5, NULL, 0, NULL, 0, &ch);
    ch.vtable->send(ch.ctx, "abc", 3, "test message", 12, NULL, 0);
#if SC_IS_TEST
    const char *last = sc_nostr_test_last_message(&ch);
    SC_ASSERT_NOT_NULL(last);
    SC_ASSERT_STR_EQ(last, "test message");
#endif
    sc_nostr_destroy(&ch);
}
#endif

#if SC_HAS_QQ
static void test_qq_create_succeeds(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_qq_create(&alloc, "app123", 6, "token", 5, false, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(ch.ctx);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "qq");
    sc_qq_destroy(&ch);
}
static void test_qq_send_in_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_qq_create(&alloc, "app123", 6, "token", 5, false, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    err = ch.vtable->send(ch.ctx, "channel", 7, "hello", 5, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_qq_destroy(&ch);
}
#endif

#if SC_HAS_MAIXCAM
static void test_maixcam_create_succeeds(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_maixcam_create(&alloc, "localhost", 9, 8080, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(ch.ctx);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "maixcam");
    sc_maixcam_destroy(&ch);
}
static void test_maixcam_send_in_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_maixcam_create(&alloc, "localhost", 9, 8080, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    err = ch.vtable->send(ch.ctx, NULL, 0, "hello", 5, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_maixcam_destroy(&ch);
}
#endif

#if SC_HAS_DISPATCH
static void test_dispatch_create_succeeds(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_dispatch_create(&alloc, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(ch.ctx);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "dispatch");
    sc_dispatch_destroy(&ch);
}
static void test_dispatch_send_in_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_dispatch_create(&alloc, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    err = ch.vtable->send(ch.ctx, NULL, 0, "hello", 5, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_dispatch_destroy(&ch);
}
#endif

void run_channel_tests(void) {
    SC_TEST_SUITE("Channel");
    SC_RUN_TEST(test_cli_create_succeeds);
    SC_RUN_TEST(test_cli_start_stop);
    SC_RUN_TEST(test_cli_health_check);
    SC_RUN_TEST(test_cli_send);
    SC_RUN_TEST(test_cli_is_quit_exit);
    SC_RUN_TEST(test_cli_is_quit_quit);
    SC_RUN_TEST(test_cli_is_quit_colon_q);
    SC_RUN_TEST(test_cli_is_not_quit);
#if SC_HAS_TELEGRAM
    SC_RUN_TEST(test_telegram_create_succeeds);
    SC_RUN_TEST(test_telegram_send_in_test_mode);
    SC_RUN_TEST(test_telegram_create_ex_with_allowlist);
    SC_RUN_TEST(test_telegram_commands_help);
    SC_RUN_TEST(test_telegram_escape_markdown_v2);
    SC_RUN_TEST(test_telegram_poll_in_test_mode);
    SC_RUN_TEST(test_telegram_start_stop_typing);
#endif
#if SC_HAS_SIGNAL
    SC_RUN_TEST(test_signal_create_succeeds);
    SC_RUN_TEST(test_signal_send_in_test_mode);
    SC_RUN_TEST(test_signal_create_ex_with_policy);
    SC_RUN_TEST(test_signal_health_check_in_test_mode);
    SC_RUN_TEST(test_signal_poll_in_test_mode);
    SC_RUN_TEST(test_signal_start_stop_typing);
#endif
#if SC_HAS_NOSTR
    SC_RUN_TEST(test_nostr_create_succeeds);
    SC_RUN_TEST(test_nostr_send_in_test_mode);
    SC_RUN_TEST(test_nostr_is_configured);
#if SC_IS_TEST
    SC_RUN_TEST(test_nostr_last_message_in_test_mode);
#endif
#endif
#if SC_HAS_QQ
    SC_RUN_TEST(test_qq_create_succeeds);
    SC_RUN_TEST(test_qq_send_in_test_mode);
#endif
#if SC_HAS_MAIXCAM
    SC_RUN_TEST(test_maixcam_create_succeeds);
    SC_RUN_TEST(test_maixcam_send_in_test_mode);
#endif
#if SC_HAS_DISPATCH
    SC_RUN_TEST(test_dispatch_create_succeeds);
    SC_RUN_TEST(test_dispatch_send_in_test_mode);
#endif
}
