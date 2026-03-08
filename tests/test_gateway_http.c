/* Gateway HTTP handling tests - config, health, rate limit, path security, CORS, malformed HTTP. */
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/crypto.h"
#include "seaclaw/gateway.h"
#include "seaclaw/health.h"
#include "test_framework.h"
#include <string.h>

static void test_gateway_config_defaults(void) {
    sc_gateway_config_t cfg = {0};
    SC_ASSERT_NULL(cfg.host);
    SC_ASSERT_EQ(cfg.port, 0);
    SC_ASSERT_EQ(cfg.max_body_size, 0);
    cfg.host = "0.0.0.0";
    cfg.port = 8080;
    cfg.max_body_size = 65536;
    SC_ASSERT_STR_EQ(cfg.host, "0.0.0.0");
    SC_ASSERT_EQ(cfg.port, 8080);
    SC_ASSERT_EQ(cfg.max_body_size, 65536);
}

static void test_gateway_health_marking(void) {
    sc_health_reset();
    sc_health_mark_ok("gateway");
    sc_allocator_t alloc = sc_system_allocator();
    sc_readiness_result_t r = sc_health_check_readiness(&alloc);
    SC_ASSERT_EQ(r.status, SC_READINESS_READY);
    if (r.checks)
        alloc.free(alloc.ctx, (void *)r.checks, r.check_count * sizeof(sc_component_check_t));
}

static void test_gateway_readiness_when_error(void) {
    sc_health_reset();
    sc_health_mark_ok("a");
    sc_health_mark_error("gateway", "bind failed");
    sc_allocator_t alloc = sc_system_allocator();
    sc_readiness_result_t r = sc_health_check_readiness(&alloc);
    SC_ASSERT_EQ(r.status, SC_READINESS_NOT_READY);
    if (r.checks)
        alloc.free(alloc.ctx, (void *)r.checks, r.check_count * sizeof(sc_component_check_t));
}

static void test_gateway_rate_limit_config(void) {
    sc_gateway_config_t cfg = {0};
    cfg.rate_limit_per_minute = SC_GATEWAY_RATE_LIMIT_PER_MIN;
    SC_ASSERT_EQ(cfg.rate_limit_per_minute, 60);
}

static void test_gateway_hmac_verify_helper(void) {
    uint8_t key[] = "secret";
    uint8_t msg[] = "hello";
    uint8_t out[32];
    sc_hmac_sha256(key, 6, msg, 5, out);
    SC_ASSERT(out[0] != 0 || out[1] != 0);
}

static void test_gateway_run_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_gateway_config_t config = {.port = 9999, .test_mode = true};
    sc_error_t err = sc_gateway_run(&alloc, "127.0.0.1", 9999, &config);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_gateway_max_body_size_constant(void) {
    SC_ASSERT_EQ(SC_GATEWAY_MAX_BODY_SIZE, 65536);
}

/* ── Path traversal (serve_static_file) ────────────────────────────────────── */

static void test_path_traversal_rejects_dotdot(void) {
    SC_ASSERT_TRUE(sc_gateway_path_has_traversal("/static/../etc/passwd"));
    SC_ASSERT_TRUE(sc_gateway_path_has_traversal("../index.html"));
    SC_ASSERT_TRUE(sc_gateway_path_has_traversal("/.."));
}

static void test_path_traversal_rejects_percent_encoded(void) {
    SC_ASSERT_TRUE(sc_gateway_path_has_traversal("/%2e%2e/etc/passwd"));
    SC_ASSERT_TRUE(sc_gateway_path_has_traversal("/%2E%2E/secret"));
    SC_ASSERT_TRUE(sc_gateway_path_has_traversal("/%2e%2E/foo"));
    SC_ASSERT_TRUE(sc_gateway_path_has_traversal("/%2E%2e/bar"));
}

static void test_path_traversal_rejects_null_byte(void) {
    SC_ASSERT_TRUE(sc_gateway_path_has_traversal("/path%00/../../../etc/passwd"));
}

static void test_path_traversal_rejects_double_encoded(void) {
    SC_ASSERT_TRUE(sc_gateway_path_has_traversal("/%252e%252e/etc/passwd"));
    SC_ASSERT_TRUE(sc_gateway_path_has_traversal("/%252E%252E/secret"));
}

static void test_path_traversal_allows_safe_paths(void) {
    SC_ASSERT_FALSE(sc_gateway_path_has_traversal("/index.html"));
    SC_ASSERT_FALSE(sc_gateway_path_has_traversal("/static/app.js"));
    SC_ASSERT_FALSE(sc_gateway_path_has_traversal("/"));
    SC_ASSERT_FALSE(sc_gateway_path_has_traversal(NULL));
}

/* ── Webhook path matching ────────────────────────────────────────────────── */

static void test_webhook_path_valid_webhook_prefix(void) {
    SC_ASSERT_TRUE(sc_gateway_is_webhook_path("/webhook/telegram"));
    SC_ASSERT_TRUE(sc_gateway_is_webhook_path("/webhook/facebook"));
    SC_ASSERT_TRUE(sc_gateway_is_webhook_path("/webhook"));
    SC_ASSERT_TRUE(sc_gateway_is_webhook_path("/webhook/"));
}

static void test_webhook_path_valid_direct_channels(void) {
    SC_ASSERT_TRUE(sc_gateway_is_webhook_path("/telegram"));
    SC_ASSERT_TRUE(sc_gateway_is_webhook_path("/discord"));
    SC_ASSERT_TRUE(sc_gateway_is_webhook_path("/slack/events"));
    SC_ASSERT_TRUE(sc_gateway_is_webhook_path("/tiktok"));
    SC_ASSERT_TRUE(sc_gateway_is_webhook_path("/whatsapp"));
    SC_ASSERT_TRUE(sc_gateway_is_webhook_path("/facebook"));
}

static void test_webhook_path_rejects_traversal(void) {
    SC_ASSERT_FALSE(sc_gateway_is_webhook_path("/webhook/../../../etc/passwd"));
    SC_ASSERT_FALSE(sc_gateway_is_webhook_path("/webhook/%2e%2e/foo"));
}

static void test_webhook_path_rejects_invalid_prefix(void) {
    SC_ASSERT_FALSE(sc_gateway_is_webhook_path("/webhookx"));
    SC_ASSERT_FALSE(sc_gateway_is_webhook_path("/webhooks"));
    SC_ASSERT_FALSE(sc_gateway_is_webhook_path("/health"));
    SC_ASSERT_FALSE(sc_gateway_is_webhook_path(NULL));
}

/* ── CORS origin validation ────────────────────────────────────────────────── */

static void test_cors_allows_localhost(void) {
    SC_ASSERT_TRUE(sc_gateway_is_allowed_origin("http://localhost:3000", NULL, 0));
    SC_ASSERT_TRUE(sc_gateway_is_allowed_origin("https://localhost", NULL, 0));
    SC_ASSERT_TRUE(sc_gateway_is_allowed_origin("http://127.0.0.1:8080", NULL, 0));
    SC_ASSERT_TRUE(sc_gateway_is_allowed_origin("http://[::1]:3000", NULL, 0));
}

static void test_cors_allows_explicit_origins(void) {
    const char *allowed[] = {"https://app.example.com", "https://dashboard.example.com"};
    SC_ASSERT_TRUE(sc_gateway_is_allowed_origin("https://app.example.com", allowed, 2));
    SC_ASSERT_TRUE(sc_gateway_is_allowed_origin("https://dashboard.example.com", allowed, 2));
}

static void test_cors_rejects_unknown_origins(void) {
    const char *allowed[] = {"https://app.example.com"};
    SC_ASSERT_FALSE(sc_gateway_is_allowed_origin("https://evil.com", allowed, 1));
    SC_ASSERT_FALSE(sc_gateway_is_allowed_origin("https://app.example.com.evil.com", allowed, 1));
}

static void test_cors_allows_empty_origin(void) {
    SC_ASSERT_TRUE(sc_gateway_is_allowed_origin("", NULL, 0));
    SC_ASSERT_TRUE(sc_gateway_is_allowed_origin(NULL, NULL, 0));
}

/* ── Malformed HTTP (Content-Length parsing) ───────────────────────────────── */

static void test_content_length_valid(void) {
    size_t len = 0;
    sc_error_t err = sc_gateway_parse_content_length("42", SC_GATEWAY_MAX_BODY_SIZE, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(len, 42u);
}

static void test_content_length_with_spaces(void) {
    size_t len = 0;
    sc_error_t err = sc_gateway_parse_content_length("  100  ", SC_GATEWAY_MAX_BODY_SIZE, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(len, 100u);
}

static void test_content_length_non_numeric_rejected(void) {
    size_t len = 0;
    sc_error_t err = sc_gateway_parse_content_length("abc", SC_GATEWAY_MAX_BODY_SIZE, &len);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_content_length_negative_rejected(void) {
    size_t len = 0;
    sc_error_t err = sc_gateway_parse_content_length("-1", SC_GATEWAY_MAX_BODY_SIZE, &len);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_content_length_exceeds_max_rejected(void) {
    size_t len = 0;
    sc_error_t err = sc_gateway_parse_content_length("70000", 65536, &len);
    SC_ASSERT_EQ(err, SC_ERR_GATEWAY_BODY_TOO_LARGE);
}

static void test_content_length_empty_rejected(void) {
    size_t len = 0;
    sc_error_t err = sc_gateway_parse_content_length("", 65536, &len);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_content_length_null_args_rejected(void) {
    size_t len = 0;
    sc_error_t err = sc_gateway_parse_content_length(NULL, 65536, &len);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

void run_gateway_http_tests(void) {
    SC_TEST_SUITE("Gateway HTTP");
    SC_RUN_TEST(test_gateway_config_defaults);
    SC_RUN_TEST(test_gateway_health_marking);
    SC_RUN_TEST(test_gateway_readiness_when_error);
    SC_RUN_TEST(test_gateway_rate_limit_config);
    SC_RUN_TEST(test_gateway_hmac_verify_helper);
    SC_RUN_TEST(test_gateway_run_test_mode);
    SC_RUN_TEST(test_gateway_max_body_size_constant);

    SC_TEST_SUITE("Path Traversal");
    SC_RUN_TEST(test_path_traversal_rejects_dotdot);
    SC_RUN_TEST(test_path_traversal_rejects_percent_encoded);
    SC_RUN_TEST(test_path_traversal_rejects_null_byte);
    SC_RUN_TEST(test_path_traversal_rejects_double_encoded);
    SC_RUN_TEST(test_path_traversal_allows_safe_paths);

    SC_TEST_SUITE("Webhook Path");
    SC_RUN_TEST(test_webhook_path_valid_webhook_prefix);
    SC_RUN_TEST(test_webhook_path_valid_direct_channels);
    SC_RUN_TEST(test_webhook_path_rejects_traversal);
    SC_RUN_TEST(test_webhook_path_rejects_invalid_prefix);

    SC_TEST_SUITE("CORS Origin");
    SC_RUN_TEST(test_cors_allows_localhost);
    SC_RUN_TEST(test_cors_allows_explicit_origins);
    SC_RUN_TEST(test_cors_rejects_unknown_origins);
    SC_RUN_TEST(test_cors_allows_empty_origin);

    SC_TEST_SUITE("Content-Length Parsing");
    SC_RUN_TEST(test_content_length_valid);
    SC_RUN_TEST(test_content_length_with_spaces);
    SC_RUN_TEST(test_content_length_non_numeric_rejected);
    SC_RUN_TEST(test_content_length_negative_rejected);
    SC_RUN_TEST(test_content_length_exceeds_max_rejected);
    SC_RUN_TEST(test_content_length_empty_rejected);
    SC_RUN_TEST(test_content_length_null_args_rejected);
}
