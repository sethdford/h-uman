#if SC_HAS_GMAIL
#include "seaclaw/channels/gmail.h"
#include "seaclaw/channel.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/channel_loop.h"
#include "test_framework.h"
#include <string.h>

static void test_gmail_create_destroy(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_gmail_create(&alloc, "client_id", 6, "client_secret", 13, "refresh_token",
                                    13, 60, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(ch.ctx);
    SC_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "gmail");
    sc_gmail_destroy(&ch);
}

static void test_gmail_create_null_alloc(void) {
    sc_channel_t ch;
    sc_error_t err = sc_gmail_create(NULL, "client_id", 6, "client_secret", 13, "refresh_token",
                                    13, 60, &ch);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_gmail_create_null_out(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_error_t err = sc_gmail_create(&alloc, "client_id", 6, "client_secret", 13, "refresh_token",
                                    13, 60, NULL);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_gmail_send_not_supported(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_gmail_create(&alloc, "client_id", 6, "client_secret", 13, "refresh_token", 13, 60, &ch);
    sc_error_t err = ch.vtable->send(ch.ctx, "target", 6, "message", 7, NULL, 0);
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
    sc_gmail_destroy(&ch);
}

static void test_gmail_health_check_no_token(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_gmail_create(&alloc, "client_id", 6, "client_secret", 13, "refresh_token", 13, 60, &ch);
    bool ok = ch.vtable->health_check(ch.ctx);
    SC_ASSERT_FALSE(ok);
    sc_gmail_destroy(&ch);
}

static void test_gmail_poll_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_gmail_create(&alloc, "client_id", 6, "client_secret", 13, "refresh_token", 13, 60, &ch);
    sc_channel_loop_msg_t msgs[4];
    size_t out_count = 99;
    sc_error_t err = sc_gmail_poll(ch.ctx, &alloc, msgs, 4, &out_count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out_count, 0u);
    sc_gmail_destroy(&ch);
}

static void test_gmail_start_stop(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_gmail_create(&alloc, "client_id", 6, "client_secret", 13, "refresh_token", 13, 60, &ch);
    sc_error_t err = ch.vtable->start(ch.ctx);
    SC_ASSERT_EQ(err, SC_OK);
    ch.vtable->stop(ch.ctx);
    sc_gmail_destroy(&ch);
}

static void test_gmail_is_configured(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_gmail_create(&alloc, "client_id", 6, "client_secret", 13, "refresh_token", 13, 60, &ch);
    SC_ASSERT_TRUE(sc_gmail_is_configured(&ch));
    sc_gmail_destroy(&ch);

    sc_gmail_create(&alloc, NULL, 0, NULL, 0, NULL, 0, 60, &ch);
    SC_ASSERT_FALSE(sc_gmail_is_configured(&ch));
    sc_gmail_destroy(&ch);
}

void run_gmail_tests(void) {
    SC_TEST_SUITE("Gmail");

    SC_RUN_TEST(test_gmail_create_destroy);
    SC_RUN_TEST(test_gmail_create_null_alloc);
    SC_RUN_TEST(test_gmail_create_null_out);
    SC_RUN_TEST(test_gmail_send_not_supported);
    SC_RUN_TEST(test_gmail_health_check_no_token);
    SC_RUN_TEST(test_gmail_poll_test_mode);
    SC_RUN_TEST(test_gmail_start_stop);
    SC_RUN_TEST(test_gmail_is_configured);
}
#else
void run_gmail_tests(void) {
    (void)0; /* Gmail channel not built */
}
#endif
