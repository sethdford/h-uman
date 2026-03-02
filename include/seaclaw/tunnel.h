#ifndef SC_TUNNEL_H
#define SC_TUNNEL_H

#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Tunnel provider enum
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum sc_tunnel_provider {
    SC_TUNNEL_NONE,
    SC_TUNNEL_CLOUDFLARE,
    SC_TUNNEL_TAILSCALE,
    SC_TUNNEL_NGROK,
    SC_TUNNEL_CUSTOM,
} sc_tunnel_provider_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Tunnel errors
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum sc_tunnel_error {
    SC_TUNNEL_ERR_OK = 0,
    SC_TUNNEL_ERR_START_FAILED,
    SC_TUNNEL_ERR_PROCESS_SPAWN,
    SC_TUNNEL_ERR_URL_NOT_FOUND,
    SC_TUNNEL_ERR_TIMEOUT,
    SC_TUNNEL_ERR_INVALID_COMMAND,
    SC_TUNNEL_ERR_NOT_IMPLEMENTED,
} sc_tunnel_error_t;

const char *sc_tunnel_error_string(sc_tunnel_error_t err);

/* ──────────────────────────────────────────────────────────────────────────
 * Tunnel vtable
 * ────────────────────────────────────────────────────────────────────────── */

struct sc_tunnel_vtable;

typedef struct sc_tunnel {
    void *ctx;
    const struct sc_tunnel_vtable *vtable;
} sc_tunnel_t;

typedef struct sc_tunnel_vtable {
    sc_tunnel_error_t (*start)(void *ctx, uint16_t local_port, char **public_url_out, size_t *url_len);
    void (*stop)(void *ctx);
    void (*deinit)(void *ctx, sc_allocator_t *alloc);
    const char *(*public_url)(void *ctx);
    const char *(*provider_name)(void *ctx);
    bool (*is_running)(void *ctx);
} sc_tunnel_vtable_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Tunnel config
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_tunnel_config {
    sc_tunnel_provider_t provider;
    const char *cloudflare_token;   /* for cloudflare */
    size_t cloudflare_token_len;
    const char *ngrok_auth_token;    /* for ngrok */
    size_t ngrok_auth_token_len;
    const char *ngrok_domain;        /* optional */
    size_t ngrok_domain_len;
    const char *custom_start_cmd;    /* for custom */
    size_t custom_start_cmd_len;
} sc_tunnel_config_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Factory
 * ────────────────────────────────────────────────────────────────────────── */

sc_tunnel_t sc_tunnel_create(sc_allocator_t *alloc, const sc_tunnel_config_t *config);

/* Implementation-specific constructors (for factory use) */
sc_tunnel_t sc_none_tunnel_create(sc_allocator_t *alloc);
sc_tunnel_t sc_cloudflare_tunnel_create(sc_allocator_t *alloc,
    const char *token, size_t token_len);
sc_tunnel_t sc_ngrok_tunnel_create(sc_allocator_t *alloc,
    const char *auth_token, size_t auth_token_len,
    const char *domain, size_t domain_len);
sc_tunnel_t sc_tailscale_tunnel_create(sc_allocator_t *alloc);
sc_tunnel_t sc_custom_tunnel_create(sc_allocator_t *alloc,
    const char *command_template, size_t command_template_len);

#endif /* SC_TUNNEL_H */
