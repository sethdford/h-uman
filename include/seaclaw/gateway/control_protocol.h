#ifndef SC_CONTROL_PROTOCOL_H
#define SC_CONTROL_PROTOCOL_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/gateway/ws_server.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct sc_config;
struct sc_session_manager;
struct sc_cron_scheduler;
struct sc_skillforge;
struct sc_cost_tracker;
struct sc_bus;
struct sc_tool;
struct sc_push_manager;

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

void sc_control_set_app_ctx(sc_control_protocol_t *proto,
    sc_app_context_t *ctx);

void sc_control_on_message(sc_ws_conn_t *conn, const char *data, size_t data_len,
    void *ctx);
void sc_control_on_close(sc_ws_conn_t *conn, void *ctx);

void sc_control_send_event(sc_control_protocol_t *proto, const char *event_name,
    const char *payload_json);
sc_error_t sc_control_send_response(sc_ws_conn_t *conn, const char *id,
    bool ok, const char *payload_json);

#endif /* SC_CONTROL_PROTOCOL_H */
