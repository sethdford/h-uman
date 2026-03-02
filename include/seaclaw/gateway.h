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

#define SC_GATEWAY_MAX_BODY_SIZE 65536
#define SC_GATEWAY_RATE_LIMIT_PER_MIN 60

/* Forward declarations */
typedef struct sc_control_protocol sc_control_protocol_t;
struct sc_app_context;

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

    /* Control UI: directory containing static files to serve (SPA) */
    const char *control_ui_dir;

    /* Auth token for WebSocket connections (NULL = localhost-only auto-approve) */
    const char *auth_token;

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

void sc_gateway_config_from_cfg(const struct sc_config_gateway *cfg_gw,
                               sc_gateway_config_t *out);

/* ──────────────────────────────────────────────────────────────────────────
 * API
 * ────────────────────────────────────────────────────────────────────────── */

/* Run the gateway server (HTTP + WebSocket + static files). Blocks until stopped.
 * POSIX only: uses socket/bind/listen/accept + poll for multiplexing.
 * In tests, does NOT bind to a port if SC_GATEWAY_TEST_MODE is defined. */
sc_error_t sc_gateway_run(sc_allocator_t *alloc,
    const char *host, uint16_t port,
    const sc_gateway_config_t *config);

#endif /* SC_GATEWAY_H */
