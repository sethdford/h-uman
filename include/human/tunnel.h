#ifndef HU_TUNNEL_H
#define HU_TUNNEL_H

#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Tunnel provider enum
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum hu_tunnel_provider {
    HU_TUNNEL_NONE,
    HU_TUNNEL_CLOUDFLARE,
    HU_TUNNEL_TAILSCALE,
    HU_TUNNEL_NGROK,
    HU_TUNNEL_CUSTOM,
} hu_tunnel_provider_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Tunnel errors
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum hu_tunnel_error {
    HU_TUNNEL_ERR_OK = 0,
    HU_TUNNEL_ERR_START_FAILED,
    HU_TUNNEL_ERR_PROCESS_SPAWN,
    HU_TUNNEL_ERR_URL_NOT_FOUND,
    HU_TUNNEL_ERR_TIMEOUT,
    HU_TUNNEL_ERR_INVALID_COMMAND,
    HU_TUNNEL_ERR_NOT_IMPLEMENTED,
} hu_tunnel_error_t;

const char *hu_tunnel_error_string(hu_tunnel_error_t err);

/* ──────────────────────────────────────────────────────────────────────────
 * Tunnel vtable
 * ────────────────────────────────────────────────────────────────────────── */

struct hu_tunnel_vtable;

typedef struct hu_tunnel {
    void *ctx;
    const struct hu_tunnel_vtable *vtable;
} hu_tunnel_t;

typedef struct hu_tunnel_vtable {
    hu_tunnel_error_t (*start)(void *ctx, uint16_t local_port, char **public_url_out,
                               size_t *url_len);
    void (*stop)(void *ctx);
    void (*deinit)(void *ctx, hu_allocator_t *alloc);
    const char *(*public_url)(void *ctx);
    const char *(*provider_name)(void *ctx);
    bool (*is_running)(void *ctx);
} hu_tunnel_vtable_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Tunnel config
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_tunnel_config {
    hu_tunnel_provider_t provider;
    const char *cloudflare_token; /* for cloudflare */
    size_t cloudflare_token_len;
    const char *ngrok_auth_token; /* for ngrok */
    size_t ngrok_auth_token_len;
    const char *ngrok_domain; /* optional */
    size_t ngrok_domain_len;
    const char *custom_start_cmd; /* for custom */
    size_t custom_start_cmd_len;
} hu_tunnel_config_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Factory
 * ────────────────────────────────────────────────────────────────────────── */

hu_tunnel_t hu_tunnel_create(hu_allocator_t *alloc, const hu_tunnel_config_t *config);

/* Implementation-specific constructors (for factory use) */
hu_tunnel_t hu_none_tunnel_create(hu_allocator_t *alloc);
hu_tunnel_t hu_cloudflare_tunnel_create(hu_allocator_t *alloc, const char *token, size_t token_len);
hu_tunnel_t hu_ngrok_tunnel_create(hu_allocator_t *alloc, const char *auth_token,
                                   size_t auth_token_len, const char *domain, size_t domain_len);
hu_tunnel_t hu_tailscale_tunnel_create(hu_allocator_t *alloc);
hu_tunnel_t hu_custom_tunnel_create(hu_allocator_t *alloc, const char *command_template,
                                    size_t command_template_len);

#endif /* HU_TUNNEL_H */
