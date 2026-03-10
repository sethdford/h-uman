/* Meta Graph API common layer tests. */
#include "human/channels/meta_common.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"
#include <string.h>

static void test_meta_verify_webhook_test_mode(void) {
    /* Under HU_IS_TEST, always returns true. */
    bool ok = hu_meta_verify_webhook("body", 4, "sha256=abc", "secret");
    HU_ASSERT_TRUE(ok);
}

static void test_meta_graph_url_basic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *url = NULL;
    size_t url_len = 0;
    hu_error_t err = hu_meta_graph_url(&alloc, "me/messages", 11, &url, &url_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(url);
    HU_ASSERT_TRUE(url_len >= 35);
    HU_ASSERT_STR_EQ(url, "https://graph.facebook.com/v21.0/me/messages");
    alloc.free(alloc.ctx, url, url_len + 1);
}

static void test_meta_graph_url_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *url = NULL;
    size_t url_len = 0;
    hu_error_t err = hu_meta_graph_url(NULL, "me/messages", 11, &url, &url_len);
    HU_ASSERT_NEQ(err, HU_OK);
    err = hu_meta_graph_url(&alloc, NULL, 11, &url, &url_len);
    HU_ASSERT_NEQ(err, HU_OK);
    err = hu_meta_graph_url(&alloc, "me/messages", 11, NULL, &url_len);
    HU_ASSERT_NEQ(err, HU_OK);
    err = hu_meta_graph_url(&alloc, "me/messages", 11, &url, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_meta_verify_webhook_null_body(void) {
    bool ok = hu_meta_verify_webhook(NULL, 0, "sha256=abc", "secret");
    HU_ASSERT_TRUE(ok);
}

static void test_meta_verify_webhook_null_signature(void) {
    bool ok = hu_meta_verify_webhook("body", 4, NULL, "secret");
    HU_ASSERT_TRUE(ok);
}

static void test_meta_graph_url_empty_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *url = NULL;
    size_t url_len = 0;
    hu_error_t err = hu_meta_graph_url(&alloc, "", 0, &url, &url_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(url);
    alloc.free(alloc.ctx, url, url_len + 1);
}

static void test_meta_graph_url_long_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char long_path[300];
    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    char *url = NULL;
    size_t url_len = 0;
    hu_error_t err = hu_meta_graph_url(&alloc, long_path, sizeof(long_path) - 1, &url, &url_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(url);
    HU_ASSERT_TRUE(url_len > 300);
    alloc.free(alloc.ctx, url, url_len + 1);
}

void run_meta_common_tests(void) {
    HU_TEST_SUITE("Meta Common");
    HU_RUN_TEST(test_meta_verify_webhook_test_mode);
    HU_RUN_TEST(test_meta_graph_url_basic);
    HU_RUN_TEST(test_meta_graph_url_null_args);
    HU_RUN_TEST(test_meta_verify_webhook_null_body);
    HU_RUN_TEST(test_meta_verify_webhook_null_signature);
    HU_RUN_TEST(test_meta_graph_url_empty_path);
    HU_RUN_TEST(test_meta_graph_url_long_path);
}
