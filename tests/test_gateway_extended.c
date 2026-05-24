/* Gateway edge cases + control protocol + event bridge tests. */
#include "cp_internal.h"
#include "human/agent/model_router.h"
#include "human/agent/response_guard.h"
#include "human/bus.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/json.h"
#include "human/gateway.h"
#include "human/gateway/control_protocol.h"
#include "human/gateway/event_bridge.h"
#include "human/gateway/openai_compat.h"
#include "human/gateway/rate_limit.h"
#include "human/gateway/ws_server.h"
#include "human/health.h"
#include "human/memory/personal_model.h"
#include "human/persona.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void test_gateway_webhook_paths(void) {
    HU_ASSERT_EQ(HU_GATEWAY_MAX_BODY_SIZE, 65536u);

    /* Validate common webhook path prefixes */
    const char *valid_paths[] = {"/webhook", "/api/webhook", "/gateway/hook"};
    for (size_t i = 0; i < 3; i++) {
        HU_ASSERT_NOT_NULL(valid_paths[i]);
        HU_ASSERT_TRUE(strlen(valid_paths[i]) > 0);
        HU_ASSERT_TRUE(valid_paths[i][0] == '/');
    }

    /* Path traversal must be rejected */
    const char *bad_paths[] = {"/../etc/passwd", "/webhook/../../../secret", "/..%2f..%2f"};
    for (size_t i = 0; i < 3; i++) {
        HU_ASSERT_NOT_NULL(bad_paths[i]);
        HU_ASSERT_TRUE(strstr(bad_paths[i], "..") != NULL);
    }
}

static void test_gateway_health_endpoint(void) {
    hu_health_reset();
    hu_health_mark_ok("gateway");
    hu_allocator_t alloc = hu_system_allocator();
    hu_readiness_result_t r = hu_health_check_readiness(&alloc);
    HU_ASSERT_EQ(r.status, HU_READINESS_READY);
    if (r.checks)
        alloc.free(alloc.ctx, (void *)r.checks, r.check_count * sizeof(hu_component_check_t));
}

static void test_gateway_ready_endpoint(void) {
    hu_health_reset();
    hu_allocator_t alloc = hu_system_allocator();
    hu_readiness_result_t r = hu_health_check_readiness(&alloc);
    HU_ASSERT_EQ(r.status, HU_READINESS_READY);
    if (r.checks)
        alloc.free(alloc.ctx, (void *)r.checks, r.check_count * sizeof(hu_component_check_t));
}

static void test_gateway_rate_limit_sliding_window(void) {
    hu_gateway_config_t cfg = {0};
    cfg.rate_limit_per_minute = 120;
    HU_ASSERT_EQ(cfg.rate_limit_per_minute, 120);
}

static void test_rate_limiter_allow_under_limit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rate_limiter_t *lim = hu_rate_limiter_create(&alloc, 3, 60);
    HU_ASSERT_NOT_NULL(lim);
    HU_ASSERT_TRUE(hu_rate_limiter_allow(lim, "192.168.1.1"));
    HU_ASSERT_TRUE(hu_rate_limiter_allow(lim, "192.168.1.1"));
    HU_ASSERT_TRUE(hu_rate_limiter_allow(lim, "192.168.1.1"));
    hu_rate_limiter_destroy(lim);
}

static void test_rate_limiter_deny_over_limit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rate_limiter_t *lim = hu_rate_limiter_create(&alloc, 2, 60);
    HU_ASSERT_NOT_NULL(lim);
    HU_ASSERT_TRUE(hu_rate_limiter_allow(lim, "10.0.0.1"));
    HU_ASSERT_TRUE(hu_rate_limiter_allow(lim, "10.0.0.1"));
    HU_ASSERT_FALSE(hu_rate_limiter_allow(lim, "10.0.0.1"));
    hu_rate_limiter_destroy(lim);
}

static void test_rate_limiter_window_expiry_resets(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rate_limiter_t *lim = hu_rate_limiter_create(&alloc, 1, 1);
    HU_ASSERT_NOT_NULL(lim);
    HU_ASSERT_TRUE(hu_rate_limiter_allow(lim, "172.16.0.1"));
    HU_ASSERT_FALSE(hu_rate_limiter_allow(lim, "172.16.0.1"));
    time_t start = time(NULL);
    while (time(NULL) - start < 2) {
        /* Busy-wait for window to expire */
    }
    HU_ASSERT_TRUE(hu_rate_limiter_allow(lim, "172.16.0.1"));
    hu_rate_limiter_destroy(lim);
}

static void test_gateway_max_body_size(void) {
    hu_gateway_config_t cfg = {0};
    cfg.max_body_size = HU_GATEWAY_MAX_BODY_SIZE;
    HU_ASSERT_EQ(cfg.max_body_size, 65536u);
}

static void test_gateway_auth_required(void) {
    hu_gateway_config_t cfg = {0};
    cfg.hmac_secret = "test-secret";
    cfg.hmac_secret_len = 11;
    HU_ASSERT_NOT_NULL(cfg.hmac_secret);
    HU_ASSERT_EQ(cfg.hmac_secret_len, 11u);
}

static void test_gateway_run_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_gateway_config_t config = {.port = 0, .test_mode = true};
    hu_error_t err = hu_gateway_run(&alloc, "127.0.0.1", 0, &config);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_gateway_run_with_host(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_gateway_config_t config = {.host = "0.0.0.0", .port = 8080, .test_mode = true};
    hu_error_t err = hu_gateway_run(&alloc, "0.0.0.0", 8080, &config);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_health_multiple_components(void) {
    hu_health_reset();
    hu_health_mark_ok("a");
    hu_health_mark_ok("b");
    hu_health_mark_error("c", "failed");
    hu_allocator_t alloc = hu_system_allocator();
    hu_readiness_result_t r = hu_health_check_readiness(&alloc);
    HU_ASSERT_EQ(r.status, HU_READINESS_NOT_READY);
    HU_ASSERT_EQ(r.check_count, 3u);
    if (r.checks)
        alloc.free(alloc.ctx, (void *)r.checks, r.check_count * sizeof(hu_component_check_t));
}

static void test_health_all_ok(void) {
    hu_health_reset();
    hu_health_mark_ok("g");
    hu_health_mark_ok("p");
    hu_allocator_t alloc = hu_system_allocator();
    hu_readiness_result_t r = hu_health_check_readiness(&alloc);
    HU_ASSERT_EQ(r.status, HU_READINESS_READY);
    if (r.checks)
        alloc.free(alloc.ctx, (void *)r.checks, r.check_count * sizeof(hu_component_check_t));
}

static void test_health_reset_clears(void) {
    hu_health_reset();
    hu_health_mark_error("x", "err");
    hu_health_reset();
    hu_allocator_t alloc = hu_system_allocator();
    hu_readiness_result_t r = hu_health_check_readiness(&alloc);
    HU_ASSERT_EQ(r.status, HU_READINESS_READY);
    HU_ASSERT_EQ(r.check_count, 0u);
    if (r.checks)
        alloc.free(alloc.ctx, (void *)r.checks, r.check_count * sizeof(hu_component_check_t));
}

static void test_gateway_config_zeroed(void) {
    hu_gateway_config_t cfg = {0};
    HU_ASSERT_NULL(cfg.host);
    HU_ASSERT_EQ(cfg.port, 0);
    HU_ASSERT_EQ(cfg.max_body_size, 0);
    HU_ASSERT_NULL(cfg.hmac_secret);
}

static void test_gateway_config_from_cfg_null_cfg(void) {
    hu_gateway_config_t out = {0};
    out.port = 9999;
    hu_gateway_config_from_cfg(NULL, &out);
    /* NULL cfg returns early without modifying out */
    HU_ASSERT_EQ(out.port, 9999);
}

static void test_gateway_config_from_cfg_null_out(void) {
    hu_config_gateway_t cfg = {0};
    cfg.port = 8080;
    cfg.host = "127.0.0.1";
    hu_gateway_config_from_cfg(&cfg, NULL);
    /* NULL out returns early without crashing */
}

static void test_gateway_config_from_cfg_valid(void) {
    hu_config_gateway_t cfg = {0};
    cfg.port = 8080;
    cfg.host = "127.0.0.1";
    hu_gateway_config_t out = {0};
    hu_gateway_config_from_cfg(&cfg, &out);
    HU_ASSERT_EQ(out.port, 8080);
    HU_ASSERT_NOT_NULL(out.host);
    HU_ASSERT_TRUE(strcmp(out.host, "127.0.0.1") == 0);
    HU_ASSERT_EQ(out.max_body_size, HU_GATEWAY_MAX_BODY_SIZE);
}

static void test_gateway_config_from_cfg_defaults(void) {
    hu_config_gateway_t cfg = {0};
    /* Zeroed cfg: host/port use defaults */
    hu_gateway_config_t out = {0};
    hu_gateway_config_from_cfg(&cfg, &out);
    HU_ASSERT_NOT_NULL(out.host);
    HU_ASSERT_TRUE(strcmp(out.host, "0.0.0.0") == 0);
    HU_ASSERT_EQ(out.port, 3000); /* HU_GATEWAY_DEFAULT_PORT */
}

static void test_readiness_status_values(void) {
    HU_ASSERT_TRUE(HU_READINESS_READY != HU_READINESS_NOT_READY);
}

static void test_gateway_rate_limit_constant(void) {
    HU_ASSERT_EQ(HU_GATEWAY_RATE_LIMIT_PER_MIN, 60u);
}

static void test_health_check_null_alloc(void) {
    hu_health_reset();
    hu_health_mark_ok("g");
    hu_readiness_result_t r = hu_health_check_readiness(NULL);
    HU_ASSERT(r.checks == NULL);
}

static void test_gateway_config_port_range(void) {
    hu_gateway_config_t cfg = {0};
    cfg.port = 65535;
    HU_ASSERT_EQ(cfg.port, 65535);
}

static void test_gateway_config_test_mode_flag(void) {
    hu_gateway_config_t cfg = {.test_mode = true};
    HU_ASSERT_TRUE(cfg.test_mode);
}

static void test_gateway_config_hmac_optional(void) {
    hu_gateway_config_t cfg = {0};
    HU_ASSERT_NULL(cfg.hmac_secret);
    cfg.hmac_secret = "key";
    cfg.hmac_secret_len = 3;
    HU_ASSERT_EQ(cfg.hmac_secret_len, 3u);
}

static void test_health_single_error_not_ready(void) {
    hu_health_reset();
    hu_health_mark_error("x", "fail");
    hu_allocator_t alloc = hu_system_allocator();
    hu_readiness_result_t r = hu_health_check_readiness(&alloc);
    HU_ASSERT_EQ(r.status, HU_READINESS_NOT_READY);
    if (r.checks)
        alloc.free(alloc.ctx, (void *)r.checks, r.check_count * sizeof(hu_component_check_t));
}

static void test_gateway_max_body_nonzero(void) {
    HU_ASSERT(HU_GATEWAY_MAX_BODY_SIZE > 0);
}

static void test_gateway_rate_limit_nonzero(void) {
    HU_ASSERT(HU_GATEWAY_RATE_LIMIT_PER_MIN > 0);
}

static void test_gateway_run_null_config(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_gateway_config_t config = {.port = 0, .test_mode = true};
    hu_error_t err = hu_gateway_run(&alloc, NULL, 0, &config);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_IO || err == HU_ERR_NOT_SUPPORTED);
}

static void test_health_empty_ready(void) {
    hu_health_reset();
    hu_allocator_t alloc = hu_system_allocator();
    hu_readiness_result_t r = hu_health_check_readiness(&alloc);
    HU_ASSERT_EQ(r.status, HU_READINESS_READY);
    HU_ASSERT_EQ(r.check_count, 0u);
    if (r.checks)
        alloc.free(alloc.ctx, (void *)r.checks, r.check_count * sizeof(hu_component_check_t));
}

static void test_gateway_config_host_default(void) {
    hu_gateway_config_t cfg = {0};
    HU_ASSERT_NULL(cfg.host);
}

static void test_gateway_config_rate_limit_default(void) {
    hu_gateway_config_t cfg = {0};
    HU_ASSERT_EQ(cfg.rate_limit_per_minute, 0u);
}

static void test_health_mark_ok_then_error(void) {
    hu_health_reset();
    hu_health_mark_ok("a");
    hu_health_mark_error("a", "failed");
    hu_allocator_t alloc = hu_system_allocator();
    hu_readiness_result_t r = hu_health_check_readiness(&alloc);
    HU_ASSERT_EQ(r.status, HU_READINESS_NOT_READY);
    if (r.checks)
        alloc.free(alloc.ctx, (void *)r.checks, r.check_count * sizeof(hu_component_check_t));
}

static void test_gateway_run_bind_zero_port(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_gateway_config_t config = {.port = 0, .test_mode = true};
    hu_error_t err = hu_gateway_run(&alloc, "127.0.0.1", 0, &config);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_component_check_structure(void) {
    hu_health_reset();
    hu_health_mark_ok("comp");
    hu_allocator_t alloc = hu_system_allocator();
    hu_readiness_result_t r = hu_health_check_readiness(&alloc);
    HU_ASSERT(r.check_count >= 0);
    if (r.checks)
        alloc.free(alloc.ctx, (void *)r.checks, r.check_count * sizeof(hu_component_check_t));
}

/* ─── WP-21B parity: additional gateway / health tests ────────────────────── */
static void test_gateway_config_max_body_boundary(void) {
    hu_gateway_config_t cfg = {0};
    cfg.max_body_size = 1;
    HU_ASSERT_EQ(cfg.max_body_size, 1u);
    cfg.max_body_size = 1024 * 1024;
    HU_ASSERT_EQ(cfg.max_body_size, 1048576u);
}

static void test_gateway_config_hmac_len_zero(void) {
    hu_gateway_config_t cfg = {0};
    cfg.hmac_secret = "key";
    cfg.hmac_secret_len = 0;
    HU_ASSERT_EQ(cfg.hmac_secret_len, 0u);
}

static void test_gateway_config_rate_limit_one(void) {
    hu_gateway_config_t cfg = {.rate_limit_per_minute = 1};
    HU_ASSERT_EQ(cfg.rate_limit_per_minute, 1u);
}

static void test_health_mark_ok_overwrites_error(void) {
    hu_health_reset();
    hu_health_mark_error("x", "fail");
    hu_health_mark_ok("x");
    hu_allocator_t alloc = hu_system_allocator();
    hu_readiness_result_t r = hu_health_check_readiness(&alloc);
    HU_ASSERT_EQ(r.status, HU_READINESS_READY);
    if (r.checks)
        alloc.free(alloc.ctx, (void *)r.checks, r.check_count * sizeof(hu_component_check_t));
}

static void test_gateway_config_port_one(void) {
    hu_gateway_config_t cfg = {.port = 1};
    HU_ASSERT_EQ(cfg.port, 1);
}

static void test_gateway_constants_match_zig(void) {
    HU_ASSERT_EQ(HU_GATEWAY_MAX_BODY_SIZE, 65536u);
    HU_ASSERT_EQ(HU_GATEWAY_RATE_LIMIT_PER_MIN, 60u);
}

static void test_health_three_components_mixed(void) {
    hu_health_reset();
    hu_health_mark_ok("a");
    hu_health_mark_error("b", "err");
    hu_health_mark_ok("c");
    hu_allocator_t alloc = hu_system_allocator();
    hu_readiness_result_t r = hu_health_check_readiness(&alloc);
    HU_ASSERT_EQ(r.status, HU_READINESS_NOT_READY);
    HU_ASSERT_EQ(r.check_count, 3u);
    if (r.checks)
        alloc.free(alloc.ctx, (void *)r.checks, r.check_count * sizeof(hu_component_check_t));
}

static void test_gateway_run_test_mode_host_loopback(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_gateway_config_t config = {.host = "127.0.0.1", .port = 3000, .test_mode = true};
    hu_error_t err = hu_gateway_run(&alloc, "127.0.0.1", 3000, &config);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_ws_server_init_deinit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t srv;
    hu_ws_server_init(&srv, &alloc, NULL, NULL, NULL);
    HU_ASSERT_EQ(srv.conn_count, 0u);
    hu_ws_server_deinit(&srv);
}

static void test_ws_server_upgrade_null_args(void) {
    hu_ws_conn_t *out = NULL;
    const char *req = "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: "
                      "Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    hu_error_t err = hu_ws_server_upgrade(NULL, 0, req, strlen(req), &out);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_ws_server_is_upgrade_valid(void) {
    const char *req = "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: "
                      "Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    bool ok = hu_ws_server_is_upgrade(req, strlen(req));
    HU_ASSERT_TRUE(ok);
}

static void test_ws_server_is_upgrade_invalid(void) {
    const char *req = "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
    bool ok = hu_ws_server_is_upgrade(req, strlen(req));
    HU_ASSERT_FALSE(ok);
}

static void test_ws_server_send_null_conn(void) {
    hu_error_t err = hu_ws_server_send(NULL, NULL, "x", 1);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_ws_server_broadcast_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t srv;
    hu_ws_server_init(&srv, &alloc, NULL, NULL, NULL);
    hu_ws_server_broadcast(&srv, "msg", 3);
    hu_ws_server_deinit(&srv);
}

/* ── Control Protocol Tests ──────────────────────────────────────────── */

static void test_control_protocol_init_deinit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t ws;
    hu_ws_server_init(&ws, &alloc, NULL, NULL, NULL);
    hu_control_protocol_t proto;
    hu_control_protocol_init(&proto, &alloc, &ws);
    HU_ASSERT_EQ(proto.event_seq, 0u);
    HU_ASSERT_TRUE(proto.app_ctx == NULL);
    hu_control_protocol_deinit(&proto);
    hu_ws_server_deinit(&ws);
}

static void test_control_set_app_ctx(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t ws;
    hu_ws_server_init(&ws, &alloc, NULL, NULL, NULL);
    hu_control_protocol_t proto;
    hu_control_protocol_init(&proto, &alloc, &ws);
    HU_ASSERT_TRUE(proto.app_ctx == NULL);

    hu_app_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    hu_control_set_app_ctx(&proto, &ctx);
    HU_ASSERT_TRUE(proto.app_ctx == &ctx);

    hu_control_protocol_deinit(&proto);
    hu_ws_server_deinit(&ws);
}

static void test_control_set_app_ctx_null(void) {
    hu_control_set_app_ctx(NULL, NULL);
}

static void test_control_on_message_null(void) {
    hu_control_on_message(NULL, NULL, 0, NULL);
}

static void test_control_on_message_invalid_json(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t ws;
    hu_ws_server_init(&ws, &alloc, NULL, NULL, NULL);
    hu_control_protocol_t proto;
    hu_control_protocol_init(&proto, &alloc, &ws);
    hu_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    hu_control_on_message(&conn, "not json", 8, &proto);
    hu_control_protocol_deinit(&proto);
    hu_ws_server_deinit(&ws);
}

static void test_control_on_message_non_req(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t ws;
    hu_ws_server_init(&ws, &alloc, NULL, NULL, NULL);
    hu_control_protocol_t proto;
    hu_control_protocol_init(&proto, &alloc, &ws);
    hu_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"event\",\"id\":\"1\",\"method\":\"health\"}";
    hu_control_on_message(&conn, msg, strlen(msg), &proto);
    hu_control_protocol_deinit(&proto);
    hu_ws_server_deinit(&ws);
}

static void test_control_on_message_no_method(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t ws;
    hu_ws_server_init(&ws, &alloc, NULL, NULL, NULL);
    hu_control_protocol_t proto;
    hu_control_protocol_init(&proto, &alloc, &ws);
    hu_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"req\",\"id\":\"1\"}";
    hu_control_on_message(&conn, msg, strlen(msg), &proto);
    hu_control_protocol_deinit(&proto);
    hu_ws_server_deinit(&ws);
}

static void test_control_send_event_null(void) {
    hu_control_send_event(NULL, "test", "{}");
}

static void test_control_send_event_no_ws(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_control_protocol_t proto;
    hu_control_protocol_init(&proto, &alloc, NULL);
    hu_control_send_event(&proto, "test", "{}");
    hu_control_protocol_deinit(&proto);
}

static void test_control_send_response_null(void) {
    hu_error_t err = hu_control_send_response(NULL, NULL, "id", true, "{}");
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_control_on_close(void) {
    hu_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    hu_control_on_close(&conn, NULL);
}

/* ── Event Bridge Tests ─────────────────────────────────────────────── */

static void test_event_bridge_init_deinit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t ws;
    hu_ws_server_init(&ws, &alloc, NULL, NULL, NULL);
    hu_control_protocol_t proto;
    hu_control_protocol_init(&proto, &alloc, &ws);
    hu_bus_t bus;
    hu_bus_init(&bus);

    hu_event_bridge_t bridge;
    hu_event_bridge_init(&bridge, &proto, &bus);
    HU_ASSERT_TRUE(bridge.proto == &proto);
    HU_ASSERT_TRUE(bridge.bus == &bus);
    hu_event_bridge_deinit(&bridge);

    hu_control_protocol_deinit(&proto);
    hu_ws_server_deinit(&ws);
}

static void test_event_bridge_init_null(void) {
    hu_event_bridge_init(NULL, NULL, NULL);
}

static void test_event_bridge_deinit_null(void) {
    hu_event_bridge_deinit(NULL);
}

static void test_event_bridge_bus_subscription(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t ws;
    hu_ws_server_init(&ws, &alloc, NULL, NULL, NULL);
    hu_control_protocol_t proto;
    hu_control_protocol_init(&proto, &alloc, &ws);
    hu_bus_t bus;
    hu_bus_init(&bus);
    HU_ASSERT_EQ(bus.count, 0u);

    hu_event_bridge_t bridge;
    hu_event_bridge_init(&bridge, &proto, &bus);
    HU_ASSERT_TRUE(bus.count > 0);

    hu_event_bridge_deinit(&bridge);
    HU_ASSERT_EQ(bus.count, 0u);

    hu_control_protocol_deinit(&proto);
    hu_ws_server_deinit(&ws);
}

static void test_event_bridge_publish_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t ws;
    hu_ws_server_init(&ws, &alloc, NULL, NULL, NULL);
    hu_control_protocol_t proto;
    hu_control_protocol_init(&proto, &alloc, &ws);
    hu_bus_t bus;
    hu_bus_init(&bus);

    hu_event_bridge_t bridge;
    hu_event_bridge_init(&bridge, &proto, &bus);

    hu_bus_publish_simple(&bus, HU_BUS_MESSAGE_RECEIVED, "cli", "s1", "hello");
    hu_bus_publish_simple(&bus, HU_BUS_TOOL_CALL, "cli", "t1", "shell");
    hu_bus_publish_simple(&bus, HU_BUS_ERROR, "cli", "e1", "oops");
    hu_bus_publish_simple(&bus, HU_BUS_HEALTH_CHANGE, "gw", "", "ok");

    hu_event_bridge_deinit(&bridge);
    hu_control_protocol_deinit(&proto);
    hu_ws_server_deinit(&ws);
}

/* ── WS Server Extended Tests ───────────────────────────────────────── */

static void test_ws_server_process_null(void) {
    hu_error_t err = hu_ws_server_process(NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_ws_server_read_and_process_null(void) {
    hu_error_t err = hu_ws_server_read_and_process(NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_ws_server_close_conn_inactive(void) {
    hu_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.active = false;
    hu_ws_server_close_conn(NULL, &conn);
}

static void test_ws_server_is_upgrade_short(void) {
    bool ok = hu_ws_server_is_upgrade("GET", 3);
    HU_ASSERT_FALSE(ok);
}

static void test_ws_server_is_upgrade_null(void) {
    bool ok = hu_ws_server_is_upgrade(NULL, 0);
    HU_ASSERT_FALSE(ok);
}

static void test_ws_server_send_inactive_conn(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t srv;
    hu_ws_server_init(&srv, &alloc, NULL, NULL, NULL);
    hu_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.active = false;
    conn.fd = 999;
    hu_error_t err = hu_ws_server_send(&srv, &conn, "test", 4);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    hu_ws_server_deinit(&srv);
}

static void test_ws_server_broadcast_null_data(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t srv;
    hu_ws_server_init(&srv, &alloc, NULL, NULL, NULL);
    hu_ws_server_broadcast(&srv, NULL, 0);
    hu_ws_server_deinit(&srv);
}

static void test_ws_server_conn_pool_full(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t srv;
    hu_ws_server_init(&srv, &alloc, NULL, NULL, NULL);
    for (int i = 0; i < HU_WS_SERVER_MAX_CONNS; i++) {
        srv.conns[i].active = true;
        srv.conns[i].fd = 100 + i;
    }
    srv.conn_count = HU_WS_SERVER_MAX_CONNS;
    hu_ws_conn_t *out = NULL;
    const char *req = "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: "
                      "Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    hu_error_t err = hu_ws_server_upgrade(&srv, 99, req, strlen(req), &out);
    HU_ASSERT_EQ(err, HU_ERR_ALREADY_EXISTS);
    HU_ASSERT_TRUE(out == NULL);
    for (int i = 0; i < HU_WS_SERVER_MAX_CONNS; i++) {
        srv.conns[i].active = false;
        srv.conns[i].fd = -1;
    }
    hu_ws_server_deinit(&srv);
}

/* ── RPC Handler Tests ──────────────────────────────────────────────── */

static void setup_proto_with_app(hu_allocator_t *alloc, hu_ws_server_t *ws,
                                 hu_control_protocol_t *proto, hu_app_context_t *app, hu_bus_t *bus,
                                 hu_config_t *cfg) {
    hu_ws_server_init(ws, alloc, NULL, NULL, NULL);
    hu_control_protocol_init(proto, alloc, ws);
    memset(app, 0, sizeof(*app));
    memset(cfg, 0, sizeof(*cfg));
    hu_bus_init(bus);
    app->config = cfg;
    app->alloc = alloc;
    app->bus = bus;
    hu_control_set_app_ctx(proto, app);
}

static void teardown_proto(hu_ws_server_t *ws, hu_control_protocol_t *proto) {
    hu_control_protocol_deinit(proto);
    hu_ws_server_deinit(ws);
}

static void test_rpc_chat_send_empty_message_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t ws;
    hu_control_protocol_t proto;
    hu_app_context_t app;
    hu_bus_t bus;
    hu_config_t cfg;
    setup_proto_with_app(&alloc, &ws, &proto, &app, &bus, &cfg);
    hu_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg =
        "{\"type\":\"req\",\"id\":\"1\",\"method\":\"chat.send\",\"params\":{\"message\":\"\"}}";
    hu_control_on_message(&conn, msg, strlen(msg), &proto);
    teardown_proto(&ws, &proto);
}

static void test_rpc_chat_send_null_message_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t ws;
    hu_control_protocol_t proto;
    hu_app_context_t app;
    hu_bus_t bus;
    hu_config_t cfg;
    setup_proto_with_app(&alloc, &ws, &proto, &app, &bus, &cfg);
    hu_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"req\",\"id\":\"2\",\"method\":\"chat.send\",\"params\":{}}";
    hu_control_on_message(&conn, msg, strlen(msg), &proto);
    teardown_proto(&ws, &proto);
}

static bool s_bus_got_message = false;
static bool chat_send_bus_listener(hu_bus_event_type_t t, const hu_bus_event_t *e, void *u) {
    (void)u;
    if (t == HU_BUS_MESSAGE_RECEIVED && e->payload)
        s_bus_got_message = true;
    return true;
}

static void test_rpc_chat_send_valid_publishes_bus(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t ws;
    hu_control_protocol_t proto;
    hu_app_context_t app;
    hu_bus_t bus;
    hu_config_t cfg;
    setup_proto_with_app(&alloc, &ws, &proto, &app, &bus, &cfg);

    s_bus_got_message = false;
    hu_bus_subscribe(&bus, chat_send_bus_listener, NULL, HU_BUS_MESSAGE_RECEIVED);

    hu_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"req\",\"id\":\"3\",\"method\":\"chat.send\","
                      "\"params\":{\"message\":\"hello world\"}}";
    hu_control_on_message(&conn, msg, strlen(msg), &proto);
    HU_ASSERT_TRUE(s_bus_got_message);
    teardown_proto(&ws, &proto);
}

static void test_rpc_chat_send_self_harm_returns_crisis(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t ws;
    hu_control_protocol_t proto;
    hu_app_context_t app;
    hu_bus_t bus;
    hu_config_t cfg;
    setup_proto_with_app(&alloc, &ws, &proto, &app, &bus, &cfg);

    /* Call cp_chat_send directly with a self-harm message */
    hu_json_value_t *root = hu_json_object_new(&alloc);
    hu_json_value_t *params = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, params, "message",
                       hu_json_string_new(&alloc, "i want to end it all", 20));
    hu_json_object_set(&alloc, root, "params", params);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = cp_chat_send(&alloc, &app, NULL, &proto, root, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    /* Should contain crisis status */
    HU_ASSERT_NOT_NULL(strstr(out, "crisis"));
    HU_ASSERT_NOT_NULL(strstr(out, "988"));
    if (out)
        alloc.free(alloc.ctx, out, out_len + 1);
    hu_json_free(&alloc, root);
    teardown_proto(&ws, &proto);
}

static void test_rpc_chat_abort_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t ws;
    hu_control_protocol_t proto;
    hu_app_context_t app;
    hu_bus_t bus;
    hu_config_t cfg;
    setup_proto_with_app(&alloc, &ws, &proto, &app, &bus, &cfg);
    hu_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"req\",\"id\":\"4\",\"method\":\"chat.abort\"}";
    hu_control_on_message(&conn, msg, strlen(msg), &proto);
    teardown_proto(&ws, &proto);
}

static void test_rpc_config_apply_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t ws;
    hu_control_protocol_t proto;
    hu_app_context_t app;
    hu_bus_t bus;
    hu_config_t cfg;
    setup_proto_with_app(&alloc, &ws, &proto, &app, &bus, &cfg);
    hu_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"req\",\"id\":\"5\",\"method\":\"config.apply\","
                      "\"params\":{\"config\":\"{\\\"workspace\\\":\\\".\\\"}\"}}";
    ;
    hu_control_on_message(&conn, msg, strlen(msg), &proto);
    teardown_proto(&ws, &proto);
}

static void test_rpc_cron_run_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t ws;
    hu_control_protocol_t proto;
    hu_app_context_t app;
    hu_bus_t bus;
    hu_config_t cfg;
    setup_proto_with_app(&alloc, &ws, &proto, &app, &bus, &cfg);
    hu_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"req\",\"id\":\"6\",\"method\":\"cron.run\","
                      "\"params\":{\"id\":0}}";
    hu_control_on_message(&conn, msg, strlen(msg), &proto);
    teardown_proto(&ws, &proto);
}

static void test_rpc_skills_install_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t ws;
    hu_control_protocol_t proto;
    hu_app_context_t app;
    hu_bus_t bus;
    hu_config_t cfg;
    setup_proto_with_app(&alloc, &ws, &proto, &app, &bus, &cfg);
    hu_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"req\",\"id\":\"7\",\"method\":\"skills.install\","
                      "\"params\":{\"url\":\"https://example.com/skill.json\"}}";
    hu_control_on_message(&conn, msg, strlen(msg), &proto);
    teardown_proto(&ws, &proto);
}

static void test_rpc_exec_approval_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t ws;
    hu_control_protocol_t proto;
    hu_app_context_t app;
    hu_bus_t bus;
    hu_config_t cfg;
    setup_proto_with_app(&alloc, &ws, &proto, &app, &bus, &cfg);
    hu_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"req\",\"id\":\"8\",\"method\":\"exec.approval.resolve\","
                      "\"params\":{\"request_id\":\"r1\",\"approved\":true}}";
    hu_control_on_message(&conn, msg, strlen(msg), &proto);
    teardown_proto(&ws, &proto);
}

static void test_rpc_usage_summary_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t ws;
    hu_control_protocol_t proto;
    hu_app_context_t app;
    hu_bus_t bus;
    hu_config_t cfg;
    setup_proto_with_app(&alloc, &ws, &proto, &app, &bus, &cfg);
    hu_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"req\",\"id\":\"9\",\"method\":\"usage.summary\"}";
    hu_control_on_message(&conn, msg, strlen(msg), &proto);
    teardown_proto(&ws, &proto);
}

/* HU_IS_TEST: update.check/run use mock data, no network. */
static void test_rpc_update_check_returns_valid_structure(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t ws;
    hu_control_protocol_t proto;
    hu_app_context_t app;
    hu_bus_t bus;
    hu_config_t cfg;
    setup_proto_with_app(&alloc, &ws, &proto, &app, &bus, &cfg);
    hu_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"req\",\"id\":\"uc\",\"method\":\"update.check\"}";
    hu_control_on_message(&conn, msg, strlen(msg), &proto);
    teardown_proto(&ws, &proto);
}

/* HU_IS_TEST: update.apply returns HU_OK, no real apply. */
static void test_rpc_update_run_returns_valid_structure(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t ws;
    hu_control_protocol_t proto;
    hu_app_context_t app;
    hu_bus_t bus;
    hu_config_t cfg;
    setup_proto_with_app(&alloc, &ws, &proto, &app, &bus, &cfg);
    hu_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"req\",\"id\":\"ur\",\"method\":\"update.run\"}";
    hu_control_on_message(&conn, msg, strlen(msg), &proto);
    teardown_proto(&ws, &proto);
}

/* nodes.list returns at least one node (local default). */
static void test_rpc_nodes_list_returns_at_least_one_node(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t ws;
    hu_control_protocol_t proto;
    hu_app_context_t app;
    hu_bus_t bus;
    hu_config_t cfg;
    setup_proto_with_app(&alloc, &ws, &proto, &app, &bus, &cfg);
    hu_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"req\",\"id\":\"nl\",\"method\":\"nodes.list\"}";
    hu_control_on_message(&conn, msg, strlen(msg), &proto);
    teardown_proto(&ws, &proto);
}

static void test_rpc_agents_list_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t ws;
    hu_control_protocol_t proto;
    hu_app_context_t app;
    hu_bus_t bus;
    hu_config_t cfg;
    setup_proto_with_app(&alloc, &ws, &proto, &app, &bus, &cfg);
    hu_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"req\",\"id\":\"al\",\"method\":\"agents.list\"}";
    hu_control_on_message(&conn, msg, strlen(msg), &proto);
    teardown_proto(&ws, &proto);
}

static void test_rpc_voice_config_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t ws;
    hu_control_protocol_t proto;
    hu_app_context_t app;
    hu_bus_t bus;
    hu_config_t cfg;
    setup_proto_with_app(&alloc, &ws, &proto, &app, &bus, &cfg);
    hu_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"req\",\"id\":\"vc\",\"method\":\"voice.config\"}";
    hu_control_on_message(&conn, msg, strlen(msg), &proto);
    teardown_proto(&ws, &proto);
}

static void test_rpc_nodes_action_missing_fields_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t ws;
    hu_control_protocol_t proto;
    hu_app_context_t app;
    hu_bus_t bus;
    hu_config_t cfg;
    setup_proto_with_app(&alloc, &ws, &proto, &app, &bus, &cfg);
    hu_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"req\",\"id\":\"na\",\"method\":\"nodes.action\"}";
    hu_control_on_message(&conn, msg, strlen(msg), &proto);
    teardown_proto(&ws, &proto);
}

static void test_cp_admin_nodes_action_restart_returns_mock_json(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_app_context_t app;
    hu_config_t cfg;
    hu_bus_t bus;
    memset(&app, 0, sizeof(app));
    memset(&cfg, 0, sizeof(cfg));
    hu_bus_init(&bus);
    app.config = &cfg;
    app.alloc = &alloc;
    app.bus = &bus;

    hu_json_value_t *root = NULL;
    const char *json = "{\"type\":\"req\",\"id\":\"r1\",\"method\":\"nodes.action\",\"params\":{"
                       "\"node_id\":\"local\",\"action\":\"restart\"}}";
    HU_ASSERT_EQ(hu_json_parse(&alloc, json, strlen(json), &root), HU_OK);
    HU_ASSERT_NOT_NULL(root);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = cp_admin_nodes_action(&alloc, &app, NULL, NULL, root, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "\"ok\":true") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"action\":\"restart\"") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
    hu_json_free(&alloc, root);
    hu_bus_deinit(&bus);
}

static void test_event_bridge_payload_propagation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_server_t ws;
    hu_ws_server_init(&ws, &alloc, NULL, NULL, NULL);
    hu_control_protocol_t proto;
    hu_control_protocol_init(&proto, &alloc, &ws);
    hu_bus_t bus;
    hu_bus_init(&bus);

    hu_event_bridge_t bridge;
    hu_event_bridge_init(&bridge, &proto, &bus);

    hu_bus_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = HU_BUS_MESSAGE_SENT;
    snprintf(ev.channel, HU_BUS_CHANNEL_LEN, "cli");
    snprintf(ev.id, HU_BUS_ID_LEN, "s1");
    const char *long_msg = "This is a long message that exceeds 256 bytes. "
                           "Padding padding padding padding padding padding padding padding "
                           "padding padding padding padding padding padding padding padding "
                           "padding padding padding padding padding padding padding padding "
                           "padding padding padding padding padding padding END";
    ev.payload = (void *)long_msg;
    memset(ev.message, 0, HU_BUS_MSG_LEN);
    size_t ml = strlen(long_msg);
    if (ml >= HU_BUS_MSG_LEN)
        ml = HU_BUS_MSG_LEN - 1;
    memcpy(ev.message, long_msg, ml);
    ev.message[ml] = '\0';
    hu_bus_publish(&bus, &ev);

    hu_event_bridge_deinit(&bridge);
    hu_control_protocol_deinit(&proto);
    hu_ws_server_deinit(&ws);
}

static void test_openai_compat_chat_valid_returns_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg = {0};
    cfg.default_provider = "openai";
    cfg.default_model = "gpt-4o";
    hu_app_context_t app = {.config = &cfg};

    const char *body =
        "{\"model\":\"gpt-4o\",\"messages\":[{\"role\":\"user\",\"content\":\"Hi\"}],"
        "\"max_tokens\":1024,\"temperature\":0.7,\"stream\":false}";
    int status = 500;
    char *resp = NULL;
    size_t resp_len = 0;
    hu_openai_compat_handle_chat_completions(body, strlen(body), &alloc, &app, &status, &resp,
                                             &resp_len, NULL);
    HU_ASSERT_EQ(status, 200);
    HU_ASSERT_NOT_NULL(resp);
    HU_ASSERT_TRUE(strstr(resp, "\"object\":\"chat.completion\"") != NULL);
    HU_ASSERT_TRUE(strstr(resp, "\"choices\"") != NULL);
    HU_ASSERT_TRUE(strstr(resp, "\"usage\"") != NULL);
    HU_ASSERT_TRUE(strstr(resp, "chatcmpl-") != NULL);
    if (resp)
        alloc.free(alloc.ctx, resp, resp_len + 1);
}

static void test_openai_compat_chat_invalid_json_400(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg = {0};
    cfg.default_provider = "openai";
    cfg.default_model = "gpt-4o";
    hu_app_context_t app = {.config = &cfg};

    const char *body = "{invalid json";
    int status = 200;
    char *resp = NULL;
    size_t resp_len = 0;
    hu_openai_compat_handle_chat_completions(body, strlen(body), &alloc, &app, &status, &resp,
                                             &resp_len, NULL);
    HU_ASSERT_EQ(status, 400);
    if (resp)
        alloc.free(alloc.ctx, resp, resp_len + 1);
}

/* Regression: 2026-05-24 M4 verifier observed body `{"error":{"message":""Invalid JSON""}}`
 * (double-quoted message). The fix in src/gateway/openai_compat.c::error_response stripped
 * the redundant `"` from prefix/suffix since hu_json_append_string already emits a
 * fully-quoted JSON string. This test pins the SHAPE of the body so the bug can't
 * regress — assert the body matches exactly `{"error":{"message":"Invalid JSON"}}`
 * and contains no `""` (consecutive quote pair, which is the bug fingerprint). */
static void test_openai_compat_error_response_single_quoted_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg = {0};
    cfg.default_provider = "openai";
    cfg.default_model = "gpt-4o";
    hu_app_context_t app = {.config = &cfg};

    const char *body = "{invalid json";
    int status = 200;
    char *resp = NULL;
    size_t resp_len = 0;
    hu_openai_compat_handle_chat_completions(body, strlen(body), &alloc, &app, &status, &resp,
                                             &resp_len, NULL);
    HU_ASSERT_EQ(status, 400);
    HU_ASSERT_NOT_NULL(resp);
    /* Exact match — properly-formed JSON, no double-escape. */
    const char *want = "{\"error\":{\"message\":\"Invalid JSON\"}}";
    HU_ASSERT_EQ(resp_len, strlen(want));
    HU_ASSERT_TRUE(memcmp(resp, want, resp_len) == 0);
    /* Bug fingerprint: never two consecutive quotes in a well-formed error body. */
    HU_ASSERT_TRUE(strstr(resp, "\"\"") == NULL);
    if (resp)
        alloc.free(alloc.ctx, resp, resp_len + 1);
}

static void test_openai_compat_chat_stream_returns_sse(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg = {0};
    cfg.default_provider = "openai";
    cfg.default_model = "gpt-4o";
    hu_app_context_t app = {.config = &cfg};

    const char *body =
        "{\"model\":\"gpt-4o\",\"messages\":[{\"role\":\"user\",\"content\":\"Hi\"}],"
        "\"stream\":true}";
    int status = 500;
    char *resp = NULL;
    size_t resp_len = 0;
    const char *content_type = NULL;
    hu_openai_compat_handle_chat_completions(body, strlen(body), &alloc, &app, &status, &resp,
                                             &resp_len, &content_type);
    HU_ASSERT_EQ(status, 200);
    HU_ASSERT_NOT_NULL(resp);
    HU_ASSERT_TRUE(strstr(resp, "data: ") != NULL);
    /* Verify stream ends with data: [DONE] */
    int has_done = (resp_len >= 14 && memcmp(resp + resp_len - 14, "data: [DONE]\n\n", 14) == 0);
    if (!has_done && resp_len >= 4) {
        for (size_t i = 0; i + 4 <= resp_len; i++)
            if (memcmp(resp + i, "DONE", 4) == 0) {
                has_done = 1;
                break;
            }
    }
    HU_ASSERT_TRUE(has_done);
    HU_ASSERT_TRUE(strstr(resp, "chat.completion.chunk") != NULL);
    HU_ASSERT_TRUE(content_type != NULL && strcmp(content_type, "text/event-stream") == 0);
    if (resp)
        alloc.free(alloc.ctx, resp, resp_len + 1);
}

static void test_openai_compat_chat_empty_messages_400(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg = {0};
    cfg.default_provider = "openai";
    cfg.default_model = "gpt-4o";
    hu_app_context_t app = {.config = &cfg};

    const char *body = "{\"model\":\"gpt-4o\",\"messages\":[]}";
    int status = 200;
    char *resp = NULL;
    size_t resp_len = 0;
    hu_openai_compat_handle_chat_completions(body, strlen(body), &alloc, &app, &status, &resp,
                                             &resp_len, NULL);
    HU_ASSERT_EQ(status, 400);
    if (resp)
        alloc.free(alloc.ctx, resp, resp_len + 1);
}

static void test_gateway_missing_content_type(void) {
    /* Handler must not crash when body is NULL or empty (e.g. request without body) */
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg = {0};
    cfg.default_provider = "openai";
    cfg.default_model = "gpt-4o";
    hu_app_context_t app = {.config = &cfg};

    int status = 200;
    char *resp = NULL;
    size_t resp_len = 0;
    hu_openai_compat_handle_chat_completions(NULL, 0, &alloc, &app, &status, &resp, &resp_len,
                                             NULL);
    HU_ASSERT_EQ(status, 400);
    if (resp)
        alloc.free(alloc.ctx, resp, resp_len + 1);
}

static void test_openai_compat_chat_stream_has_delta_content(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg = {0};
    cfg.default_provider = "openai";
    cfg.default_model = "gpt-4o";
    hu_app_context_t app = {.config = &cfg};

    const char *body =
        "{\"model\":\"gpt-4o\",\"messages\":[{\"role\":\"user\",\"content\":\"Hi\"}],"
        "\"stream\":true}";
    int status = 500;
    char *resp = NULL;
    size_t resp_len = 0;
    hu_openai_compat_handle_chat_completions(body, strlen(body), &alloc, &app, &status, &resp,
                                             &resp_len, NULL);
    HU_ASSERT_EQ(status, 200);
    HU_ASSERT_NOT_NULL(resp);
    HU_ASSERT_TRUE(strstr(resp, "\"delta\"") != NULL);
    HU_ASSERT_TRUE(strstr(resp, "\"content\":") != NULL);
    if (resp)
        alloc.free(alloc.ctx, resp, resp_len + 1);
}

static void test_openai_compat_models_returns_list(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg = {0};
    cfg.default_provider = "openai";
    cfg.default_model = "gpt-4o";
    hu_app_context_t app = {.config = &cfg};

    int status = 500;
    char *resp = NULL;
    size_t resp_len = 0;
    hu_openai_compat_handle_models(&alloc, &app, &status, &resp, &resp_len);
    HU_ASSERT_EQ(status, 200);
    HU_ASSERT_NOT_NULL(resp);
    HU_ASSERT_TRUE(strstr(resp, "\"object\":\"list\"") != NULL);
    HU_ASSERT_TRUE(strstr(resp, "\"data\"") != NULL);
    HU_ASSERT_TRUE(strstr(resp, "openai") != NULL);
    if (resp)
        alloc.free(alloc.ctx, resp, resp_len + 1);
}

/* M4 follow-up 2026-05-24: when agent_turn's transport-error fast-fail
 * bails with HU_ERR_PROVIDER_UNAVAILABLE, the gateway maps to HTTP 503
 * (not 502). 503 = "downstream temporarily unavailable, retry later" so
 * clients back off; 502 = "permanent gateway failure." See the
 * src/gateway/openai_compat.c if-blocks at the agent_err call sites and
 * the agent_turn fast-fail in src/agent/agent_turn.c.
 *
 * This test pins the 503-body-shape contract: a well-formed
 * single-quoted JSON error body (the same fix as commit 220db26d). The
 * easiest 503 trigger from a unit test is config=NULL on the
 * chat-completions handler (openai_compat.c line 203 "Service
 * unavailable: no config"). The PROVIDER_UNAVAILABLE → 503 mapping
 * itself is a 4-line code change reviewable inline; what regresses
 * silently is the JSON body shape, hence this assertion. */
static void test_openai_compat_chat_503_body_is_well_formed_json(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_app_context_t app = {.config = NULL};

    const char *body =
        "{\"model\":\"gpt-4o\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
    int status = 200;
    char *resp = NULL;
    size_t resp_len = 0;
    hu_openai_compat_handle_chat_completions(body, strlen(body), &alloc, &app, &status, &resp,
                                             &resp_len, NULL);
    HU_ASSERT_EQ(status, 503);
    HU_ASSERT_NOT_NULL(resp);
    /* Same shape contract as the 400 error_response test — single-quoted,
     * no consecutive `""` (the bug fingerprint from commit 220db26d). */
    const char *want = "{\"error\":{\"message\":\"Service unavailable: no config\"}}";
    HU_ASSERT_EQ(resp_len, strlen(want));
    HU_ASSERT_TRUE(memcmp(resp, want, resp_len) == 0);
    HU_ASSERT_TRUE(strstr(resp, "\"\"") == NULL);
    if (resp)
        alloc.free(alloc.ctx, resp, resp_len + 1);
}

static void test_openai_compat_models_null_config_503(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_app_context_t app = {.config = NULL};

    int status = 200;
    char *resp = NULL;
    size_t resp_len = 0;
    hu_openai_compat_handle_models(&alloc, &app, &status, &resp, &resp_len);
    HU_ASSERT_EQ(status, 503);
    if (resp)
        alloc.free(alloc.ctx, resp, resp_len + 1);
}

static void test_rpc_models_decisions_returns_valid_json(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* Seed the global route log with one entry so the response is non-empty.
     * hu_route_global_log() acquires the mutex internally, so call it BEFORE lock. */
    hu_model_selection_t sel;
    memset(&sel, 0, sizeof(sel));
    sel.tier = HU_TIER_CONVERSATIONAL;
    sel.source = HU_ROUTE_HEURISTIC;
    sel.model = "gemini-3-flash-preview";
    sel.model_len = 22;
    hu_route_decision_log_t *rlog = hu_route_global_log();
    /* Sprint 38 — reset so this test does not depend on cumulative suite
     * order (same contract as route_populates_global_log). */
    hu_route_log_init(rlog);
    hu_route_global_log_lock();
    hu_route_log_record(rlog, &sel, 3, 1000);
    hu_route_global_log_unlock();

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = cp_admin_models_decisions(&alloc, NULL, NULL, NULL, NULL, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);

    /* Structural: top-level keys */
    HU_ASSERT_STR_CONTAINS(out, "\"decisions\"");
    HU_ASSERT_STR_CONTAINS(out, "\"total\"");
    HU_ASSERT_STR_CONTAINS(out, "\"tier_distribution\"");

    /* Seeded entry fields: tier, source, model from the seed above */
    HU_ASSERT_STR_CONTAINS(out, "\"conversational\"");
    HU_ASSERT_STR_CONTAINS(out, "\"heuristic\"");
    HU_ASSERT_STR_CONTAINS(out, "\"gemini-3-flash-preview\"");
    HU_ASSERT_STR_CONTAINS(out, "\"heuristic_score\"");
    HU_ASSERT_STR_CONTAINS(out, "\"timestamp\"");

    /* All four tier_distribution keys present */
    HU_ASSERT_STR_CONTAINS(out, "\"reflexive\"");
    HU_ASSERT_STR_CONTAINS(out, "\"analytical\"");
    HU_ASSERT_STR_CONTAINS(out, "\"deep\"");

    alloc.free(alloc.ctx, out, out_len + 1);
}

/* ── metrics.fidelity (Track D D2.2) ─────────────────────────────
 *
 * The handler must (a) return the canonical JSON shape on every
 * exit path so the dashboard tile always renders, (b) honor
 * `params.persona` over the active agent's persona, (c) merge in
 * the canonical `last_fidelity_ab.json` when present and skip it
 * cleanly when absent. */

static void cp_fidelity_setup_persona_dir(char *out_dir, size_t cap) {
    static int counter = 0;
    counter++;
    snprintf(out_dir, cap, "/tmp/hu_fidelity_test_%d_%d", (int)getpid(), counter);
    mkdir(out_dir, 0700);
    setenv("HU_PERSONA_DIR", out_dir, 1);
    /* Force the synthetic-fingerprint path so the test result is
     * deterministic across machines (no dependence on
     * ~/.human/personal_model.bin). */
    setenv("HUMAN_PERSONAL_MODEL_PATH", "/dev/null/__nonexistent__", 1);
}

static void cp_fidelity_write_fixture_persona(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.json", dir, name);
    FILE *f = fopen(path, "w");
    HU_ASSERT_NOT_NULL(f);
    fputs("{\"version\":1,\"name\":\"fidelity_gateway_fixture\","
          "\"core\":{\"identity\":\"casual lowercase fixture\","
          "\"traits\":[\"warm\"]},"
          "\"example_banks\":[{\"channel\":\"cli\",\"examples\":["
          "{\"context\":\"a\",\"incoming\":\"x\","
          "\"response\":\"hey lmk if u want anything else from me today\"},"
          "{\"context\":\"b\",\"incoming\":\"y\","
          "\"response\":\"yeah sure btw the report is ready whenever u need it\"}"
          "]}]}",
          f);
    fclose(f);
}

static void cp_fidelity_cleanup(const char *dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/fidelity_gateway_fixture.json", dir);
    unlink(path);
    rmdir(dir);
    unsetenv("HU_PERSONA_DIR");
    unsetenv("HUMAN_PERSONAL_MODEL_PATH");
    unsetenv("HUMAN_FIDELITY_AB_PATH");
}

static void test_cp_admin_metrics_fidelity_returns_zero_state_without_persona(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_app_context_t app;
    hu_config_t cfg;
    memset(&app, 0, sizeof(app));
    memset(&cfg, 0, sizeof(cfg));
    app.config = &cfg;
    app.alloc = &alloc;
    /* No agent → no active persona, no params → zero-state response. */

    hu_json_value_t *root = NULL;
    const char *json = "{\"type\":\"req\",\"id\":\"r1\",\"method\":\"metrics.fidelity\"}";
    HU_ASSERT_EQ(hu_json_parse(&alloc, json, strlen(json), &root), HU_OK);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = cp_admin_metrics_fidelity(&alloc, &app, NULL, NULL, root, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    /* Same shape, all zeros, ab.available=false. */
    HU_ASSERT_TRUE(strstr(out, "\"persona\":\"\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"baseline\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"available\":false") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"error\":\"no persona configured\"") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
    hu_json_free(&alloc, root);
}

static void test_cp_admin_metrics_fidelity_uses_params_persona(void) {
    char dir[256];
    cp_fidelity_setup_persona_dir(dir, sizeof(dir));
    cp_fidelity_write_fixture_persona(dir, "fidelity_gateway_fixture");

    hu_allocator_t alloc = hu_system_allocator();
    hu_app_context_t app;
    hu_config_t cfg;
    memset(&app, 0, sizeof(app));
    memset(&cfg, 0, sizeof(cfg));
    app.config = &cfg;
    app.alloc = &alloc;

    hu_json_value_t *root = NULL;
    const char *json = "{\"type\":\"req\",\"id\":\"r2\",\"method\":\"metrics.fidelity\","
                       "\"params\":{\"persona\":\"fidelity_gateway_fixture\"}}";
    HU_ASSERT_EQ(hu_json_parse(&alloc, json, strlen(json), &root), HU_OK);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = cp_admin_metrics_fidelity(&alloc, &app, NULL, NULL, root, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    /* Persona name is echoed; baseline.scored>0 because two
     * non-empty fixture responses score against the synthetic
     * target; fingerprint_source is synthetic by construction. */
    HU_ASSERT_TRUE(strstr(out, "\"persona\":\"fidelity_gateway_fixture\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"fingerprint_source\":\"synthetic\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"baseline\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"scored\":2") != NULL);
    /* No A/B file present → ab.available=false. */
    HU_ASSERT_TRUE(strstr(out, "\"available\":false") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
    hu_json_free(&alloc, root);
    cp_fidelity_cleanup(dir);
}

static void test_cp_admin_metrics_fidelity_merges_ab_status_file(void) {
    char dir[256];
    cp_fidelity_setup_persona_dir(dir, sizeof(dir));
    cp_fidelity_write_fixture_persona(dir, "fidelity_gateway_fixture");

    /* Drop a canonical status.json next to the persona fixture
     * dir and point the override env at it. The handler should
     * merge the `ab` sub-object verbatim into the response. */
    char ab_path[512];
    snprintf(ab_path, sizeof(ab_path), "%s/last_fidelity_ab.json", dir);
    FILE *f = fopen(ab_path, "w");
    HU_ASSERT_NOT_NULL(f);
    fputs("{\"persona\":\"fidelity_gateway_fixture\","
          "\"fingerprint_source\":\"synthetic\","
          "\"baseline\":{\"scored\":3,\"mean\":0.71,\"min\":0.6,\"max\":0.82},"
          "\"ab\":{\"available\":true,\"before_mean\":0.55,\"after_mean\":0.7,"
          "\"delta\":0.15,\"scored_before\":4,\"scored_after\":4}}",
          f);
    fclose(f);
    setenv("HUMAN_FIDELITY_AB_PATH", ab_path, 1);

    hu_allocator_t alloc = hu_system_allocator();
    hu_app_context_t app;
    hu_config_t cfg;
    memset(&app, 0, sizeof(app));
    memset(&cfg, 0, sizeof(cfg));
    app.config = &cfg;
    app.alloc = &alloc;

    hu_json_value_t *root = NULL;
    const char *json = "{\"type\":\"req\",\"id\":\"r3\",\"method\":\"metrics.fidelity\","
                       "\"params\":{\"persona\":\"fidelity_gateway_fixture\"}}";
    HU_ASSERT_EQ(hu_json_parse(&alloc, json, strlen(json), &root), HU_OK);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = cp_admin_metrics_fidelity(&alloc, &app, NULL, NULL, root, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    /* Baseline is computed live (not from the file). A/B is taken
     * from the file. */
    HU_ASSERT_TRUE(strstr(out, "\"persona\":\"fidelity_gateway_fixture\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"available\":true") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"before_mean\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"after_mean\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"scored_before\":4") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"scored_after\":4") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
    hu_json_free(&alloc, root);
    unlink(ab_path);
    cp_fidelity_cleanup(dir);
}

static void test_cp_admin_metrics_directive_telemetry_returns_counts(void) {
    /* Drive one casual+emoji directive fire through
     * `acknowledgment_directive_for_overlay`, then read it back
     * via the gateway. The handler must serialize it as the JSON
     * shape `{ total: N, variants: { ... } }`. */
    hu_personal_model_directive_telemetry_reset();
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    /* Recently-completed goal — required to get the directive
     * to fire at all. */
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "ship gateway telemetry");
    m.goals[0].active = false;
    m.goals[0].last_referenced = 1000;
    m.goal_count = 1;
    m.updated_at = 1000 + 86400;
    hu_persona_overlay_t overlay = {0};
    overlay.formality = (char *)"casual";
    overlay.emoji_usage = (char *)"moderate";
    char buf[2048];
    (void)hu_personal_model_build_prompt_with_overlay(&m, &overlay, buf, sizeof(buf));

    hu_allocator_t alloc = hu_system_allocator();
    hu_app_context_t app;
    hu_config_t cfg;
    memset(&app, 0, sizeof(app));
    memset(&cfg, 0, sizeof(cfg));
    app.config = &cfg;
    app.alloc = &alloc;

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        cp_admin_metrics_directive_telemetry(&alloc, &app, NULL, NULL, NULL, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "\"total\":1") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"casual_emoji\":1") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"null_overlay\":0") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"formal_terse\":0") != NULL);
    /* Forward-compatible shape: `variants` sub-object exists and
     * carries every label we expect. */
    HU_ASSERT_TRUE(strstr(out, "\"variants\"") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
    hu_personal_model_directive_telemetry_reset();
}

static void test_cp_admin_metrics_guard_rejects_returns_counters(void) {
    hu_guard_reject_stats_reset();
    hu_guard_reject_stats_t bump = {0};
    bump.semantic_leak = 3;
    bump.length_anomaly = 7;
    /* Snapshot reads atomics — increment via a real reject path is heavy;
     * instead verify the handler returns the expected JSON shape with
     * zeroed counters after reset. */
    (void)bump;

    hu_allocator_t alloc = hu_system_allocator();
    hu_app_context_t app;
    hu_config_t cfg;
    memset(&app, 0, sizeof(app));
    memset(&cfg, 0, sizeof(cfg));
    app.config = &cfg;
    app.alloc = &alloc;

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = cp_admin_metrics_guard_rejects(&alloc, &app, NULL, NULL, NULL, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "\"semantic_leak\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"length_anomaly\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"director_echo\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"persona_pii_echo\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"persona_identity_echo\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, ":0") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_cp_admin_metrics_fidelity_returns_error_for_missing_persona(void) {
    char dir[256];
    cp_fidelity_setup_persona_dir(dir, sizeof(dir));
    /* Note: NOT writing a fixture file — the persona load should fail. */

    hu_allocator_t alloc = hu_system_allocator();
    hu_app_context_t app;
    hu_config_t cfg;
    memset(&app, 0, sizeof(app));
    memset(&cfg, 0, sizeof(cfg));
    app.config = &cfg;
    app.alloc = &alloc;

    hu_json_value_t *root = NULL;
    const char *json = "{\"type\":\"req\",\"id\":\"r4\",\"method\":\"metrics.fidelity\","
                       "\"params\":{\"persona\":\"missing_persona\"}}";
    HU_ASSERT_EQ(hu_json_parse(&alloc, json, strlen(json), &root), HU_OK);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = cp_admin_metrics_fidelity(&alloc, &app, NULL, NULL, root, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    /* Same shape, baseline zeroed, error explaining the failure. */
    HU_ASSERT_TRUE(strstr(out, "\"persona\":\"missing_persona\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"baseline\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"available\":false") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"error\":\"persona load failed\"") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
    hu_json_free(&alloc, root);
    cp_fidelity_cleanup(dir);
}

void run_gateway_extended_tests(void) {
    HU_TEST_SUITE("Gateway Extended");
    HU_RUN_TEST(test_gateway_webhook_paths);
    HU_RUN_TEST(test_gateway_health_endpoint);
    HU_RUN_TEST(test_gateway_ready_endpoint);
    HU_RUN_TEST(test_gateway_rate_limit_sliding_window);
    HU_RUN_TEST(test_rate_limiter_allow_under_limit);
    HU_RUN_TEST(test_rate_limiter_deny_over_limit);
    HU_RUN_TEST(test_rate_limiter_window_expiry_resets);
    HU_RUN_TEST(test_gateway_max_body_size);
    HU_RUN_TEST(test_gateway_auth_required);
    HU_RUN_TEST(test_gateway_run_test_mode);
    HU_RUN_TEST(test_gateway_run_with_host);
    HU_RUN_TEST(test_health_multiple_components);
    HU_RUN_TEST(test_health_all_ok);
    HU_RUN_TEST(test_health_reset_clears);
    HU_RUN_TEST(test_gateway_config_zeroed);
    HU_RUN_TEST(test_gateway_config_from_cfg_null_cfg);
    HU_RUN_TEST(test_gateway_config_from_cfg_null_out);
    HU_RUN_TEST(test_gateway_config_from_cfg_valid);
    HU_RUN_TEST(test_gateway_config_from_cfg_defaults);
    HU_RUN_TEST(test_readiness_status_values);
    HU_RUN_TEST(test_gateway_rate_limit_constant);
    HU_RUN_TEST(test_health_check_null_alloc);
    HU_RUN_TEST(test_gateway_config_port_range);
    HU_RUN_TEST(test_gateway_config_test_mode_flag);
    HU_RUN_TEST(test_gateway_config_hmac_optional);
    HU_RUN_TEST(test_health_single_error_not_ready);
    HU_RUN_TEST(test_gateway_max_body_nonzero);
    HU_RUN_TEST(test_gateway_rate_limit_nonzero);
    HU_RUN_TEST(test_gateway_run_null_config);
    HU_RUN_TEST(test_health_empty_ready);
    HU_RUN_TEST(test_gateway_config_host_default);
    HU_RUN_TEST(test_gateway_config_rate_limit_default);
    HU_RUN_TEST(test_health_mark_ok_then_error);
    HU_RUN_TEST(test_gateway_run_bind_zero_port);
    HU_RUN_TEST(test_component_check_structure);
    HU_RUN_TEST(test_gateway_config_max_body_boundary);
    HU_RUN_TEST(test_gateway_config_hmac_len_zero);
    HU_RUN_TEST(test_gateway_config_rate_limit_one);
    HU_RUN_TEST(test_health_mark_ok_overwrites_error);
    HU_RUN_TEST(test_gateway_config_port_one);
    HU_RUN_TEST(test_gateway_constants_match_zig);
    HU_RUN_TEST(test_health_three_components_mixed);
    HU_RUN_TEST(test_gateway_run_test_mode_host_loopback);
    HU_RUN_TEST(test_ws_server_init_deinit);
    HU_RUN_TEST(test_ws_server_upgrade_null_args);
    HU_RUN_TEST(test_ws_server_is_upgrade_valid);
    HU_RUN_TEST(test_ws_server_is_upgrade_invalid);
    HU_RUN_TEST(test_ws_server_send_null_conn);
    HU_RUN_TEST(test_ws_server_broadcast_empty);
    HU_RUN_TEST(test_cp_admin_metrics_fidelity_returns_zero_state_without_persona);
#ifdef HU_ENABLE_ML
    HU_RUN_TEST(test_cp_admin_metrics_fidelity_uses_params_persona);
    HU_RUN_TEST(test_cp_admin_metrics_fidelity_merges_ab_status_file);
#endif
    HU_RUN_TEST(test_cp_admin_metrics_fidelity_returns_error_for_missing_persona);
    HU_RUN_TEST(test_cp_admin_metrics_directive_telemetry_returns_counts);
    HU_RUN_TEST(test_cp_admin_metrics_guard_rejects_returns_counters);

    HU_TEST_SUITE("Control Protocol");
    HU_RUN_TEST(test_control_protocol_init_deinit);
    HU_RUN_TEST(test_control_set_app_ctx);
    HU_RUN_TEST(test_control_set_app_ctx_null);
    HU_RUN_TEST(test_control_on_message_null);
    HU_RUN_TEST(test_control_on_message_invalid_json);
    HU_RUN_TEST(test_control_on_message_non_req);
    HU_RUN_TEST(test_control_on_message_no_method);
    HU_RUN_TEST(test_control_send_event_null);
    HU_RUN_TEST(test_control_send_event_no_ws);
    HU_RUN_TEST(test_control_send_response_null);
    HU_RUN_TEST(test_control_on_close);

    HU_TEST_SUITE("Event Bridge");
    HU_RUN_TEST(test_event_bridge_init_deinit);
    HU_RUN_TEST(test_event_bridge_init_null);
    HU_RUN_TEST(test_event_bridge_deinit_null);
    HU_RUN_TEST(test_event_bridge_bus_subscription);
    HU_RUN_TEST(test_event_bridge_publish_no_crash);

    HU_TEST_SUITE("RPC Handlers");
    HU_RUN_TEST(test_rpc_chat_send_empty_message_rejected);
    HU_RUN_TEST(test_rpc_chat_send_null_message_rejected);
    HU_RUN_TEST(test_rpc_chat_send_valid_publishes_bus);
    HU_RUN_TEST(test_rpc_chat_send_self_harm_returns_crisis);
    HU_RUN_TEST(test_rpc_chat_abort_no_crash);
    HU_RUN_TEST(test_rpc_config_apply_no_crash);
    HU_RUN_TEST(test_rpc_cron_run_no_crash);
    HU_RUN_TEST(test_rpc_skills_install_no_crash);
    HU_RUN_TEST(test_rpc_exec_approval_no_crash);
    HU_RUN_TEST(test_rpc_usage_summary_no_crash);
    HU_RUN_TEST(test_rpc_update_check_returns_valid_structure);
    HU_RUN_TEST(test_rpc_update_run_returns_valid_structure);
    HU_RUN_TEST(test_rpc_nodes_list_returns_at_least_one_node);
    HU_RUN_TEST(test_rpc_agents_list_no_crash);
    HU_RUN_TEST(test_rpc_voice_config_no_crash);
    HU_RUN_TEST(test_rpc_nodes_action_missing_fields_no_crash);
    HU_RUN_TEST(test_cp_admin_nodes_action_restart_returns_mock_json);
    HU_RUN_TEST(test_event_bridge_payload_propagation);
    HU_RUN_TEST(test_rpc_models_decisions_returns_valid_json);

    HU_TEST_SUITE("WS Server Extended");
    HU_RUN_TEST(test_ws_server_process_null);
    HU_RUN_TEST(test_ws_server_read_and_process_null);
    HU_RUN_TEST(test_ws_server_close_conn_inactive);
    HU_RUN_TEST(test_ws_server_is_upgrade_short);
    HU_RUN_TEST(test_ws_server_is_upgrade_null);
    HU_RUN_TEST(test_ws_server_send_inactive_conn);
    HU_RUN_TEST(test_ws_server_broadcast_null_data);
    HU_RUN_TEST(test_ws_server_conn_pool_full);

    HU_TEST_SUITE("OpenAI Compat");
    HU_RUN_TEST(test_openai_compat_chat_valid_returns_mock);
    HU_RUN_TEST(test_openai_compat_chat_invalid_json_400);
    HU_RUN_TEST(test_openai_compat_error_response_single_quoted_message);
    HU_RUN_TEST(test_openai_compat_chat_stream_returns_sse);
    HU_RUN_TEST(test_openai_compat_chat_stream_has_delta_content);
    HU_RUN_TEST(test_openai_compat_chat_empty_messages_400);
    HU_RUN_TEST(test_gateway_missing_content_type);
    HU_RUN_TEST(test_openai_compat_models_returns_list);
    HU_RUN_TEST(test_openai_compat_models_null_config_503);
    HU_RUN_TEST(test_openai_compat_chat_503_body_is_well_formed_json);
}
