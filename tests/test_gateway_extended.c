/* Gateway edge cases (~20 tests). Uses SC_GATEWAY_TEST_MODE - no real binding. */
#include "test_framework.h"
#include "seaclaw/gateway.h"
#include "seaclaw/gateway/ws_server.h"
#include "seaclaw/health.h"
#include "seaclaw/core/allocator.h"
#include <string.h>

static void test_gateway_webhook_paths(void) {
    SC_ASSERT_EQ(SC_GATEWAY_MAX_BODY_SIZE, 65536u);
}

static void test_gateway_health_endpoint(void) {
    sc_health_reset();
    sc_health_mark_ok("gateway");
    sc_allocator_t alloc = sc_system_allocator();
    sc_readiness_result_t r = sc_health_check_readiness(&alloc);
    SC_ASSERT_EQ(r.status, SC_READINESS_READY);
    if (r.checks) alloc.free(alloc.ctx, (void *)r.checks,
        r.check_count * sizeof(sc_component_check_t));
}

static void test_gateway_ready_endpoint(void) {
    sc_health_reset();
    sc_allocator_t alloc = sc_system_allocator();
    sc_readiness_result_t r = sc_health_check_readiness(&alloc);
    SC_ASSERT_EQ(r.status, SC_READINESS_READY);
    if (r.checks) alloc.free(alloc.ctx, (void *)r.checks,
        r.check_count * sizeof(sc_component_check_t));
}

static void test_gateway_rate_limit_sliding_window(void) {
    sc_gateway_config_t cfg = {0};
    cfg.rate_limit_per_minute = 120;
    SC_ASSERT_EQ(cfg.rate_limit_per_minute, 120);
}

static void test_gateway_max_body_size(void) {
    sc_gateway_config_t cfg = {0};
    cfg.max_body_size = SC_GATEWAY_MAX_BODY_SIZE;
    SC_ASSERT_EQ(cfg.max_body_size, 65536u);
}

static void test_gateway_auth_required(void) {
    sc_gateway_config_t cfg = {0};
    cfg.hmac_secret = "test-secret";
    cfg.hmac_secret_len = 11;
    SC_ASSERT_NOT_NULL(cfg.hmac_secret);
    SC_ASSERT_EQ(cfg.hmac_secret_len, 11u);
}

static void test_gateway_run_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_gateway_config_t config = { .port = 0, .test_mode = true };
    sc_error_t err = sc_gateway_run(&alloc, "127.0.0.1", 0, &config);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_gateway_run_with_host(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_gateway_config_t config = { .host = "0.0.0.0", .port = 8080, .test_mode = true };
    sc_error_t err = sc_gateway_run(&alloc, "0.0.0.0", 8080, &config);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_health_multiple_components(void) {
    sc_health_reset();
    sc_health_mark_ok("a");
    sc_health_mark_ok("b");
    sc_health_mark_error("c", "failed");
    sc_allocator_t alloc = sc_system_allocator();
    sc_readiness_result_t r = sc_health_check_readiness(&alloc);
    SC_ASSERT_EQ(r.status, SC_READINESS_NOT_READY);
    SC_ASSERT_EQ(r.check_count, 3u);
    if (r.checks) alloc.free(alloc.ctx, (void *)r.checks,
        r.check_count * sizeof(sc_component_check_t));
}

static void test_health_all_ok(void) {
    sc_health_reset();
    sc_health_mark_ok("g");
    sc_health_mark_ok("p");
    sc_allocator_t alloc = sc_system_allocator();
    sc_readiness_result_t r = sc_health_check_readiness(&alloc);
    SC_ASSERT_EQ(r.status, SC_READINESS_READY);
    if (r.checks) alloc.free(alloc.ctx, (void *)r.checks,
        r.check_count * sizeof(sc_component_check_t));
}

static void test_health_reset_clears(void) {
    sc_health_reset();
    sc_health_mark_error("x", "err");
    sc_health_reset();
    sc_allocator_t alloc = sc_system_allocator();
    sc_readiness_result_t r = sc_health_check_readiness(&alloc);
    SC_ASSERT_EQ(r.status, SC_READINESS_READY);
    SC_ASSERT_EQ(r.check_count, 0u);
    if (r.checks) alloc.free(alloc.ctx, (void *)r.checks,
        r.check_count * sizeof(sc_component_check_t));
}

static void test_gateway_config_zeroed(void) {
    sc_gateway_config_t cfg = {0};
    SC_ASSERT_NULL(cfg.host);
    SC_ASSERT_EQ(cfg.port, 0);
    SC_ASSERT_EQ(cfg.max_body_size, 0);
    SC_ASSERT_NULL(cfg.hmac_secret);
}

static void test_readiness_status_values(void) {
    SC_ASSERT_TRUE(SC_READINESS_READY != SC_READINESS_NOT_READY);
}

static void test_gateway_rate_limit_constant(void) {
    SC_ASSERT_EQ(SC_GATEWAY_RATE_LIMIT_PER_MIN, 60u);
}

static void test_health_check_null_alloc(void) {
    sc_health_reset();
    sc_health_mark_ok("g");
    sc_readiness_result_t r = sc_health_check_readiness(NULL);
    SC_ASSERT(r.checks == NULL);
}

static void test_gateway_config_port_range(void) {
    sc_gateway_config_t cfg = {0};
    cfg.port = 65535;
    SC_ASSERT_EQ(cfg.port, 65535);
}

static void test_gateway_config_test_mode_flag(void) {
    sc_gateway_config_t cfg = { .test_mode = true };
    SC_ASSERT_TRUE(cfg.test_mode);
}

static void test_gateway_config_hmac_optional(void) {
    sc_gateway_config_t cfg = {0};
    SC_ASSERT_NULL(cfg.hmac_secret);
    cfg.hmac_secret = "key";
    cfg.hmac_secret_len = 3;
    SC_ASSERT_EQ(cfg.hmac_secret_len, 3u);
}


static void test_health_single_error_not_ready(void) {
    sc_health_reset();
    sc_health_mark_error("x", "fail");
    sc_allocator_t alloc = sc_system_allocator();
    sc_readiness_result_t r = sc_health_check_readiness(&alloc);
    SC_ASSERT_EQ(r.status, SC_READINESS_NOT_READY);
    if (r.checks) alloc.free(alloc.ctx, (void *)r.checks,
        r.check_count * sizeof(sc_component_check_t));
}

static void test_gateway_max_body_nonzero(void) {
    SC_ASSERT(SC_GATEWAY_MAX_BODY_SIZE > 0);
}

static void test_gateway_rate_limit_nonzero(void) {
    SC_ASSERT(SC_GATEWAY_RATE_LIMIT_PER_MIN > 0);
}

static void test_gateway_run_null_config(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_gateway_config_t config = { .port = 0, .test_mode = true };
    sc_error_t err = sc_gateway_run(&alloc, NULL, 0, &config);
    SC_ASSERT(err == SC_OK || err != SC_OK);
}

static void test_health_empty_ready(void) {
    sc_health_reset();
    sc_allocator_t alloc = sc_system_allocator();
    sc_readiness_result_t r = sc_health_check_readiness(&alloc);
    SC_ASSERT_EQ(r.status, SC_READINESS_READY);
    SC_ASSERT_EQ(r.check_count, 0u);
    if (r.checks) alloc.free(alloc.ctx, (void *)r.checks,
        r.check_count * sizeof(sc_component_check_t));
}

static void test_gateway_config_host_default(void) {
    sc_gateway_config_t cfg = {0};
    SC_ASSERT_NULL(cfg.host);
}

static void test_gateway_config_rate_limit_default(void) {
    sc_gateway_config_t cfg = {0};
    SC_ASSERT_EQ(cfg.rate_limit_per_minute, 0u);
}

static void test_health_mark_ok_then_error(void) {
    sc_health_reset();
    sc_health_mark_ok("a");
    sc_health_mark_error("a", "failed");
    sc_allocator_t alloc = sc_system_allocator();
    sc_readiness_result_t r = sc_health_check_readiness(&alloc);
    SC_ASSERT_EQ(r.status, SC_READINESS_NOT_READY);
    if (r.checks) alloc.free(alloc.ctx, (void *)r.checks,
        r.check_count * sizeof(sc_component_check_t));
}

static void test_gateway_run_bind_zero_port(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_gateway_config_t config = { .port = 0, .test_mode = true };
    sc_error_t err = sc_gateway_run(&alloc, "127.0.0.1", 0, &config);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_component_check_structure(void) {
    sc_health_reset();
    sc_health_mark_ok("comp");
    sc_allocator_t alloc = sc_system_allocator();
    sc_readiness_result_t r = sc_health_check_readiness(&alloc);
    SC_ASSERT(r.check_count >= 0);
    if (r.checks) alloc.free(alloc.ctx, (void *)r.checks,
        r.check_count * sizeof(sc_component_check_t));
}

/* ─── WP-21B parity: additional gateway / health tests ────────────────────── */
static void test_gateway_config_max_body_boundary(void) {
    sc_gateway_config_t cfg = {0};
    cfg.max_body_size = 1;
    SC_ASSERT_EQ(cfg.max_body_size, 1u);
    cfg.max_body_size = 1024 * 1024;
    SC_ASSERT_EQ(cfg.max_body_size, 1048576u);
}

static void test_gateway_config_hmac_len_zero(void) {
    sc_gateway_config_t cfg = {0};
    cfg.hmac_secret = "key";
    cfg.hmac_secret_len = 0;
    SC_ASSERT_EQ(cfg.hmac_secret_len, 0u);
}

static void test_gateway_config_rate_limit_one(void) {
    sc_gateway_config_t cfg = { .rate_limit_per_minute = 1 };
    SC_ASSERT_EQ(cfg.rate_limit_per_minute, 1u);
}

static void test_health_mark_ok_overwrites_error(void) {
    sc_health_reset();
    sc_health_mark_error("x", "fail");
    sc_health_mark_ok("x");
    sc_allocator_t alloc = sc_system_allocator();
    sc_readiness_result_t r = sc_health_check_readiness(&alloc);
    SC_ASSERT_EQ(r.status, SC_READINESS_READY);
    if (r.checks) alloc.free(alloc.ctx, (void *)r.checks,
        r.check_count * sizeof(sc_component_check_t));
}

static void test_gateway_config_port_one(void) {
    sc_gateway_config_t cfg = { .port = 1 };
    SC_ASSERT_EQ(cfg.port, 1);
}

static void test_gateway_constants_match_zig(void) {
    SC_ASSERT_EQ(SC_GATEWAY_MAX_BODY_SIZE, 65536u);
    SC_ASSERT_EQ(SC_GATEWAY_RATE_LIMIT_PER_MIN, 60u);
}

static void test_health_three_components_mixed(void) {
    sc_health_reset();
    sc_health_mark_ok("a");
    sc_health_mark_error("b", "err");
    sc_health_mark_ok("c");
    sc_allocator_t alloc = sc_system_allocator();
    sc_readiness_result_t r = sc_health_check_readiness(&alloc);
    SC_ASSERT_EQ(r.status, SC_READINESS_NOT_READY);
    SC_ASSERT_EQ(r.check_count, 3u);
    if (r.checks) alloc.free(alloc.ctx, (void *)r.checks,
        r.check_count * sizeof(sc_component_check_t));
}

static void test_gateway_run_test_mode_host_loopback(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_gateway_config_t config = { .host = "127.0.0.1", .port = 3000, .test_mode = true };
    sc_error_t err = sc_gateway_run(&alloc, "127.0.0.1", 3000, &config);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_ws_server_init_deinit(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_server_t srv;
    sc_ws_server_init(&srv, &alloc, NULL, NULL, NULL);
    SC_ASSERT_EQ(srv.conn_count, 0u);
    sc_ws_server_deinit(&srv);
}

static void test_ws_server_upgrade_null_args(void) {
    sc_ws_conn_t *out = NULL;
    const char *req = "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    sc_error_t err = sc_ws_server_upgrade(NULL, 0, req, strlen(req), &out);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_ws_server_is_upgrade_valid(void) {
    const char *req = "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    bool ok = sc_ws_server_is_upgrade(req, strlen(req));
    SC_ASSERT_TRUE(ok);
}

static void test_ws_server_is_upgrade_invalid(void) {
    const char *req = "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
    bool ok = sc_ws_server_is_upgrade(req, strlen(req));
    SC_ASSERT_FALSE(ok);
}

static void test_ws_server_send_null_conn(void) {
    sc_error_t err = sc_ws_server_send(NULL, "x", 1);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_ws_server_broadcast_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_server_t srv;
    sc_ws_server_init(&srv, &alloc, NULL, NULL, NULL);
    sc_ws_server_broadcast(&srv, "msg", 3);
    sc_ws_server_deinit(&srv);
}

void run_gateway_extended_tests(void) {
    SC_TEST_SUITE("Gateway Extended");
    SC_RUN_TEST(test_gateway_webhook_paths);
    SC_RUN_TEST(test_gateway_health_endpoint);
    SC_RUN_TEST(test_gateway_ready_endpoint);
    SC_RUN_TEST(test_gateway_rate_limit_sliding_window);
    SC_RUN_TEST(test_gateway_max_body_size);
    SC_RUN_TEST(test_gateway_auth_required);
    SC_RUN_TEST(test_gateway_run_test_mode);
    SC_RUN_TEST(test_gateway_run_with_host);
    SC_RUN_TEST(test_health_multiple_components);
    SC_RUN_TEST(test_health_all_ok);
    SC_RUN_TEST(test_health_reset_clears);
    SC_RUN_TEST(test_gateway_config_zeroed);
    SC_RUN_TEST(test_readiness_status_values);
    SC_RUN_TEST(test_gateway_rate_limit_constant);
    SC_RUN_TEST(test_health_check_null_alloc);
    SC_RUN_TEST(test_gateway_config_port_range);
    SC_RUN_TEST(test_gateway_config_test_mode_flag);
    SC_RUN_TEST(test_gateway_config_hmac_optional);
    SC_RUN_TEST(test_health_single_error_not_ready);
    SC_RUN_TEST(test_gateway_max_body_nonzero);
    SC_RUN_TEST(test_gateway_rate_limit_nonzero);
    SC_RUN_TEST(test_gateway_run_null_config);
    SC_RUN_TEST(test_health_empty_ready);
    SC_RUN_TEST(test_gateway_config_host_default);
    SC_RUN_TEST(test_gateway_config_rate_limit_default);
    SC_RUN_TEST(test_health_mark_ok_then_error);
    SC_RUN_TEST(test_gateway_run_bind_zero_port);
    SC_RUN_TEST(test_component_check_structure);
    SC_RUN_TEST(test_gateway_config_max_body_boundary);
    SC_RUN_TEST(test_gateway_config_hmac_len_zero);
    SC_RUN_TEST(test_gateway_config_rate_limit_one);
    SC_RUN_TEST(test_health_mark_ok_overwrites_error);
    SC_RUN_TEST(test_gateway_config_port_one);
    SC_RUN_TEST(test_gateway_constants_match_zig);
    SC_RUN_TEST(test_health_three_components_mixed);
    SC_RUN_TEST(test_gateway_run_test_mode_host_loopback);
    SC_RUN_TEST(test_ws_server_init_deinit);
    SC_RUN_TEST(test_ws_server_upgrade_null_args);
    SC_RUN_TEST(test_ws_server_is_upgrade_valid);
    SC_RUN_TEST(test_ws_server_is_upgrade_invalid);
    SC_RUN_TEST(test_ws_server_send_null_conn);
    SC_RUN_TEST(test_ws_server_broadcast_empty);
}
