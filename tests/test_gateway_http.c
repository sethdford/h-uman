/* Gateway HTTP handling tests - config, health, rate limit. */
#include "test_framework.h"
#include "seaclaw/gateway.h"
#include "seaclaw/health.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/crypto.h"
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
    if (r.checks) alloc.free(alloc.ctx, (void *)r.checks, r.check_count * sizeof(sc_component_check_t));
}

static void test_gateway_readiness_when_error(void) {
    sc_health_reset();
    sc_health_mark_ok("a");
    sc_health_mark_error("gateway", "bind failed");
    sc_allocator_t alloc = sc_system_allocator();
    sc_readiness_result_t r = sc_health_check_readiness(&alloc);
    SC_ASSERT_EQ(r.status, SC_READINESS_NOT_READY);
    if (r.checks) alloc.free(alloc.ctx, (void *)r.checks, r.check_count * sizeof(sc_component_check_t));
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
    sc_gateway_config_t config = { .port = 9999, .test_mode = true };
    sc_error_t err = sc_gateway_run(&alloc, "127.0.0.1", 9999, &config);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_gateway_max_body_size_constant(void) {
    SC_ASSERT_EQ(SC_GATEWAY_MAX_BODY_SIZE, 65536);
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
}
