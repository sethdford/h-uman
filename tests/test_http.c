/* HTTP GET tests */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "test_framework.h"
#include <string.h>

static void test_http_get_mock(void) {
#if HU_IS_TEST
    hu_allocator_t alloc = hu_system_allocator();
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(&alloc, "https://example.com/", NULL, &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.body);
    HU_ASSERT_TRUE(resp.body_len > 0);
    HU_ASSERT_EQ(resp.status_code, 200);
    HU_ASSERT_TRUE(resp.owned);
    HU_ASSERT_TRUE(strstr(resp.body, "hu_http_get") != NULL);
    hu_http_response_free(&alloc, &resp);
    HU_ASSERT_NULL(resp.body);
#endif
}

static void test_http_get_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(NULL, "https://x.com", NULL, &resp);
    HU_ASSERT_NEQ(err, HU_OK);
    err = hu_http_get(&alloc, NULL, NULL, &resp);
    HU_ASSERT_NEQ(err, HU_OK);
    err = hu_http_get(&alloc, "https://x.com", NULL, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_http_response_free_null_body(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_http_response_t resp = {0};
    resp.body = NULL;
    resp.owned = false;
    hu_http_response_free(&alloc, &resp);
    HU_ASSERT_NULL(resp.body);
}

static void test_http_response_status_code(void) {
#if HU_IS_TEST
    hu_allocator_t alloc = hu_system_allocator();
    hu_http_response_t resp = {0};
    hu_http_get(&alloc, "https://example.com/", NULL, &resp);
    HU_ASSERT_EQ(resp.status_code, 200);
    hu_http_response_free(&alloc, &resp);
#endif
}

static void test_http_response_body_extraction(void) {
#if HU_IS_TEST
    hu_allocator_t alloc = hu_system_allocator();
    hu_http_response_t resp = {0};
    hu_http_get(&alloc, "https://x.com/", NULL, &resp);
    HU_ASSERT_NOT_NULL(resp.body);
    HU_ASSERT_TRUE(resp.body_len > 0);
    HU_ASSERT_TRUE(resp.body[resp.body_len] == '\0' || resp.body_len < 4096);
    hu_http_response_free(&alloc, &resp);
#endif
}

static void test_http_get_ex_mock(void) {
#if HU_IS_TEST
    hu_allocator_t alloc = hu_system_allocator();
    hu_http_response_t resp = {0};
    hu_error_t err =
        hu_http_get_ex(&alloc, "https://example.com/", "Accept: application/json\n", &resp);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_NOT_SUPPORTED);
    if (err == HU_OK) {
        HU_ASSERT_NOT_NULL(resp.body);
        hu_http_response_free(&alloc, &resp);
    }
#endif
}

static void test_http_request_get_mock(void) {
#if HU_IS_TEST
    hu_allocator_t alloc = hu_system_allocator();
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_request(&alloc, "https://api.test/", "GET", NULL, NULL, 0, &resp);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_NOT_SUPPORTED);
    if (err == HU_OK) {
        HU_ASSERT_NOT_NULL(resp.body);
        hu_http_response_free(&alloc, &resp);
    }
#endif
}

static size_t test_http_stream_cb_noop(const char *chunk, size_t chunk_len, void *userdata) {
    (void)chunk;
    (void)userdata;
    return chunk_len;
}

static void test_http_post_json_stream_null_alloc(void) {
    const char *body = "{}";
    hu_error_t err = hu_http_post_json_stream(NULL, "https://example.com/", NULL, NULL, body,
                                              strlen(body), test_http_stream_cb_noop, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_http_post_json_stream_null_url(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *body = "{}";
    hu_error_t err = hu_http_post_json_stream(&alloc, NULL, NULL, NULL, body, strlen(body),
                                              test_http_stream_cb_noop, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_http_post_json_stream_null_callback(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *body = "{}";
    hu_error_t err = hu_http_post_json_stream(&alloc, "https://example.com/", NULL, NULL, body,
                                              strlen(body), NULL, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

#if HU_IS_TEST
static void test_http_post_json_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_http_response_t resp = {0};
    const char *json = "{\"key\":\"value\"}";
    hu_error_t err =
        hu_http_post_json(&alloc, "https://api.example.com/", NULL, json, strlen(json), &resp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp.body);
    hu_http_response_free(&alloc, &resp);
}
#endif

void run_http_tests(void) {
    HU_TEST_SUITE("HTTP GET");
    HU_RUN_TEST(test_http_get_mock);
    HU_RUN_TEST(test_http_get_null_args);
    HU_RUN_TEST(test_http_post_json_stream_null_alloc);
    HU_RUN_TEST(test_http_post_json_stream_null_url);
    HU_RUN_TEST(test_http_post_json_stream_null_callback);
    HU_RUN_TEST(test_http_response_free_null_body);
    HU_RUN_TEST(test_http_response_status_code);
    HU_RUN_TEST(test_http_response_body_extraction);
    HU_RUN_TEST(test_http_get_ex_mock);
    HU_RUN_TEST(test_http_request_get_mock);
#if HU_IS_TEST
    HU_RUN_TEST(test_http_post_json_mock);
#endif
}
