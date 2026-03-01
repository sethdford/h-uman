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

typedef struct sc_gateway_config {
    const char *host;
    uint16_t port;
    size_t max_body_size;
    uint32_t rate_limit_per_minute;
    const char *hmac_secret;   /* for webhook verification, optional */
    size_t hmac_secret_len;
    bool test_mode;            /* if true, skip binding (for unit tests) */
    void (*on_webhook)(const char *channel, const char *body, size_t body_len, void *ctx);
    void *on_webhook_ctx;
} sc_gateway_config_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Gateway state (opaque)
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_gateway_state sc_gateway_state_t;

struct sc_config_gateway;

/* Map sc_config_gateway_t (config schema) to sc_gateway_config_t (runtime).
 * Caller must include seaclaw/config.h. */
void sc_gateway_config_from_cfg(const struct sc_config_gateway *cfg_gw,
                               sc_gateway_config_t *out);

/* ──────────────────────────────────────────────────────────────────────────
 * API
 * ────────────────────────────────────────────────────────────────────────── */

/* Run the HTTP gateway server. Blocks until stopped.
 * POSIX only: uses socket/bind/listen/accept.
 * In tests, does NOT bind to a port if SC_GATEWAY_TEST_MODE is defined.
 */
sc_error_t sc_gateway_run(sc_allocator_t *alloc,
    const char *host, uint16_t port,
    const sc_gateway_config_t *config);

#endif /* SC_GATEWAY_H */
