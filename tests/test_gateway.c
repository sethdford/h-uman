#include "human/core/allocator.h"
#include "human/gateway.h"
#include "human/health.h"
#include "test_framework.h"
#include <string.h>

/* Gateway tests run with HU_GATEWAY_TEST_MODE - no actual port binding */

static void test_gateway_run_does_not_bind_in_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_gateway_config_t config = {.port = 9999, .test_mode = true};
    hu_error_t err = hu_gateway_run(&alloc, "127.0.0.1", 9999, &config);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_health_mark_ok_then_readiness(void) {
    hu_health_reset();
    hu_health_mark_ok("gateway");
    hu_allocator_t alloc = hu_system_allocator();
    hu_readiness_result_t r = hu_health_check_readiness(&alloc);
    HU_ASSERT_EQ(r.status, HU_READINESS_READY);
    if (r.checks)
        alloc.free(alloc.ctx, (void *)r.checks, r.check_count * sizeof(hu_component_check_t));
}

static void test_health_mark_error_then_not_ready(void) {
    hu_health_reset();
    hu_health_mark_ok("a");
    hu_health_mark_error("b", "connection refused");
    hu_allocator_t alloc = hu_system_allocator();
    hu_readiness_result_t r = hu_health_check_readiness(&alloc);
    HU_ASSERT_EQ(r.status, HU_READINESS_NOT_READY);
    HU_ASSERT_EQ(r.check_count, 2);
    if (r.checks)
        alloc.free(alloc.ctx, (void *)r.checks, r.check_count * sizeof(hu_component_check_t));
}

static void test_health_empty_registry_ready(void) {
    hu_health_reset();
    hu_allocator_t alloc = hu_system_allocator();
    hu_readiness_result_t r = hu_health_check_readiness(&alloc);
    HU_ASSERT_EQ(r.status, HU_READINESS_READY);
    HU_ASSERT_EQ(r.check_count, 0);
}

static void test_rate_limiter_config(void) {
    hu_gateway_config_t cfg = {0};
    HU_ASSERT_EQ(cfg.rate_limit_per_minute, 0);
    cfg.rate_limit_per_minute = 60;
    HU_ASSERT_EQ(cfg.rate_limit_per_minute, 60);
}

static void test_gateway_path_is_exact_match(void) {
    HU_ASSERT_TRUE(hu_gateway_path_is("/api/health", "/api/health"));
    HU_ASSERT_FALSE(hu_gateway_path_is("/api/healthz", "/api/health"));
    HU_ASSERT_FALSE(hu_gateway_path_is("/api/heal", "/api/health"));
}

static void test_gateway_path_traversal_detection(void) {
    HU_ASSERT_TRUE(hu_gateway_path_has_traversal("/../etc/passwd"));
    HU_ASSERT_TRUE(hu_gateway_path_has_traversal("/foo/%2e%2e/bar"));
    HU_ASSERT_TRUE(hu_gateway_path_has_traversal("/foo%00bar"));
    HU_ASSERT_FALSE(hu_gateway_path_has_traversal("/api/health"));
    HU_ASSERT_FALSE(hu_gateway_path_has_traversal("/foo/bar/baz"));
}

static void test_gateway_is_webhook_path(void) {
    HU_ASSERT_TRUE(hu_gateway_is_webhook_path("/webhook/telegram"));
    HU_ASSERT_TRUE(hu_gateway_is_webhook_path("/webhook/discord"));
    HU_ASSERT_FALSE(hu_gateway_is_webhook_path("/api/health"));
    HU_ASSERT_FALSE(hu_gateway_is_webhook_path("/webhookfoo"));
}

static void test_gateway_cors_localhost_allowed(void) {
    HU_ASSERT_TRUE(hu_gateway_is_allowed_origin("http://localhost:5173", NULL, 0));
    HU_ASSERT_TRUE(hu_gateway_is_allowed_origin("http://127.0.0.1:3000", NULL, 0));
    HU_ASSERT_FALSE(hu_gateway_is_allowed_origin("https://evil.com", NULL, 0));
}

static void test_gateway_cors_custom_origins(void) {
    const char *allowed[] = {"https://app.example.com"};
    HU_ASSERT_TRUE(hu_gateway_is_allowed_origin("https://app.example.com", allowed, 1));
    HU_ASSERT_FALSE(hu_gateway_is_allowed_origin("https://other.example.com", allowed, 1));
}

static void test_gateway_parse_content_length_valid(void) {
    size_t len = 0;
    hu_error_t err = hu_gateway_parse_content_length("42", 65536, &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(len, 42);
}

static void test_gateway_parse_content_length_too_large(void) {
    size_t len = 0;
    hu_error_t err = hu_gateway_parse_content_length("99999", 1024, &len);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_health_multiple_components(void) {
    hu_health_reset();
    hu_health_mark_ok("gateway");
    hu_health_mark_ok("memory");
    hu_health_mark_ok("agent");
    hu_allocator_t alloc = hu_system_allocator();
    hu_readiness_result_t r = hu_health_check_readiness(&alloc);
    HU_ASSERT_EQ(r.status, HU_READINESS_READY);
    HU_ASSERT_EQ(r.check_count, 3);
    if (r.checks)
        alloc.free(alloc.ctx, (void *)r.checks, r.check_count * sizeof(hu_component_check_t));
}

static void test_gateway_config_defaults(void) {
    hu_gateway_config_t cfg = {0};
    HU_ASSERT_EQ(cfg.port, 0);
    HU_ASSERT_NULL(cfg.host);
    HU_ASSERT_NULL(cfg.hmac_secret);
    HU_ASSERT_FALSE(cfg.test_mode);
    HU_ASSERT_FALSE(cfg.require_pairing);
    HU_ASSERT_NULL(cfg.cors_origins);
    HU_ASSERT_EQ(cfg.cors_origins_len, 0);
}

void run_gateway_tests(void) {
    HU_TEST_SUITE("gateway");
    HU_RUN_TEST(test_gateway_run_does_not_bind_in_test_mode);
    HU_RUN_TEST(test_health_mark_ok_then_readiness);
    HU_RUN_TEST(test_health_mark_error_then_not_ready);
    HU_RUN_TEST(test_health_empty_registry_ready);
    HU_RUN_TEST(test_rate_limiter_config);
    HU_RUN_TEST(test_gateway_path_is_exact_match);
    HU_RUN_TEST(test_gateway_path_traversal_detection);
    HU_RUN_TEST(test_gateway_is_webhook_path);
    HU_RUN_TEST(test_gateway_cors_localhost_allowed);
    HU_RUN_TEST(test_gateway_cors_custom_origins);
    HU_RUN_TEST(test_gateway_parse_content_length_valid);
    HU_RUN_TEST(test_gateway_parse_content_length_too_large);
    HU_RUN_TEST(test_health_multiple_components);
    HU_RUN_TEST(test_gateway_config_defaults);
}
