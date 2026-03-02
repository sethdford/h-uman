#include "test_framework.h"
#include "seaclaw/gateway.h"
#include "seaclaw/health.h"
#include "seaclaw/core/allocator.h"
#include <string.h>

/* Gateway tests run with SC_GATEWAY_TEST_MODE - no actual port binding */

static void test_gateway_run_does_not_bind_in_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_gateway_config_t config = { .port = 9999, .test_mode = true };
    sc_error_t err = sc_gateway_run(&alloc, "127.0.0.1", 9999, &config);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_health_mark_ok_then_readiness(void) {
    sc_health_reset();
    sc_health_mark_ok("gateway");
    sc_allocator_t alloc = sc_system_allocator();
    sc_readiness_result_t r = sc_health_check_readiness(&alloc);
    SC_ASSERT_EQ(r.status, SC_READINESS_READY);
    if (r.checks) alloc.free(alloc.ctx, (void *)r.checks,
        r.check_count * sizeof(sc_component_check_t));
}

static void test_health_mark_error_then_not_ready(void) {
    sc_health_reset();
    sc_health_mark_ok("a");
    sc_health_mark_error("b", "connection refused");
    sc_allocator_t alloc = sc_system_allocator();
    sc_readiness_result_t r = sc_health_check_readiness(&alloc);
    SC_ASSERT_EQ(r.status, SC_READINESS_NOT_READY);
    SC_ASSERT_EQ(r.check_count, 2);
    if (r.checks) alloc.free(alloc.ctx, (void *)r.checks,
        r.check_count * sizeof(sc_component_check_t));
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
    /* Just verify struct exists and can be used */
    cfg.rate_limit_per_minute = 60;
    SC_ASSERT_EQ(cfg.rate_limit_per_minute, 60);
}

void run_gateway_tests(void) {
    SC_TEST_SUITE("gateway");
    SC_RUN_TEST(test_gateway_run_does_not_bind_in_test_mode);
    SC_RUN_TEST(test_health_mark_ok_then_readiness);
    SC_RUN_TEST(test_health_mark_error_then_not_ready);
    SC_RUN_TEST(test_health_empty_registry_ready);
    SC_RUN_TEST(test_rate_limiter_config);
}
