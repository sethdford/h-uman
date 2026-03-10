#ifndef HU_CONTROL_PROTOCOL_H
#define HU_CONTROL_PROTOCOL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/gateway/ws_server.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hu_agent;
struct hu_config;
struct hu_session_manager;
struct hu_cron_scheduler;
struct hu_skillforge;
struct hu_cost_tracker;
struct hu_bus;
struct hu_tool;
struct hu_push_manager;
struct hu_pairing_guard;
struct hu_graph;

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
    struct hu_agent *agent; /* gateway agent for persona.set; NULL when --with-agent not used */
    struct hu_graph *graph; /* knowledge graph for memory.graph RPC; NULL when not opened */
} hu_app_context_t;

typedef struct hu_control_protocol {
    hu_allocator_t *alloc;
    hu_ws_server_t *ws;
    uint64_t event_seq;
    hu_app_context_t *app_ctx;
    bool require_pairing;
    struct hu_pairing_guard *pairing_guard;
    const char *auth_token;
    void *oauth_ctx; /* hu_oauth_ctx_t * for auth.oauth.* methods */
    void *oauth_pending_ctx;
    void (*oauth_pending_store)(void *ctx, const char *state, const char *verifier);
    const char *(*oauth_pending_lookup)(void *ctx, const char *state);
    void (*oauth_pending_remove)(void *ctx, const char *state);
    uint32_t rpc_count;     /* RPC calls in current window */
    uint64_t rpc_window_ms; /* Window start timestamp */
} hu_control_protocol_t;

void hu_control_protocol_init(hu_control_protocol_t *proto, hu_allocator_t *alloc,
                              hu_ws_server_t *ws);
void hu_control_protocol_deinit(hu_control_protocol_t *proto);

void hu_control_set_app_ctx(hu_control_protocol_t *proto, hu_app_context_t *ctx);

void hu_control_set_auth(hu_control_protocol_t *proto, bool require_pairing,
                         struct hu_pairing_guard *pairing_guard, const char *auth_token);

void hu_control_set_oauth(hu_control_protocol_t *proto, void *oauth_ctx);

void hu_control_set_oauth_pending(hu_control_protocol_t *proto, void *ctx,
                                  void (*store)(void *ctx, const char *state, const char *verifier),
                                  const char *(*lookup)(void *ctx, const char *state),
                                  void (*remove)(void *ctx, const char *state));

void hu_control_on_message(hu_ws_conn_t *conn, const char *data, size_t data_len, void *ctx);
void hu_control_on_close(hu_ws_conn_t *conn, void *ctx);

void hu_control_send_event(hu_control_protocol_t *proto, const char *event_name,
                           const char *payload_json);
hu_error_t hu_control_send_response(hu_ws_conn_t *conn, const char *id, bool ok,
                                    const char *payload_json);

#endif /* HU_CONTROL_PROTOCOL_H */
