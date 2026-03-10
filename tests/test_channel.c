#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/channels/cli.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"
#include <string.h>

#if HU_HAS_TELEGRAM
#include "human/channels/telegram.h"
#endif
#if HU_HAS_SIGNAL
#include "human/channels/signal.h"
#endif
#if HU_HAS_NOSTR
#include "human/channels/nostr.h"
#endif
#if HU_HAS_QQ
#include "human/channels/qq.h"
#endif
#if HU_HAS_MAIXCAM
#include "human/channels/maixcam.h"
#endif
#if HU_HAS_DISPATCH
#include "human/channels/dispatch.h"
#endif
#if HU_HAS_IMAP
#include "human/channels/imap.h"
#endif
#if HU_HAS_SONATA
#include "human/channels/voice_channel.h"
#endif

static void test_cli_create_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_cli_create(&alloc, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ch.ctx);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "cli");
    hu_cli_destroy(&ch);
}

static void test_cli_start_stop(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_cli_create(&alloc, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->start(ch.ctx);
    HU_ASSERT_EQ(err, HU_OK);
    ch.vtable->stop(ch.ctx);
    hu_cli_destroy(&ch);
}

static void test_cli_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_cli_create(&alloc, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_cli_destroy(&ch);
}

static void test_cli_send(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_cli_create(&alloc, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->send(ch.ctx, NULL, 0, "hello", 5, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_cli_destroy(&ch);
}

static void test_cli_is_quit_exit(void) {
    HU_ASSERT_TRUE(hu_cli_is_quit_command("exit", 4));
    HU_ASSERT_TRUE(hu_cli_is_quit_command("  exit  ", 8));
}

static void test_cli_is_quit_quit(void) {
    HU_ASSERT_TRUE(hu_cli_is_quit_command("quit", 4));
}

static void test_cli_is_quit_colon_q(void) {
    HU_ASSERT_TRUE(hu_cli_is_quit_command(":q", 2));
}

static void test_cli_is_not_quit(void) {
    HU_ASSERT_FALSE(hu_cli_is_quit_command("hello", 5));
    HU_ASSERT_FALSE(hu_cli_is_quit_command("", 0));
}

#if HU_HAS_TELEGRAM
static void test_telegram_create_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_telegram_create(&alloc, "test:token", 10, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ch.ctx);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "telegram");
    hu_telegram_destroy(&ch);
}

static void test_telegram_send_in_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_telegram_create(&alloc, "test:token", 10, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->send(ch.ctx, "123", 3, "hello", 5, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_telegram_destroy(&ch);
}

static void test_telegram_create_ex_with_allowlist(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    const char *allow[] = {"*"};
    hu_error_t err = hu_telegram_create_ex(&alloc, "tok", 3, allow, 1, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ch.ctx);
    hu_telegram_destroy(&ch);
}

static void test_telegram_commands_help(void) {
    const char *help = hu_telegram_commands_help();
    HU_ASSERT_NOT_NULL(help);
    HU_ASSERT_TRUE(strstr(help, "/help") != NULL);
    HU_ASSERT_TRUE(strstr(help, "/start") != NULL);
}

static void test_telegram_escape_markdown_v2(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t out_len = 0;
    char *esc = hu_telegram_escape_markdown_v2(&alloc, "a_b", 3, &out_len);
    HU_ASSERT_NOT_NULL(esc);
    HU_ASSERT_TRUE(strstr(esc, "\\_") != NULL);
    alloc.free(alloc.ctx, esc, out_len + 1);
}

static void test_telegram_poll_in_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_telegram_create(&alloc, "t", 1, &ch);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 0;
    hu_error_t err = hu_telegram_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0);
    hu_telegram_destroy(&ch);
}

static void test_telegram_start_stop_typing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_telegram_create(&alloc, "t", 1, &ch);
    if (ch.vtable->start_typing) {
        hu_error_t err = ch.vtable->start_typing(ch.ctx, "123", 3);
        HU_ASSERT_EQ(err, HU_OK);
    }
    if (ch.vtable->stop_typing) {
        hu_error_t err = ch.vtable->stop_typing(ch.ctx, "123", 3);
        HU_ASSERT_EQ(err, HU_OK);
    }
    hu_telegram_destroy(&ch);
}
#endif

#if HU_HAS_SIGNAL
static void test_signal_create_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_signal_create(&alloc, "http://localhost", 16, "test", 4, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ch.ctx);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "signal");
    hu_signal_destroy(&ch);
}
static void test_signal_send_in_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_signal_create(&alloc, "http://localhost", 16, "test", 4, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->send(ch.ctx, "recipient", 9, "hello", 5, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_signal_destroy(&ch);
}
static void test_signal_create_ex_with_policy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    const char *allow[] = {"+111", "*"};
    const char *group_allow[] = {"+222"};
    hu_error_t err = hu_signal_create_ex(&alloc, "http://localhost:8080", 21, "+1234567890", 11,
                                         allow, 2, group_allow, 1, HU_SIGNAL_GROUP_POLICY_ALLOWLIST,
                                         strlen(HU_SIGNAL_GROUP_POLICY_ALLOWLIST), &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ch.ctx);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "signal");
    hu_signal_destroy(&ch);
}
static void test_signal_health_check_in_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_signal_create(&alloc, "http://localhost", 16, "test", 4, &ch);
    HU_ASSERT_TRUE(ch.vtable->start(ch.ctx) == HU_OK);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    ch.vtable->stop(ch.ctx);
    hu_signal_destroy(&ch);
}
static void test_signal_poll_in_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_signal_create(&alloc, "http://localhost", 16, "test", 4, &ch);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 0;
    hu_error_t err = hu_signal_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0);
    hu_signal_destroy(&ch);
}
static void test_signal_start_stop_typing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_signal_create(&alloc, "http://localhost", 16, "test", 4, &ch);
    if (ch.vtable->start_typing) {
        HU_ASSERT_EQ(ch.vtable->start_typing(ch.ctx, "recipient", 9), HU_OK);
    }
    if (ch.vtable->stop_typing) {
        HU_ASSERT_EQ(ch.vtable->stop_typing(ch.ctx, "recipient", 9), HU_OK);
    }
    hu_signal_destroy(&ch);
}
#endif

#if HU_HAS_NOSTR
static void test_nostr_create_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_nostr_create(&alloc, "/tmp/nak", 8, "npub1abc", 8, NULL, 0, NULL, 0, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ch.ctx);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "nostr");
    hu_nostr_destroy(&ch);
}
static void test_nostr_send_in_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_nostr_create(&alloc, "/tmp/nak", 8, "npub1abc", 8, NULL, 0, NULL, 0, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->send(ch.ctx, NULL, 0, "hello", 5, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_nostr_destroy(&ch);
}

static void test_nostr_is_configured(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_nostr_create(&alloc, "/tmp/nak", 8, "npub1", 5, NULL, 0, NULL, 0, &ch);
    HU_ASSERT_FALSE(hu_nostr_is_configured(&ch));
    hu_nostr_destroy(&ch);

    hu_nostr_create(&alloc, "/tmp/nak", 8, "npub1", 5, "wss://relay.example", 17, "aabbcc", 6, &ch);
    HU_ASSERT_TRUE(hu_nostr_is_configured(&ch));
    hu_nostr_destroy(&ch);
}

static void test_nostr_last_message_in_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_nostr_create(&alloc, "/tmp/nak", 8, "npub1", 5, NULL, 0, NULL, 0, &ch);
    ch.vtable->send(ch.ctx, "abc", 3, "test message", 12, NULL, 0);
#if HU_IS_TEST
    const char *last = hu_nostr_test_last_message(&ch);
    HU_ASSERT_NOT_NULL(last);
    HU_ASSERT_TRUE(strstr(last, "test message") != NULL);
    HU_ASSERT_TRUE(strstr(last, "\"kind\":4") != NULL);
#endif
    hu_nostr_destroy(&ch);
}

#if HU_IS_TEST
static void test_nostr_poll_returns_mock_events(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_nostr_create(&alloc, "/tmp/nak", 8, "npub1", 5, "wss://r", 6, "sec", 3, &ch);
    hu_error_t err = hu_nostr_test_inject_mock_event(&ch, "npub1abc", 8, "hello from mock", 15);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = hu_nostr_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_STR_EQ(msgs[0].session_key, "npub1abc");
    HU_ASSERT_STR_EQ(msgs[0].content, "hello from mock");
    hu_nostr_destroy(&ch);
}
#endif
#endif

#if HU_HAS_QQ
static void test_qq_create_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_qq_create(&alloc, "app123", 6, "token", 5, false, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ch.ctx);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "qq");
    hu_qq_destroy(&ch);
}
static void test_qq_send_in_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_qq_create(&alloc, "app123", 6, "token", 5, false, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->send(ch.ctx, "channel", 7, "hello", 5, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_qq_destroy(&ch);
}
#endif

#if HU_HAS_MAIXCAM
static void test_maixcam_create_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_maixcam_create(&alloc, "localhost", 9, 8080, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ch.ctx);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "maixcam");
    hu_maixcam_destroy(&ch);
}
static void test_maixcam_send_in_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_maixcam_create(&alloc, "localhost", 9, 8080, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->send(ch.ctx, NULL, 0, "hello", 5, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_maixcam_destroy(&ch);
}
#endif

#if HU_HAS_DISPATCH
static void test_dispatch_create_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_dispatch_create(&alloc, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ch.ctx);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "dispatch");
    hu_dispatch_destroy(&ch);
}
static void test_dispatch_send_in_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_dispatch_create(&alloc, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->send(ch.ctx, NULL, 0, "hello", 5, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_dispatch_destroy(&ch);
}
#endif

#if HU_HAS_IMAP
static void test_imap_create_with_mock_config(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_imap_config_t cfg = {
        .imap_host = "imap.example.com",
        .imap_host_len = 16,
        .imap_port = 993,
        .imap_username = "user",
        .imap_username_len = 4,
        .imap_password = "secret",
        .imap_password_len = 6,
        .imap_folder = "INBOX",
        .imap_folder_len = 5,
        .imap_use_tls = true,
    };
    hu_error_t err = hu_imap_create(&alloc, &cfg, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ch.ctx);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "imap");
    HU_ASSERT_TRUE(hu_imap_is_configured(&ch));
    hu_imap_destroy(&ch);
}

static void test_imap_send_stores_in_outbox(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_imap_config_t cfg = {
        .imap_host = "imap.example.com",
        .imap_host_len = 16,
        .imap_port = 993,
        .imap_username = "user",
        .imap_username_len = 4,
        .imap_password = "secret",
        .imap_password_len = 6,
        .imap_folder = "INBOX",
        .imap_folder_len = 5,
        .imap_use_tls = true,
    };
    hu_error_t err = hu_imap_create(&alloc, &cfg, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->send(ch.ctx, "alice@example.com", 17, "Hello", 5, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_imap_destroy(&ch);
}

static void test_imap_unconfigured_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_imap_config_t cfg = {0}; /* no host, username, password */
    hu_error_t err = hu_imap_create(&alloc, &cfg, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(ch.vtable->health_check(ch.ctx));
    hu_imap_destroy(&ch);
}

#if HU_IS_TEST
static void test_imap_poll_returns_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_imap_config_t cfg = {
        .imap_host = "imap.example.com",
        .imap_host_len = 16,
        .imap_port = 993,
        .imap_username = "user",
        .imap_username_len = 4,
        .imap_password = "secret",
        .imap_password_len = 6,
        .imap_folder = "INBOX",
        .imap_folder_len = 5,
        .imap_use_tls = true,
    };
    hu_error_t err = hu_imap_create(&alloc, &cfg, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_imap_test_push_mock(&ch, "sess1", 5, "mock body", 9);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = hu_imap_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_STR_EQ(msgs[0].session_key, "sess1");
    HU_ASSERT_STR_EQ(msgs[0].content, "mock body");
    hu_imap_destroy(&ch);
}
#endif
#endif

/* ── Voice channel vtable tests ──────────────────────────────── */
#if HU_HAS_SONATA
static void test_voice_channel_vtable_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_channel_voice_create(&alloc, NULL, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "voice");
    hu_channel_voice_destroy(&ch);
}

static void test_voice_channel_start_stop(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_channel_voice_create(&alloc, NULL, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->start(ch.ctx);
    HU_ASSERT_EQ(err, HU_OK);
    ch.vtable->stop(ch.ctx);
    hu_channel_voice_destroy(&ch);
}

static void test_voice_channel_send(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_channel_voice_create(&alloc, NULL, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->send(ch.ctx, "user1", 5, "hello", 5, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_voice_destroy(&ch);
}

static void test_voice_channel_health_check_before_start(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_channel_voice_create(&alloc, NULL, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(ch.vtable->health_check(ch.ctx));
    hu_channel_voice_destroy(&ch);
}
#endif

void run_channel_tests(void) {
    HU_TEST_SUITE("Channel");
    HU_RUN_TEST(test_cli_create_succeeds);
    HU_RUN_TEST(test_cli_start_stop);
    HU_RUN_TEST(test_cli_health_check);
    HU_RUN_TEST(test_cli_send);
    HU_RUN_TEST(test_cli_is_quit_exit);
    HU_RUN_TEST(test_cli_is_quit_quit);
    HU_RUN_TEST(test_cli_is_quit_colon_q);
    HU_RUN_TEST(test_cli_is_not_quit);
#if HU_HAS_TELEGRAM
    HU_RUN_TEST(test_telegram_create_succeeds);
    HU_RUN_TEST(test_telegram_send_in_test_mode);
    HU_RUN_TEST(test_telegram_create_ex_with_allowlist);
    HU_RUN_TEST(test_telegram_commands_help);
    HU_RUN_TEST(test_telegram_escape_markdown_v2);
    HU_RUN_TEST(test_telegram_poll_in_test_mode);
    HU_RUN_TEST(test_telegram_start_stop_typing);
#endif
#if HU_HAS_SIGNAL
    HU_RUN_TEST(test_signal_create_succeeds);
    HU_RUN_TEST(test_signal_send_in_test_mode);
    HU_RUN_TEST(test_signal_create_ex_with_policy);
    HU_RUN_TEST(test_signal_health_check_in_test_mode);
    HU_RUN_TEST(test_signal_poll_in_test_mode);
    HU_RUN_TEST(test_signal_start_stop_typing);
#endif
#if HU_HAS_NOSTR
    HU_RUN_TEST(test_nostr_create_succeeds);
    HU_RUN_TEST(test_nostr_send_in_test_mode);
    HU_RUN_TEST(test_nostr_is_configured);
#if HU_IS_TEST
    HU_RUN_TEST(test_nostr_last_message_in_test_mode);
    HU_RUN_TEST(test_nostr_poll_returns_mock_events);
#endif
#endif
#if HU_HAS_QQ
    HU_RUN_TEST(test_qq_create_succeeds);
    HU_RUN_TEST(test_qq_send_in_test_mode);
#endif
#if HU_HAS_MAIXCAM
    HU_RUN_TEST(test_maixcam_create_succeeds);
    HU_RUN_TEST(test_maixcam_send_in_test_mode);
#endif
#if HU_HAS_IMAP
    HU_RUN_TEST(test_imap_create_with_mock_config);
    HU_RUN_TEST(test_imap_send_stores_in_outbox);
    HU_RUN_TEST(test_imap_unconfigured_health_check);
#if HU_IS_TEST
    HU_RUN_TEST(test_imap_poll_returns_mock);
#endif
#endif
#if HU_HAS_SONATA
    HU_RUN_TEST(test_voice_channel_vtable_name);
    HU_RUN_TEST(test_voice_channel_start_stop);
    HU_RUN_TEST(test_voice_channel_send);
    HU_RUN_TEST(test_voice_channel_health_check_before_start);
#endif
}
