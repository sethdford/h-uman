#include "seaclaw/core/allocator.h"
#include "seaclaw/gateway.h"
#include "seaclaw/health.h"
#include "test_framework.h"
#include <string.h>

/* Gateway tests run with SC_GATEWAY_TEST_MODE - no actual port binding */

static void test_gateway_run_does_not_bind_in_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_gateway_config_t config = {.port = 9999, .test_mode = true};
    sc_error_t err = sc_gateway_run(&alloc, "127.0.0.1", 9999, &config);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_health_mark_ok_then_readiness(void) {
    sc_health_reset();
    sc_health_mark_ok("gateway");
    sc_allocator_t alloc = sc_system_allocator();
    sc_readiness_result_t r = sc_health_check_readiness(&alloc);
    SC_ASSERT_EQ(r.status, SC_READINESS_READY);
    if (r.checks)
        alloc.free(alloc.ctx, (void *)r.checks, r.check_count * sizeof(sc_component_check_t));
}

static void test_health_mark_error_then_not_ready(void) {
    sc_health_reset();
    sc_health_mark_ok("a");
    sc_health_mark_error("b", "connection refused");
    sc_allocator_t alloc = sc_system_allocator();
    sc_readiness_result_t r = sc_health_check_readiness(&alloc);
    SC_ASSERT_EQ(r.status, SC_READINESS_NOT_READY);
    SC_ASSERT_EQ(r.check_count, 2);
    if (r.checks)
        alloc.free(alloc.ctx, (void *)r.checks, r.check_count * sizeof(sc_component_check_t));
}

static void test_health_empty_registry_ready(void) {
    sc_health_reset();
    sc_allocator_t alloc = sc_system_allocator();
    sc_readiness_result_t r = sc_health_check_readiness(&alloc);
    SC_ASSERT_EQ(r.status, SC_READINESS_READY);
    SC_ASSERT_EQ(r.check_count, 0);
}

static void test_rate_limiter_config(void) {
    sc_gateway_config_t cfg = {0};
    SC_ASSERT_EQ(cfg.rate_limit_per_minute, 0);
    cfg.rate_limit_per_minute = 60;
    SC_ASSERT_EQ(cfg.rate_limit_per_minute, 60);
}

static void test_gateway_path_is_exact_match(void) {
    SC_ASSERT_TRUE(sc_gateway_path_is("/api/health", "/api/health"));
    SC_ASSERT_FALSE(sc_gateway_path_is("/api/healthz", "/api/health"));
    SC_ASSERT_FALSE(sc_gateway_path_is("/api/heal", "/api/health"));
}

static void test_gateway_path_traversal_detection(void) {
    SC_ASSERT_TRUE(sc_gateway_path_has_traversal("/../etc/passwd"));
    SC_ASSERT_TRUE(sc_gateway_path_has_traversal("/foo/%2e%2e/bar"));
    SC_ASSERT_TRUE(sc_gateway_path_has_traversal("/foo%00bar"));
    SC_ASSERT_FALSE(sc_gateway_path_has_traversal("/api/health"));
    SC_ASSERT_FALSE(sc_gateway_path_has_traversal("/foo/bar/baz"));
}

static void test_gateway_is_webhook_path(void) {
    SC_ASSERT_TRUE(sc_gateway_is_webhook_path("/webhook/telegram"));
    SC_ASSERT_TRUE(sc_gateway_is_webhook_path("/webhook/discord"));
    SC_ASSERT_FALSE(sc_gateway_is_webhook_path("/api/health"));
    SC_ASSERT_FALSE(sc_gateway_is_webhook_path("/webhookfoo"));
}

static void test_gateway_cors_localhost_allowed(void) {
    SC_ASSERT_TRUE(sc_gateway_is_allowed_origin("http://localhost:5173", NULL, 0));
    SC_ASSERT_TRUE(sc_gateway_is_allowed_origin("http://127.0.0.1:3000", NULL, 0));
    SC_ASSERT_FALSE(sc_gateway_is_allowed_origin("https://evil.com", NULL, 0));
}

static void test_gateway_cors_custom_origins(void) {
    const char *allowed[] = {"https://app.example.com"};
    SC_ASSERT_TRUE(sc_gateway_is_allowed_origin("https://app.example.com", allowed, 1));
    SC_ASSERT_FALSE(sc_gateway_is_allowed_origin("https://other.example.com", allowed, 1));
}

static void test_gateway_parse_content_length_valid(void) {
    size_t len = 0;
    sc_error_t err = sc_gateway_parse_content_length("42", 65536, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(len, 42);
}

static void test_gateway_parse_content_length_too_large(void) {
    size_t len = 0;
    sc_error_t err = sc_gateway_parse_content_length("99999", 1024, &len);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_health_multiple_components(void) {
    sc_health_reset();
    sc_health_mark_ok("gateway");
    sc_health_mark_ok("memory");
    sc_health_mark_ok("agent");
    sc_allocator_t alloc = sc_system_allocator();
    sc_readiness_result_t r = sc_health_check_readiness(&alloc);
    SC_ASSERT_EQ(r.status, SC_READINESS_READY);
    SC_ASSERT_EQ(r.check_count, 3);
    if (r.checks)
        alloc.free(alloc.ctx, (void *)r.checks, r.check_count * sizeof(sc_component_check_t));
}

static void test_gateway_config_defaults(void) {
    sc_gateway_config_t cfg = {0};
    SC_ASSERT_EQ(cfg.port, 0);
    SC_ASSERT_NULL(cfg.host);
    SC_ASSERT_NULL(cfg.hmac_secret);
    SC_ASSERT_FALSE(cfg.test_mode);
    SC_ASSERT_FALSE(cfg.require_pairing);
    SC_ASSERT_NULL(cfg.cors_origins);
    SC_ASSERT_EQ(cfg.cors_origins_len, 0);
}

void run_gateway_tests(void) {
    SC_TEST_SUITE("gateway");
    SC_RUN_TEST(test_gateway_run_does_not_bind_in_test_mode);
    SC_RUN_TEST(test_health_mark_ok_then_readiness);
    SC_RUN_TEST(test_health_mark_error_then_not_ready);
    SC_RUN_TEST(test_health_empty_registry_ready);
    SC_RUN_TEST(test_rate_limiter_config);
    SC_RUN_TEST(test_gateway_path_is_exact_match);
    SC_RUN_TEST(test_gateway_path_traversal_detection);
    SC_RUN_TEST(test_gateway_is_webhook_path);
    SC_RUN_TEST(test_gateway_cors_localhost_allowed);
    SC_RUN_TEST(test_gateway_cors_custom_origins);
    SC_RUN_TEST(test_gateway_parse_content_length_valid);
    SC_RUN_TEST(test_gateway_parse_content_length_too_large);
    SC_RUN_TEST(test_health_multiple_components);
    SC_RUN_TEST(test_gateway_config_defaults);
}
