#ifndef HU_AUTH_H
#define HU_AUTH_H

#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * OAuth token
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_oauth_token {
    char *access_token;  /* owned */
    char *refresh_token; /* owned, may be NULL */
    int64_t expires_at;
    char *token_type; /* owned, e.g. "Bearer" */
} hu_oauth_token_t;

void hu_oauth_token_deinit(hu_oauth_token_t *t, hu_allocator_t *alloc);

/* True if expired or within 300s of expiring. */
bool hu_oauth_token_is_expired(const hu_oauth_token_t *t);

/* ──────────────────────────────────────────────────────────────────────────
 * Credential store — ~/.human/auth.json
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_auth_save_credential(hu_allocator_t *alloc, const char *provider,
                                   const hu_oauth_token_t *token);

hu_error_t hu_auth_load_credential(hu_allocator_t *alloc, const char *provider,
                                   hu_oauth_token_t *token_out);

hu_error_t hu_auth_delete_credential(hu_allocator_t *alloc, const char *provider, bool *was_found);

/* ──────────────────────────────────────────────────────────────────────────
 * API key storage — get/set for a provider
 * ────────────────────────────────────────────────────────────────────────── */

char *hu_auth_get_api_key(hu_allocator_t *alloc, const char *provider);

hu_error_t hu_auth_set_api_key(hu_allocator_t *alloc, const char *provider, const char *api_key);

/* ──────────────────────────────────────────────────────────────────────────
 * OAuth device code flow (RFC 8628)
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_device_code {
    char *device_code;
    char *user_code;
    char *verification_uri;
    uint32_t interval;
    uint32_t expires_in;
} hu_device_code_t;

void hu_device_code_deinit(hu_device_code_t *dc, hu_allocator_t *alloc);

hu_error_t hu_auth_start_device_flow(hu_allocator_t *alloc, const char *client_id,
                                     const char *device_auth_url, const char *scope,
                                     hu_device_code_t *out);

hu_error_t hu_auth_poll_device_code(hu_allocator_t *alloc, const char *token_url,
                                    const char *client_id, const char *device_code,
                                    uint32_t interval_secs, hu_oauth_token_t *token_out);

/* ──────────────────────────────────────────────────────────────────────────
 * Token refresh
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_auth_refresh_token(hu_allocator_t *alloc, const char *token_url,
                                 const char *client_id, const char *refresh_token,
                                 hu_oauth_token_t *token_out);

#endif /* HU_AUTH_H */
