---
title: Gateway API
description: HTTP server, webhooks, Control UI, WebSocket protocol, and event bridge
updated: 2026-03-02
---

# Gateway API

The gateway provides an HTTP server for webhooks, a Control UI (SPA), WebSocket control protocol, push notifications, and event bridge.

## Gateway Server (`gateway.h`)

```c
#define SC_GATEWAY_MAX_BODY_SIZE 65536
#define SC_GATEWAY_RATE_LIMIT_PER_MIN 60

typedef struct sc_gateway_config {
    const char *host;
    uint16_t port;
    size_t max_body_size;
    uint32_t rate_limit_per_minute;
    const char *hmac_secret;
    size_t hmac_secret_len;
    bool test_mode;
    void (*on_webhook)(const char *channel, const char *body, size_t body_len, void *ctx);
    void *on_webhook_ctx;
    const char *control_ui_dir;
    const char *const *cors_origins;
    size_t cors_origins_len;
    const char *auth_token;
    sc_control_protocol_t *control;
    struct sc_app_context *app_ctx;
} sc_gateway_config_t;

sc_error_t sc_gateway_run(sc_allocator_t *alloc,
    const char *host, uint16_t port,
    const sc_gateway_config_t *config);
```

Runs HTTP + WebSocket + static files. Blocks until stopped. POSIX only (socket/poll).

## Control Protocol (`gateway/control_protocol.h`)

```c
typedef struct sc_app_context {
    struct sc_config *config;
    sc_allocator_t *alloc;
    struct sc_session_manager *sessions;
    struct sc_cron_scheduler *cron;
    struct sc_skillforge *skills;
    struct sc_cost_tracker *costs;
    struct sc_bus *bus;
    struct sc_tool *tools;
    size_t tools_count;
    struct sc_push_manager *push;
} sc_app_context_t;

typedef struct sc_control_protocol {
    sc_allocator_t *alloc;
    sc_ws_server_t *ws;
    uint64_t event_seq;
    sc_app_context_t *app_ctx;
} sc_control_protocol_t;

void sc_control_protocol_init(sc_control_protocol_t *proto,
    sc_allocator_t *alloc, sc_ws_server_t *ws);
void sc_control_protocol_deinit(sc_control_protocol_t *proto);
void sc_control_set_app_ctx(sc_control_protocol_t *proto, sc_app_context_t *ctx);

void sc_control_on_message(sc_ws_conn_t *conn, const char *data, size_t data_len, void *ctx);
void sc_control_on_close(sc_ws_conn_t *conn, void *ctx);

void sc_control_send_event(sc_control_protocol_t *proto, const char *event_name,
    const char *payload_json);
sc_error_t sc_control_send_response(sc_ws_conn_t *conn, const char *id,
    bool ok, const char *payload_json);
```

## Push (`gateway/push.h`)

```c
typedef enum sc_push_provider {
    SC_PUSH_NONE = 0,
    SC_PUSH_FCM,
    SC_PUSH_APNS,
} sc_push_provider_t;

typedef struct sc_push_manager {
    sc_allocator_t *alloc;
    sc_push_config_t config;
    sc_push_token_t *tokens;
    size_t token_count;
    size_t token_cap;
} sc_push_manager_t;

sc_error_t sc_push_init(sc_push_manager_t *mgr, sc_allocator_t *alloc,
    const sc_push_config_t *config);
void sc_push_deinit(sc_push_manager_t *mgr);

sc_error_t sc_push_register_token(sc_push_manager_t *mgr,
    const char *device_token, sc_push_provider_t provider);
sc_error_t sc_push_unregister_token(sc_push_manager_t *mgr,
    const char *device_token);

sc_error_t sc_push_send(sc_push_manager_t *mgr,
    const char *title, const char *body,
    const char *data_json);
sc_error_t sc_push_send_to(sc_push_manager_t *mgr,
    const char *device_token,
    const char *title, const char *body,
    const char *data_json);
```

## Event Bridge (`gateway/event_bridge.h`)

Bridges bus events to control protocol and push notifications.

```c
typedef struct sc_event_bridge {
    sc_control_protocol_t *proto;
    sc_bus_t *bus;
    sc_bus_subscriber_fn bus_cb;
    sc_push_manager_t *push;
} sc_event_bridge_t;

void sc_event_bridge_init(sc_event_bridge_t *bridge,
    sc_control_protocol_t *proto, sc_bus_t *bus);
void sc_event_bridge_deinit(sc_event_bridge_t *bridge);
void sc_event_bridge_set_push(sc_event_bridge_t *bridge, sc_push_manager_t *push);
```

## Config Integration

```c
void sc_gateway_config_from_cfg(const struct sc_config_gateway *cfg_gw,
    sc_gateway_config_t *out);
```

## Usage Example

```c
sc_allocator_t alloc = sc_system_allocator();
sc_gateway_config_t cfg = {
    .host = "127.0.0.1",
    .port = 8080,
    .max_body_size = SC_GATEWAY_MAX_BODY_SIZE,
    .rate_limit_per_minute = SC_GATEWAY_RATE_LIMIT_PER_MIN,
    .test_mode = false,
    .on_webhook = my_webhook_handler,
    .on_webhook_ctx = my_ctx,
    .control_ui_dir = "/path/to/control-ui/dist",
};
sc_gateway_run(&alloc, cfg.host, cfg.port, &cfg);
```
