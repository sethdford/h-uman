#ifndef SC_AUTH_H
#define SC_AUTH_H

#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * OAuth token
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_oauth_token {
    char *access_token;  /* owned */
    char *refresh_token; /* owned, may be NULL */
    int64_t expires_at;
    char *token_type; /* owned, e.g. "Bearer" */
} sc_oauth_token_t;

void sc_oauth_token_deinit(sc_oauth_token_t *t, sc_allocator_t *alloc);

/* True if expired or within 300s of expiring. */
bool sc_oauth_token_is_expired(const sc_oauth_token_t *t);

/* ──────────────────────────────────────────────────────────────────────────
 * Credential store — ~/.seaclaw/auth.json
 * ────────────────────────────────────────────────────────────────────────── */

sc_error_t sc_auth_save_credential(sc_allocator_t *alloc, const char *provider,
                                   const sc_oauth_token_t *token);

sc_error_t sc_auth_load_credential(sc_allocator_t *alloc, const char *provider,
                                   sc_oauth_token_t *token_out);

sc_error_t sc_auth_delete_credential(sc_allocator_t *alloc, const char *provider, bool *was_found);

/* ──────────────────────────────────────────────────────────────────────────
 * API key storage — get/set for a provider
 * ────────────────────────────────────────────────────────────────────────── */

char *sc_auth_get_api_key(sc_allocator_t *alloc, const char *provider);

sc_error_t sc_auth_set_api_key(sc_allocator_t *alloc, const char *provider, const char *api_key);

/* ──────────────────────────────────────────────────────────────────────────
 * OAuth device code flow (RFC 8628)
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_device_code {
    char *device_code;
    char *user_code;
    char *verification_uri;
    uint32_t interval;
    uint32_t expires_in;
} sc_device_code_t;

void sc_device_code_deinit(sc_device_code_t *dc, sc_allocator_t *alloc);

sc_error_t sc_auth_start_device_flow(sc_allocator_t *alloc, const char *client_id,
                                     const char *device_auth_url, const char *scope,
                                     sc_device_code_t *out);

sc_error_t sc_auth_poll_device_code(sc_allocator_t *alloc, const char *token_url,
                                    const char *client_id, const char *device_code,
                                    uint32_t interval_secs, sc_oauth_token_t *token_out);

/* ──────────────────────────────────────────────────────────────────────────
 * Token refresh
 * ────────────────────────────────────────────────────────────────────────── */

sc_error_t sc_auth_refresh_token(sc_allocator_t *alloc, const char *token_url,
                                 const char *client_id, const char *refresh_token,
                                 sc_oauth_token_t *token_out);

#endif /* SC_AUTH_H */
