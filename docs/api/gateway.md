---
title: Gateway API
description: HTTP server, webhooks, Control UI, WebSocket protocol, and event bridge
updated: 2026-03-02
---

# Gateway API

The gateway provides an HTTP server for webhooks, a Control UI (SPA), WebSocket control protocol, push notifications, and event bridge.

## Gateway Server (`gateway.h`)

```c
#define HU_GATEWAY_MAX_BODY_SIZE 65536
#define HU_GATEWAY_RATE_LIMIT_PER_MIN 60

typedef struct hu_gateway_config {
    const char *host;
    uint16_t port;
    size_t max_body_size;
    uint32_t rate_limit_per_minute;
    const char *hmac_secret;
    size_t hmac_secret_len;
    bool test_mode;
    bool (*on_webhook)(const char *channel, const char *body, size_t body_len, void *ctx);
    void *on_webhook_ctx;
    const char *control_ui_dir;
    const char *const *cors_origins;
    size_t cors_origins_len;
    const char *auth_token;
    hu_control_protocol_t *control;
    struct hu_app_context *app_ctx;
} hu_gateway_config_t;

hu_error_t hu_gateway_run(hu_allocator_t *alloc,
    const char *host, uint16_t port,
    const hu_gateway_config_t *config);
```

Runs HTTP + WebSocket + static files. Blocks until stopped. POSIX only (socket/poll).

## Control Protocol (`gateway/control_protocol.h`)

```c
typedef struct hu_app_context {
    struct hu_config *config;
    hu_allocator_t *alloc;
    struct hu_session_manager *sessions;
    struct hu_cron_scheduler *cron;
    struct hu_skillforge *skills;
    struct hu_cost_tracker *costs;
    struct hu_bus *bus;
    struct hu_tool *tools;
    size_t tools_count;
    struct hu_push_manager *push;
} hu_app_context_t;

typedef struct hu_control_protocol {
    hu_allocator_t *alloc;
    hu_ws_server_t *ws;
    uint64_t event_seq;
    hu_app_context_t *app_ctx;
} hu_control_protocol_t;

void hu_control_protocol_init(hu_control_protocol_t *proto,
    hu_allocator_t *alloc, hu_ws_server_t *ws);
void hu_control_protocol_deinit(hu_control_protocol_t *proto);
void hu_control_set_app_ctx(hu_control_protocol_t *proto, hu_app_context_t *ctx);

void hu_control_on_message(hu_ws_conn_t *conn, const char *data, size_t data_len, void *ctx);
void hu_control_on_close(hu_ws_conn_t *conn, void *ctx);

void hu_control_send_event(hu_control_protocol_t *proto, const char *event_name,
    const char *payload_json);
hu_error_t hu_control_send_response(hu_ws_conn_t *conn, const char *id,
    bool ok, const char *payload_json);
```

## Push (`gateway/push.h`)

```c
typedef enum hu_push_provider {
    HU_PUSH_NONE = 0,
    HU_PUSH_FCM,
    HU_PUSH_APNS,
} hu_push_provider_t;

typedef struct hu_push_manager {
    hu_allocator_t *alloc;
    hu_push_config_t config;
    hu_push_token_t *tokens;
    size_t token_count;
    size_t token_cap;
} hu_push_manager_t;

hu_error_t hu_push_init(hu_push_manager_t *mgr, hu_allocator_t *alloc,
    const hu_push_config_t *config);
void hu_push_deinit(hu_push_manager_t *mgr);

hu_error_t hu_push_register_token(hu_push_manager_t *mgr,
    const char *device_token, hu_push_provider_t provider);
hu_error_t hu_push_unregister_token(hu_push_manager_t *mgr,
    const char *device_token);

hu_error_t hu_push_send(hu_push_manager_t *mgr,
    const char *title, const char *body,
    const char *data_json);
hu_error_t hu_push_send_to(hu_push_manager_t *mgr,
    const char *device_token,
    const char *title, const char *body,
    const char *data_json);
```

## Event Bridge (`gateway/event_bridge.h`)

Bridges bus events to control protocol and push notifications.

```c
typedef struct hu_event_bridge {
    hu_control_protocol_t *proto;
    hu_bus_t *bus;
    hu_bus_subscriber_fn bus_cb;
    hu_push_manager_t *push;
} hu_event_bridge_t;

void hu_event_bridge_init(hu_event_bridge_t *bridge,
    hu_control_protocol_t *proto, hu_bus_t *bus);
void hu_event_bridge_deinit(hu_event_bridge_t *bridge);
void hu_event_bridge_set_push(hu_event_bridge_t *bridge, hu_push_manager_t *push);
```

## Config Integration

```c
void hu_gateway_config_from_cfg(const struct hu_config_gateway *cfg_gw,
    hu_gateway_config_t *out);
```

## Usage Example

```c
hu_allocator_t alloc = hu_system_allocator();
hu_gateway_config_t cfg = {
    .host = "127.0.0.1",
    .port = 8080,
    .max_body_size = HU_GATEWAY_MAX_BODY_SIZE,
    .rate_limit_per_minute = HU_GATEWAY_RATE_LIMIT_PER_MIN,
    .test_mode = false,
    .on_webhook = my_webhook_handler,
    .on_webhook_ctx = my_ctx,
    .control_ui_dir = "/path/to/control-ui/dist",
};
hu_gateway_run(&alloc, cfg.host, cfg.port, &cfg);
```
