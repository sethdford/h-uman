/* HTTP GET tests */
#include "test_framework.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <string.h>

static void test_http_get_mock(void) {
#if SC_IS_TEST
    sc_allocator_t alloc = sc_system_allocator();
    sc_http_response_t resp = {0};
    sc_error_t err = sc_http_get(&alloc, "https://example.com/", NULL, &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.body);
    SC_ASSERT_TRUE(resp.body_len > 0);
    SC_ASSERT_EQ(resp.status_code, 200);
    SC_ASSERT_TRUE(resp.owned);
    SC_ASSERT_TRUE(strstr(resp.body, "sc_http_get") != NULL);
    sc_http_response_free(&alloc, &resp);
    SC_ASSERT_NULL(resp.body);
#endif
}

static void test_http_get_null_args(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_http_response_t resp = {0};
    sc_error_t err = sc_http_get(NULL, "https://x.com", NULL, &resp);
    SC_ASSERT_NEQ(err, SC_OK);
    err = sc_http_get(&alloc, NULL, NULL, &resp);
    SC_ASSERT_NEQ(err, SC_OK);
    err = sc_http_get(&alloc, "https://x.com", NULL, NULL);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_http_response_free_null_body(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_http_response_t resp = {0};
    resp.body = NULL;
    resp.owned = false;
    sc_http_response_free(&alloc, &resp);
    SC_ASSERT_NULL(resp.body);
}

static void test_http_response_status_code(void) {
#if SC_IS_TEST
    sc_allocator_t alloc = sc_system_allocator();
    sc_http_response_t resp = {0};
    sc_http_get(&alloc, "https://example.com/", NULL, &resp);
    SC_ASSERT_EQ(resp.status_code, 200);
    sc_http_response_free(&alloc, &resp);
#endif
}

static void test_http_response_body_extraction(void) {
#if SC_IS_TEST
    sc_allocator_t alloc = sc_system_allocator();
    sc_http_response_t resp = {0};
    sc_http_get(&alloc, "https://x.com/", NULL, &resp);
    SC_ASSERT_NOT_NULL(resp.body);
    SC_ASSERT_TRUE(resp.body_len > 0);
    SC_ASSERT_TRUE(resp.body[resp.body_len] == '\0' || resp.body_len < 4096);
    sc_http_response_free(&alloc, &resp);
#endif
}

#if 0
static void test_http_get_ex_mock(void) {
#if SC_IS_TEST
    sc_allocator_t alloc = sc_system_allocator();
    sc_http_response_t resp = {0};
    sc_error_t err = sc_http_get_ex(&alloc, "https://example.com/", "Accept: application/json\n", &resp);
    if (err == SC_OK) {
        SC_ASSERT_NOT_NULL(resp.body);
        sc_http_response_free(&alloc, &resp);
    }
#endif
}
#endif /* #if 0 */

#if 0
static void test_http_request_get_mock(void) {
#if SC_IS_TEST
    sc_allocator_t alloc = sc_system_allocator();
    sc_http_response_t resp = {0};
    sc_error_t err = sc_http_request(&alloc, "https://api.test/", "GET", NULL, NULL, 0, &resp);
    if (err == SC_OK) {
        SC_ASSERT_NOT_NULL(resp.body);
        sc_http_response_free(&alloc, &resp);
    }
#endif
}
#endif

#if SC_IS_TEST
static void test_http_post_json_mock(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_http_response_t resp = {0};
    const char *json = "{\"key\":\"value\"}";
    sc_error_t err = sc_http_post_json(&alloc, "https://api.example.com/",
        NULL, json, strlen(json), &resp);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(resp.body);
    sc_http_response_free(&alloc, &resp);
}
#endif

void run_http_tests(void) {
    SC_TEST_SUITE("HTTP GET");
    SC_RUN_TEST(test_http_get_mock);
    SC_RUN_TEST(test_http_get_null_args);
    SC_RUN_TEST(test_http_response_free_null_body);
    SC_RUN_TEST(test_http_response_status_code);
    SC_RUN_TEST(test_http_response_body_extraction);
#if SC_IS_TEST
    SC_RUN_TEST(test_http_post_json_mock);
#endif
}
