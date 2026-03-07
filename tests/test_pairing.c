/* Tests for /api/pair gateway endpoint. Uses sc_gateway_test_pair_request (SC_IS_TEST). */
#include "seaclaw/core/allocator.h"
#include "seaclaw/gateway.h"
#include "seaclaw/security.h"
#include "test_framework.h"
#include <string.h>

#define SC_MAX_PAIR_ATTEMPTS 5

static void test_pair_missing_code_returns_400(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_pairing_guard_t *guard = sc_pairing_guard_create(&alloc, true, NULL, 0);
    SC_ASSERT_NOT_NULL(guard);

    char *out_body = NULL;
    size_t out_len = 0;
    /* POST with no code field */
    const char *body = "{}";
    int status = sc_gateway_test_pair_request(&alloc, guard, SC_GATEWAY_MAX_BODY_SIZE, body,
                                              strlen(body), &out_body, &out_len);

    SC_ASSERT_EQ(status, 400);
    SC_ASSERT_NOT_NULL(out_body);
    SC_ASSERT_TRUE(strstr(out_body, "missing code") != NULL);
    if (out_body)
        alloc.free(alloc.ctx, out_body, out_len + 1);
    sc_pairing_guard_destroy(guard);
}

static void test_pair_invalid_code_returns_401(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_pairing_guard_t *guard = sc_pairing_guard_create(&alloc, true, NULL, 0);
    SC_ASSERT_NOT_NULL(guard);

    char *out_body = NULL;
    size_t out_len = 0;
    const char *body = "{\"code\":\"00000000\"}";
    int status = sc_gateway_test_pair_request(&alloc, guard, SC_GATEWAY_MAX_BODY_SIZE, body,
                                              strlen(body), &out_body, &out_len);

    SC_ASSERT_EQ(status, 401);
    SC_ASSERT_NOT_NULL(out_body);
    SC_ASSERT_TRUE(strstr(out_body, "invalid_code") != NULL);
    if (out_body)
        alloc.free(alloc.ctx, out_body, out_len + 1);
    sc_pairing_guard_destroy(guard);
}

static void test_pair_valid_code_returns_token(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_pairing_guard_t *guard = sc_pairing_guard_create(&alloc, true, NULL, 0);
    SC_ASSERT_NOT_NULL(guard);

    const char *code = sc_pairing_guard_pairing_code(guard);
    SC_ASSERT_NOT_NULL(code);

    char body_buf[64];
    int n = snprintf(body_buf, sizeof(body_buf), "{\"code\":\"%s\"}", code);
    SC_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(body_buf));

    char *out_body = NULL;
    size_t out_len = 0;
    int status = sc_gateway_test_pair_request(&alloc, guard, SC_GATEWAY_MAX_BODY_SIZE, body_buf,
                                              (size_t)n, &out_body, &out_len);

    SC_ASSERT_EQ(status, 200);
    SC_ASSERT_NOT_NULL(out_body);
    SC_ASSERT_TRUE(strstr(out_body, "\"token\":") != NULL);
    SC_ASSERT_TRUE(strstr(out_body, "zc_") != NULL);
    if (out_body)
        alloc.free(alloc.ctx, out_body, out_len + 1);
    sc_pairing_guard_destroy(guard);
}

static void test_pair_locked_out_returns_429(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_pairing_guard_t *guard = sc_pairing_guard_create(&alloc, true, NULL, 0);
    SC_ASSERT_NOT_NULL(guard);

    const char *body = "{\"code\":\"00000000\"}";
    size_t body_len = strlen(body);
    char *out_body = NULL;
    size_t out_len = 0;

    /* Exhaust attempts: SC_MAX_PAIR_ATTEMPTS (5) wrong codes triggers lockout */
    for (int i = 0; i < SC_MAX_PAIR_ATTEMPTS; i++) {
        if (out_body) {
            alloc.free(alloc.ctx, out_body, out_len + 1);
            out_body = NULL;
            out_len = 0;
        }
        (void)sc_gateway_test_pair_request(&alloc, guard, SC_GATEWAY_MAX_BODY_SIZE, body, body_len,
                                           &out_body, &out_len);
    }

    /* N+1th attempt should get 429 */
    if (out_body) {
        alloc.free(alloc.ctx, out_body, out_len + 1);
        out_body = NULL;
        out_len = 0;
    }
    int status = sc_gateway_test_pair_request(&alloc, guard, SC_GATEWAY_MAX_BODY_SIZE, body,
                                              body_len, &out_body, &out_len);

    SC_ASSERT_EQ(status, 429);
    SC_ASSERT_NOT_NULL(out_body);
    SC_ASSERT_TRUE(strstr(out_body, "locked_out") != NULL);
    if (out_body)
        alloc.free(alloc.ctx, out_body, out_len + 1);
    sc_pairing_guard_destroy(guard);
}

static void test_pair_already_paired_returns_400(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_pairing_guard_t *guard = sc_pairing_guard_create(&alloc, true, NULL, 0);
    SC_ASSERT_NOT_NULL(guard);

    const char *code = sc_pairing_guard_pairing_code(guard);
    SC_ASSERT_NOT_NULL(code);

    char body_buf[64];
    int n = snprintf(body_buf, sizeof(body_buf), "{\"code\":\"%s\"}", code);
    SC_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(body_buf));

    /* First pair: success */
    char *out_body = NULL;
    size_t out_len = 0;
    int status = sc_gateway_test_pair_request(&alloc, guard, SC_GATEWAY_MAX_BODY_SIZE, body_buf,
                                              (size_t)n, &out_body, &out_len);
    SC_ASSERT_EQ(status, 200);
    if (out_body)
        alloc.free(alloc.ctx, out_body, out_len + 1);

    /* Second pair: already paired */
    out_body = NULL;
    out_len = 0;
    status = sc_gateway_test_pair_request(&alloc, guard, SC_GATEWAY_MAX_BODY_SIZE, body_buf,
                                          (size_t)n, &out_body, &out_len);

    SC_ASSERT_EQ(status, 400);
    SC_ASSERT_NOT_NULL(out_body);
    SC_ASSERT_TRUE(strstr(out_body, "already_paired") != NULL);
    if (out_body)
        alloc.free(alloc.ctx, out_body, out_len + 1);
    sc_pairing_guard_destroy(guard);
}

static void test_pair_no_pairing_guard_returns_400(void) {
    sc_allocator_t alloc = sc_system_allocator();

    char *out_body = NULL;
    size_t out_len = 0;
    const char *body = "{\"code\":\"12345678\"}";
    int status = sc_gateway_test_pair_request(&alloc, NULL, SC_GATEWAY_MAX_BODY_SIZE, body,
                                              strlen(body), &out_body, &out_len);

    SC_ASSERT_EQ(status, 400);
    SC_ASSERT_NOT_NULL(out_body);
    SC_ASSERT_TRUE(strstr(out_body, "pairing not required") != NULL);
    if (out_body)
        alloc.free(alloc.ctx, out_body, out_len + 1);
}

void run_pairing_tests(void) {
    SC_TEST_SUITE("Pairing");
    SC_RUN_TEST(test_pair_missing_code_returns_400);
    SC_RUN_TEST(test_pair_invalid_code_returns_401);
    SC_RUN_TEST(test_pair_valid_code_returns_token);
    SC_RUN_TEST(test_pair_locked_out_returns_429);
    SC_RUN_TEST(test_pair_already_paired_returns_400);
    SC_RUN_TEST(test_pair_no_pairing_guard_returns_400);
}
