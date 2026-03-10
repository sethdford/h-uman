#if HU_HAS_GMAIL
#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/channels/gmail.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"
#include <string.h>

/* Declare internal functions for testing */
extern hu_error_t base64url_decode(const char *in, size_t in_len, char *out, size_t out_cap,
                                   size_t *out_len);

static void test_gmail_base64url_basic(void) {
    char out[256];
    size_t out_len = 0;
    /* "SGVsbG8gV29ybGQ" is base64url for "Hello World" */
    hu_error_t err = base64url_decode("SGVsbG8gV29ybGQ", 15, out, sizeof(out), &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out_len, 11u);
    out[out_len] = '\0';
    HU_ASSERT_STR_EQ(out, "Hello World");
}

static void test_gmail_base64url_padding(void) {
    char out[256];
    size_t out_len = 0;
    /* "dGVzdA" is base64url for "test" (no padding) */
    hu_error_t err = base64url_decode("dGVzdA", 6, out, sizeof(out), &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out_len, 4u);
    out[out_len] = '\0';
    HU_ASSERT_STR_EQ(out, "test");
}

static void test_gmail_base64url_empty(void) {
    char out[16];
    size_t out_len = 99;
    hu_error_t err = base64url_decode("", 0, out, sizeof(out), &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out_len, 0u);
}

static void test_gmail_base64url_overflow(void) {
    char out[4]; /* too small for "Hello World" */
    size_t out_len = 0;
    hu_error_t err = base64url_decode("SGVsbG8gV29ybGQ", 15, out, sizeof(out), &out_len);
    HU_ASSERT_TRUE(err != HU_OK); /* should fail on overflow */
}

static void test_gmail_base64url_url_chars(void) {
    char out[256];
    size_t out_len = 0;
    /* base64url uses - instead of + and _ instead of / */
    /* "A-B_" is valid base64url */
    hu_error_t err = base64url_decode("A-B_", 4, out, sizeof(out), &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out_len, 3u);
}

static void test_gmail_create_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err =
        hu_gmail_create(&alloc, "client_id", 6, "client_secret", 13, "refresh_token", 13, 60, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ch.ctx);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "gmail");
    hu_gmail_destroy(&ch);
}

static void test_gmail_create_null_alloc(void) {
    hu_channel_t ch;
    hu_error_t err =
        hu_gmail_create(NULL, "client_id", 6, "client_secret", 13, "refresh_token", 13, 60, &ch);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_gmail_create_null_out(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err =
        hu_gmail_create(&alloc, "client_id", 6, "client_secret", 13, "refresh_token", 13, 60, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_gmail_send_is_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_gmail_create(&alloc, "client_id", 6, "client_secret", 13, "refresh_token", 13, 60, &ch);
    HU_ASSERT_NULL(ch.vtable->send);
    hu_gmail_destroy(&ch);
}

static void test_gmail_health_check_no_token(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_gmail_create(&alloc, "client_id", 6, "client_secret", 13, "refresh_token", 13, 60, &ch);
    bool ok = ch.vtable->health_check(ch.ctx);
    HU_ASSERT_FALSE(ok);
    hu_gmail_destroy(&ch);
}

static void test_gmail_poll_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_gmail_create(&alloc, "client_id", 6, "client_secret", 13, "refresh_token", 13, 60, &ch);
    hu_channel_loop_msg_t msgs[4];
    size_t out_count = 99;
    hu_error_t err = hu_gmail_poll(ch.ctx, &alloc, msgs, 4, &out_count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out_count, 0u);
    hu_gmail_destroy(&ch);
}

static void test_gmail_start_stop(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_gmail_create(&alloc, "client_id", 6, "client_secret", 13, "refresh_token", 13, 60, &ch);
    hu_error_t err = ch.vtable->start(ch.ctx);
    HU_ASSERT_EQ(err, HU_OK);
    ch.vtable->stop(ch.ctx);
    hu_gmail_destroy(&ch);
}

static void test_gmail_is_configured(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_gmail_create(&alloc, "client_id", 6, "client_secret", 13, "refresh_token", 13, 60, &ch);
    HU_ASSERT_TRUE(hu_gmail_is_configured(&ch));
    hu_gmail_destroy(&ch);

    hu_gmail_create(&alloc, NULL, 0, NULL, 0, NULL, 0, 60, &ch);
    HU_ASSERT_FALSE(hu_gmail_is_configured(&ch));
    hu_gmail_destroy(&ch);
}

void run_gmail_tests(void) {
    HU_TEST_SUITE("Gmail");

    HU_RUN_TEST(test_gmail_base64url_basic);
    HU_RUN_TEST(test_gmail_base64url_padding);
    HU_RUN_TEST(test_gmail_base64url_empty);
    HU_RUN_TEST(test_gmail_base64url_overflow);
    HU_RUN_TEST(test_gmail_base64url_url_chars);
    HU_RUN_TEST(test_gmail_create_destroy);
    HU_RUN_TEST(test_gmail_create_null_alloc);
    HU_RUN_TEST(test_gmail_create_null_out);
    HU_RUN_TEST(test_gmail_send_is_null);
    HU_RUN_TEST(test_gmail_health_check_no_token);
    HU_RUN_TEST(test_gmail_poll_test_mode);
    HU_RUN_TEST(test_gmail_start_stop);
    HU_RUN_TEST(test_gmail_is_configured);
}
#else
void run_gmail_tests(void) {
    (void)0; /* Gmail channel not built */
}
#endif
