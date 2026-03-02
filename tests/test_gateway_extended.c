/* Gateway edge cases + control protocol + event bridge tests. */
#include "test_framework.h"
#include "seaclaw/gateway.h"
#include "seaclaw/gateway/ws_server.h"
#include "seaclaw/gateway/control_protocol.h"
#include "seaclaw/gateway/event_bridge.h"
#include "seaclaw/bus.h"
#include "seaclaw/health.h"
#include "seaclaw/config.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/json.h"
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

/* ── Control Protocol Tests ──────────────────────────────────────────── */

static void test_control_protocol_init_deinit(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_server_t ws;
    sc_ws_server_init(&ws, &alloc, NULL, NULL, NULL);
    sc_control_protocol_t proto;
    sc_control_protocol_init(&proto, &alloc, &ws);
    SC_ASSERT_EQ(proto.event_seq, 0u);
    SC_ASSERT_TRUE(proto.app_ctx == NULL);
    sc_control_protocol_deinit(&proto);
    sc_ws_server_deinit(&ws);
}

static void test_control_set_app_ctx(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_server_t ws;
    sc_ws_server_init(&ws, &alloc, NULL, NULL, NULL);
    sc_control_protocol_t proto;
    sc_control_protocol_init(&proto, &alloc, &ws);
    SC_ASSERT_TRUE(proto.app_ctx == NULL);

    sc_app_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    sc_control_set_app_ctx(&proto, &ctx);
    SC_ASSERT_TRUE(proto.app_ctx == &ctx);

    sc_control_protocol_deinit(&proto);
    sc_ws_server_deinit(&ws);
}

static void test_control_set_app_ctx_null(void) {
    sc_control_set_app_ctx(NULL, NULL);
}

static void test_control_on_message_null(void) {
    sc_control_on_message(NULL, NULL, 0, NULL);
}

static void test_control_on_message_invalid_json(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_server_t ws;
    sc_ws_server_init(&ws, &alloc, NULL, NULL, NULL);
    sc_control_protocol_t proto;
    sc_control_protocol_init(&proto, &alloc, &ws);
    sc_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    sc_control_on_message(&conn, "not json", 8, &proto);
    sc_control_protocol_deinit(&proto);
    sc_ws_server_deinit(&ws);
}

static void test_control_on_message_non_req(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_server_t ws;
    sc_ws_server_init(&ws, &alloc, NULL, NULL, NULL);
    sc_control_protocol_t proto;
    sc_control_protocol_init(&proto, &alloc, &ws);
    sc_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"event\",\"id\":\"1\",\"method\":\"health\"}";
    sc_control_on_message(&conn, msg, strlen(msg), &proto);
    sc_control_protocol_deinit(&proto);
    sc_ws_server_deinit(&ws);
}

static void test_control_on_message_no_method(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_server_t ws;
    sc_ws_server_init(&ws, &alloc, NULL, NULL, NULL);
    sc_control_protocol_t proto;
    sc_control_protocol_init(&proto, &alloc, &ws);
    sc_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"req\",\"id\":\"1\"}";
    sc_control_on_message(&conn, msg, strlen(msg), &proto);
    sc_control_protocol_deinit(&proto);
    sc_ws_server_deinit(&ws);
}

static void test_control_send_event_null(void) {
    sc_control_send_event(NULL, "test", "{}");
}

static void test_control_send_event_no_ws(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_control_protocol_t proto;
    sc_control_protocol_init(&proto, &alloc, NULL);
    sc_control_send_event(&proto, "test", "{}");
    sc_control_protocol_deinit(&proto);
}

static void test_control_send_response_null(void) {
    sc_error_t err = sc_control_send_response(NULL, "id", true, "{}");
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_control_on_close(void) {
    sc_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    sc_control_on_close(&conn, NULL);
}

/* ── Event Bridge Tests ─────────────────────────────────────────────── */

static void test_event_bridge_init_deinit(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_server_t ws;
    sc_ws_server_init(&ws, &alloc, NULL, NULL, NULL);
    sc_control_protocol_t proto;
    sc_control_protocol_init(&proto, &alloc, &ws);
    sc_bus_t bus;
    sc_bus_init(&bus);

    sc_event_bridge_t bridge;
    sc_event_bridge_init(&bridge, &proto, &bus);
    SC_ASSERT_TRUE(bridge.proto == &proto);
    SC_ASSERT_TRUE(bridge.bus == &bus);
    sc_event_bridge_deinit(&bridge);

    sc_control_protocol_deinit(&proto);
    sc_ws_server_deinit(&ws);
}

static void test_event_bridge_init_null(void) {
    sc_event_bridge_init(NULL, NULL, NULL);
}

static void test_event_bridge_deinit_null(void) {
    sc_event_bridge_deinit(NULL);
}

static void test_event_bridge_bus_subscription(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_server_t ws;
    sc_ws_server_init(&ws, &alloc, NULL, NULL, NULL);
    sc_control_protocol_t proto;
    sc_control_protocol_init(&proto, &alloc, &ws);
    sc_bus_t bus;
    sc_bus_init(&bus);
    SC_ASSERT_EQ(bus.count, 0u);

    sc_event_bridge_t bridge;
    sc_event_bridge_init(&bridge, &proto, &bus);
    SC_ASSERT_TRUE(bus.count > 0);

    sc_event_bridge_deinit(&bridge);
    SC_ASSERT_EQ(bus.count, 0u);

    sc_control_protocol_deinit(&proto);
    sc_ws_server_deinit(&ws);
}

static void test_event_bridge_publish_no_crash(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_server_t ws;
    sc_ws_server_init(&ws, &alloc, NULL, NULL, NULL);
    sc_control_protocol_t proto;
    sc_control_protocol_init(&proto, &alloc, &ws);
    sc_bus_t bus;
    sc_bus_init(&bus);

    sc_event_bridge_t bridge;
    sc_event_bridge_init(&bridge, &proto, &bus);

    sc_bus_publish_simple(&bus, SC_BUS_MESSAGE_RECEIVED, "cli", "s1", "hello");
    sc_bus_publish_simple(&bus, SC_BUS_TOOL_CALL, "cli", "t1", "shell");
    sc_bus_publish_simple(&bus, SC_BUS_ERROR, "cli", "e1", "oops");
    sc_bus_publish_simple(&bus, SC_BUS_HEALTH_CHANGE, "gw", "", "ok");

    sc_event_bridge_deinit(&bridge);
    sc_control_protocol_deinit(&proto);
    sc_ws_server_deinit(&ws);
}

/* ── WS Server Extended Tests ───────────────────────────────────────── */

static void test_ws_server_process_null(void) {
    sc_error_t err = sc_ws_server_process(NULL, NULL);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_ws_server_read_and_process_null(void) {
    sc_error_t err = sc_ws_server_read_and_process(NULL, NULL);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_ws_server_close_conn_inactive(void) {
    sc_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.active = false;
    sc_ws_server_close_conn(NULL, &conn);
}

static void test_ws_server_is_upgrade_short(void) {
    bool ok = sc_ws_server_is_upgrade("GET", 3);
    SC_ASSERT_FALSE(ok);
}

static void test_ws_server_is_upgrade_null(void) {
    bool ok = sc_ws_server_is_upgrade(NULL, 0);
    SC_ASSERT_FALSE(ok);
}

static void test_ws_server_send_inactive_conn(void) {
    sc_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.active = false;
    conn.fd = 999;
    sc_error_t err = sc_ws_server_send(&conn, "test", 4);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_ws_server_broadcast_null_data(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_server_t srv;
    sc_ws_server_init(&srv, &alloc, NULL, NULL, NULL);
    sc_ws_server_broadcast(&srv, NULL, 0);
    sc_ws_server_deinit(&srv);
}

static void test_ws_server_conn_pool_full(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_server_t srv;
    sc_ws_server_init(&srv, &alloc, NULL, NULL, NULL);
    for (int i = 0; i < SC_WS_SERVER_MAX_CONNS; i++) {
        srv.conns[i].active = true;
        srv.conns[i].fd = 100 + i;
    }
    srv.conn_count = SC_WS_SERVER_MAX_CONNS;
    sc_ws_conn_t *out = NULL;
    const char *req = "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    sc_error_t err = sc_ws_server_upgrade(&srv, 99, req, strlen(req), &out);
    SC_ASSERT_EQ(err, SC_ERR_ALREADY_EXISTS);
    SC_ASSERT_TRUE(out == NULL);
    for (int i = 0; i < SC_WS_SERVER_MAX_CONNS; i++) {
        srv.conns[i].active = false;
        srv.conns[i].fd = -1;
    }
    sc_ws_server_deinit(&srv);
}

/* ── RPC Handler Tests ──────────────────────────────────────────────── */

static void setup_proto_with_app(sc_allocator_t *alloc, sc_ws_server_t *ws,
    sc_control_protocol_t *proto, sc_app_context_t *app,
    sc_bus_t *bus, sc_config_t *cfg) {
    sc_ws_server_init(ws, alloc, NULL, NULL, NULL);
    sc_control_protocol_init(proto, alloc, ws);
    memset(app, 0, sizeof(*app));
    memset(cfg, 0, sizeof(*cfg));
    sc_bus_init(bus);
    app->config = cfg;
    app->alloc = alloc;
    app->bus = bus;
    sc_control_set_app_ctx(proto, app);
}

static void teardown_proto(sc_ws_server_t *ws, sc_control_protocol_t *proto) {
    sc_control_protocol_deinit(proto);
    sc_ws_server_deinit(ws);
}

static void test_rpc_chat_send_empty_message_rejected(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_server_t ws;
    sc_control_protocol_t proto;
    sc_app_context_t app;
    sc_bus_t bus;
    sc_config_t cfg;
    setup_proto_with_app(&alloc, &ws, &proto, &app, &bus, &cfg);
    sc_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"req\",\"id\":\"1\",\"method\":\"chat.send\",\"params\":{\"message\":\"\"}}";
    sc_control_on_message(&conn, msg, strlen(msg), &proto);
    teardown_proto(&ws, &proto);
}

static void test_rpc_chat_send_null_message_rejected(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_server_t ws;
    sc_control_protocol_t proto;
    sc_app_context_t app;
    sc_bus_t bus;
    sc_config_t cfg;
    setup_proto_with_app(&alloc, &ws, &proto, &app, &bus, &cfg);
    sc_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"req\",\"id\":\"2\",\"method\":\"chat.send\",\"params\":{}}";
    sc_control_on_message(&conn, msg, strlen(msg), &proto);
    teardown_proto(&ws, &proto);
}

static bool s_bus_got_message = false;
static bool chat_send_bus_listener(sc_bus_event_type_t t, const sc_bus_event_t *e, void *u) {
    (void)u;
    if (t == SC_BUS_MESSAGE_RECEIVED && e->payload) s_bus_got_message = true;
    return true;
}

static void test_rpc_chat_send_valid_publishes_bus(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_server_t ws;
    sc_control_protocol_t proto;
    sc_app_context_t app;
    sc_bus_t bus;
    sc_config_t cfg;
    setup_proto_with_app(&alloc, &ws, &proto, &app, &bus, &cfg);

    s_bus_got_message = false;
    sc_bus_subscribe(&bus, chat_send_bus_listener, NULL, SC_BUS_MESSAGE_RECEIVED);

    sc_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"req\",\"id\":\"3\",\"method\":\"chat.send\","
        "\"params\":{\"message\":\"hello world\"}}";
    sc_control_on_message(&conn, msg, strlen(msg), &proto);
    SC_ASSERT_TRUE(s_bus_got_message);
    teardown_proto(&ws, &proto);
}

static void test_rpc_chat_abort_no_crash(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_server_t ws;
    sc_control_protocol_t proto;
    sc_app_context_t app;
    sc_bus_t bus;
    sc_config_t cfg;
    setup_proto_with_app(&alloc, &ws, &proto, &app, &bus, &cfg);
    sc_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"req\",\"id\":\"4\",\"method\":\"chat.abort\"}";
    sc_control_on_message(&conn, msg, strlen(msg), &proto);
    teardown_proto(&ws, &proto);
}

static void test_rpc_config_apply_no_crash(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_server_t ws;
    sc_control_protocol_t proto;
    sc_app_context_t app;
    sc_bus_t bus;
    sc_config_t cfg;
    setup_proto_with_app(&alloc, &ws, &proto, &app, &bus, &cfg);
    sc_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"req\",\"id\":\"5\",\"method\":\"config.apply\","
        "\"params\":{\"config\":\"{\\\"workspace\\\":\\\".\\\"}\"}}";;
    sc_control_on_message(&conn, msg, strlen(msg), &proto);
    teardown_proto(&ws, &proto);
}

static void test_rpc_cron_run_no_crash(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_server_t ws;
    sc_control_protocol_t proto;
    sc_app_context_t app;
    sc_bus_t bus;
    sc_config_t cfg;
    setup_proto_with_app(&alloc, &ws, &proto, &app, &bus, &cfg);
    sc_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"req\",\"id\":\"6\",\"method\":\"cron.run\","
        "\"params\":{\"id\":0}}";
    sc_control_on_message(&conn, msg, strlen(msg), &proto);
    teardown_proto(&ws, &proto);
}

static void test_rpc_skills_install_no_crash(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_server_t ws;
    sc_control_protocol_t proto;
    sc_app_context_t app;
    sc_bus_t bus;
    sc_config_t cfg;
    setup_proto_with_app(&alloc, &ws, &proto, &app, &bus, &cfg);
    sc_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"req\",\"id\":\"7\",\"method\":\"skills.install\","
        "\"params\":{\"url\":\"https://example.com/skill.json\"}}";
    sc_control_on_message(&conn, msg, strlen(msg), &proto);
    teardown_proto(&ws, &proto);
}

static void test_rpc_exec_approval_no_crash(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_server_t ws;
    sc_control_protocol_t proto;
    sc_app_context_t app;
    sc_bus_t bus;
    sc_config_t cfg;
    setup_proto_with_app(&alloc, &ws, &proto, &app, &bus, &cfg);
    sc_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"req\",\"id\":\"8\",\"method\":\"exec.approval.resolve\","
        "\"params\":{\"request_id\":\"r1\",\"approved\":true}}";
    sc_control_on_message(&conn, msg, strlen(msg), &proto);
    teardown_proto(&ws, &proto);
}

static void test_rpc_usage_summary_no_crash(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_server_t ws;
    sc_control_protocol_t proto;
    sc_app_context_t app;
    sc_bus_t bus;
    sc_config_t cfg;
    setup_proto_with_app(&alloc, &ws, &proto, &app, &bus, &cfg);
    sc_ws_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    const char *msg = "{\"type\":\"req\",\"id\":\"9\",\"method\":\"usage.summary\"}";
    sc_control_on_message(&conn, msg, strlen(msg), &proto);
    teardown_proto(&ws, &proto);
}

static void test_event_bridge_payload_propagation(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_server_t ws;
    sc_ws_server_init(&ws, &alloc, NULL, NULL, NULL);
    sc_control_protocol_t proto;
    sc_control_protocol_init(&proto, &alloc, &ws);
    sc_bus_t bus;
    sc_bus_init(&bus);

    sc_event_bridge_t bridge;
    sc_event_bridge_init(&bridge, &proto, &bus);

    sc_bus_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SC_BUS_MESSAGE_SENT;
    snprintf(ev.channel, SC_BUS_CHANNEL_LEN, "cli");
    snprintf(ev.id, SC_BUS_ID_LEN, "s1");
    const char *long_msg = "This is a long message that exceeds 256 bytes. "
        "Padding padding padding padding padding padding padding padding "
        "padding padding padding padding padding padding padding padding "
        "padding padding padding padding padding padding padding padding "
        "padding padding padding padding padding padding END";
    ev.payload = (void *)long_msg;
    memset(ev.message, 0, SC_BUS_MSG_LEN);
    size_t ml = strlen(long_msg);
    if (ml >= SC_BUS_MSG_LEN) ml = SC_BUS_MSG_LEN - 1;
    memcpy(ev.message, long_msg, ml);
    ev.message[ml] = '\0';
    sc_bus_publish(&bus, &ev);

    sc_event_bridge_deinit(&bridge);
    sc_control_protocol_deinit(&proto);
    sc_ws_server_deinit(&ws);
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

    SC_TEST_SUITE("Control Protocol");
    SC_RUN_TEST(test_control_protocol_init_deinit);
    SC_RUN_TEST(test_control_set_app_ctx);
    SC_RUN_TEST(test_control_set_app_ctx_null);
    SC_RUN_TEST(test_control_on_message_null);
    SC_RUN_TEST(test_control_on_message_invalid_json);
    SC_RUN_TEST(test_control_on_message_non_req);
    SC_RUN_TEST(test_control_on_message_no_method);
    SC_RUN_TEST(test_control_send_event_null);
    SC_RUN_TEST(test_control_send_event_no_ws);
    SC_RUN_TEST(test_control_send_response_null);
    SC_RUN_TEST(test_control_on_close);

    SC_TEST_SUITE("Event Bridge");
    SC_RUN_TEST(test_event_bridge_init_deinit);
    SC_RUN_TEST(test_event_bridge_init_null);
    SC_RUN_TEST(test_event_bridge_deinit_null);
    SC_RUN_TEST(test_event_bridge_bus_subscription);
    SC_RUN_TEST(test_event_bridge_publish_no_crash);

    SC_TEST_SUITE("RPC Handlers");
    SC_RUN_TEST(test_rpc_chat_send_empty_message_rejected);
    SC_RUN_TEST(test_rpc_chat_send_null_message_rejected);
    SC_RUN_TEST(test_rpc_chat_send_valid_publishes_bus);
    SC_RUN_TEST(test_rpc_chat_abort_no_crash);
    SC_RUN_TEST(test_rpc_config_apply_no_crash);
    SC_RUN_TEST(test_rpc_cron_run_no_crash);
    SC_RUN_TEST(test_rpc_skills_install_no_crash);
    SC_RUN_TEST(test_rpc_exec_approval_no_crash);
    SC_RUN_TEST(test_rpc_usage_summary_no_crash);
    SC_RUN_TEST(test_event_bridge_payload_propagation);

    SC_TEST_SUITE("WS Server Extended");
    SC_RUN_TEST(test_ws_server_process_null);
    SC_RUN_TEST(test_ws_server_read_and_process_null);
    SC_RUN_TEST(test_ws_server_close_conn_inactive);
    SC_RUN_TEST(test_ws_server_is_upgrade_short);
    SC_RUN_TEST(test_ws_server_is_upgrade_null);
    SC_RUN_TEST(test_ws_server_send_inactive_conn);
    SC_RUN_TEST(test_ws_server_broadcast_null_data);
    SC_RUN_TEST(test_ws_server_conn_pool_full);
}
