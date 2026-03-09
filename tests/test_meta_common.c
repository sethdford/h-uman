/* Meta Graph API common layer tests. */
#include "seaclaw/channels/meta_common.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "test_framework.h"
#include <string.h>

static void test_meta_verify_webhook_test_mode(void) {
    /* Under SC_IS_TEST, always returns true. */
    bool ok = sc_meta_verify_webhook("body", 4, "sha256=abc", "secret");
    SC_ASSERT_TRUE(ok);
}

static void test_meta_graph_url_basic(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *url = NULL;
    size_t url_len = 0;
    sc_error_t err = sc_meta_graph_url(&alloc, "me/messages", 11, &url, &url_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(url);
    SC_ASSERT_TRUE(url_len >= 35);
    SC_ASSERT_STR_EQ(url, "https://graph.facebook.com/v21.0/me/messages");
    alloc.free(alloc.ctx, url, url_len + 1);
}

static void test_meta_graph_url_null_args(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *url = NULL;
    size_t url_len = 0;
    sc_error_t err = sc_meta_graph_url(NULL, "me/messages", 11, &url, &url_len);
    SC_ASSERT_NEQ(err, SC_OK);
    err = sc_meta_graph_url(&alloc, NULL, 11, &url, &url_len);
    SC_ASSERT_NEQ(err, SC_OK);
    err = sc_meta_graph_url(&alloc, "me/messages", 11, NULL, &url_len);
    SC_ASSERT_NEQ(err, SC_OK);
    err = sc_meta_graph_url(&alloc, "me/messages", 11, &url, NULL);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_meta_verify_webhook_null_body(void) {
    bool ok = sc_meta_verify_webhook(NULL, 0, "sha256=abc", "secret");
    SC_ASSERT_TRUE(ok);
}

static void test_meta_verify_webhook_null_signature(void) {
    bool ok = sc_meta_verify_webhook("body", 4, NULL, "secret");
    SC_ASSERT_TRUE(ok);
}

static void test_meta_graph_url_empty_path(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *url = NULL;
    size_t url_len = 0;
    sc_error_t err = sc_meta_graph_url(&alloc, "", 0, &url, &url_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(url);
    alloc.free(alloc.ctx, url, url_len + 1);
}

static void test_meta_graph_url_long_path(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char long_path[300];
    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    char *url = NULL;
    size_t url_len = 0;
    sc_error_t err = sc_meta_graph_url(&alloc, long_path, sizeof(long_path) - 1, &url, &url_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(url);
    SC_ASSERT_TRUE(url_len > 300);
    alloc.free(alloc.ctx, url, url_len + 1);
}

void run_meta_common_tests(void) {
    SC_TEST_SUITE("Meta Common");
    SC_RUN_TEST(test_meta_verify_webhook_test_mode);
    SC_RUN_TEST(test_meta_graph_url_basic);
    SC_RUN_TEST(test_meta_graph_url_null_args);
    SC_RUN_TEST(test_meta_verify_webhook_null_body);
    SC_RUN_TEST(test_meta_verify_webhook_null_signature);
    SC_RUN_TEST(test_meta_graph_url_empty_path);
    SC_RUN_TEST(test_meta_graph_url_long_path);
}
