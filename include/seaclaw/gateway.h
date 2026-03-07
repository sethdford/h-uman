#ifndef SC_GATEWAY_H
#define SC_GATEWAY_H

#include "core/allocator.h"
#include "core/error.h"
#include "health.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Gateway config
 * ────────────────────────────────────────────────────────────────────────── */

#define SC_GATEWAY_MAX_BODY_SIZE      65536
#define SC_GATEWAY_RATE_LIMIT_PER_MIN 60

/* Forward declarations */
typedef struct sc_control_protocol sc_control_protocol_t;
struct sc_app_context;

typedef struct sc_gateway_config {
    const char *host;
    uint16_t port;
    size_t max_body_size;
    uint32_t rate_limit_per_minute;
    int rate_limit_requests; /* 0 = use rate_limit_per_minute semantics */
    int rate_limit_window;   /* seconds, 0 = 60 */
    const char *hmac_secret;
    size_t hmac_secret_len;
    bool test_mode;
    void (*on_webhook)(const char *channel, const char *body, size_t body_len, void *ctx);
    void *on_webhook_ctx;

    /* Control UI: directory containing static files to serve (SPA) */
    const char *control_ui_dir;

    /* CORS allowed origins (NULL/0 = localhost only). Configurable for deployments. */
    const char *const *cors_origins;
    size_t cors_origins_len;

    /* Auth token for WebSocket connections (NULL = localhost-only auto-approve) */
    const char *auth_token;

    /* When true, WebSocket RPC requires auth.token before protected methods */
    bool require_pairing;

    /* Control protocol handler (set by caller to wire RPC methods) */
    sc_control_protocol_t *control;

    /* Application context for RPC methods (sessions, tools, config, etc.) */
    struct sc_app_context *app_ctx;
} sc_gateway_config_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Gateway state (opaque)
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_gateway_state sc_gateway_state_t;

struct sc_config_gateway;

void sc_gateway_config_from_cfg(const struct sc_config_gateway *cfg_gw, sc_gateway_config_t *out);

/* ──────────────────────────────────────────────────────────────────────────
 * API
 * ────────────────────────────────────────────────────────────────────────── */

/* Run the gateway server (HTTP + WebSocket + static files). Blocks until stopped.
 * POSIX only: uses socket/bind/listen/accept + poll for multiplexing.
 * In tests, does NOT bind to a port if SC_GATEWAY_TEST_MODE is defined. */
sc_error_t sc_gateway_run(sc_allocator_t *alloc, const char *host, uint16_t port,
                          const sc_gateway_config_t *config);

bool sc_gateway_path_is(const char *path, const char *base);

/* Path security: returns true if path contains traversal attempts (.., %2e%2e, %00, etc.) */
bool sc_gateway_path_has_traversal(const char *path);

/* Webhook routing: returns true if path matches a webhook endpoint */
bool sc_gateway_is_webhook_path(const char *path);

/* CORS: returns true if origin is allowed (localhost/127.0.0.1/[::1] or in allowed list) */
bool sc_gateway_is_allowed_origin(const char *origin, const char *const *allowed, size_t n);

/* Content-Length parsing: SC_OK + *out_len, SC_ERR_INVALID_ARGUMENT (non-numeric),
 * SC_ERR_GATEWAY_BODY_TOO_LARGE (exceeds max). value is the part after "Content-Length: " */
sc_error_t sc_gateway_parse_content_length(const char *value, size_t max_body, size_t *out_len);

#if SC_IS_TEST
/* Test-only: process POST /api/pair body, return HTTP status and JSON body.
 * guard may be NULL (pairing not enabled). out_body allocated via alloc; caller must free. */
int sc_gateway_test_pair_request(sc_allocator_t *alloc, void *guard, size_t max_body,
                                 const char *body, size_t body_len, char **out_body,
                                 size_t *out_len);
#endif

#endif /* SC_GATEWAY_H */
