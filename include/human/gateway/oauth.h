#ifndef HU_GATEWAY_OAUTH_H
#define HU_GATEWAY_OAUTH_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hu_oauth_config {
    const char *provider; /* "google", "github", or custom */
    const char *client_id;
    const char *client_secret;
    const char *redirect_uri;
    const char *scopes;
    const char *authorize_url;
    const char *token_url;
} hu_oauth_config_t;

typedef struct hu_oauth_session {
    char session_id[65];
    char user_id[256];
    char access_token[2048];
    char refresh_token[2048];
    int64_t expires_at;
} hu_oauth_session_t;

typedef struct hu_oauth_ctx hu_oauth_ctx_t;

hu_error_t hu_oauth_init(hu_allocator_t *alloc, const hu_oauth_config_t *config,
                         hu_oauth_ctx_t **out);
void hu_oauth_destroy(hu_oauth_ctx_t *ctx);

hu_error_t hu_oauth_generate_pkce(hu_oauth_ctx_t *ctx, char *verifier, size_t verifier_size,
                                  char *challenge, size_t challenge_size);

hu_error_t hu_oauth_build_auth_url(hu_oauth_ctx_t *ctx, const char *challenge, size_t challenge_len,
                                   const char *state, size_t state_len, char *url_out,
                                   size_t url_out_size);

hu_error_t hu_oauth_exchange_code(hu_oauth_ctx_t *ctx, const char *code, size_t code_len,
                                  const char *verifier, size_t verifier_len,
                                  hu_oauth_session_t *session_out);

hu_error_t hu_oauth_refresh_token(hu_oauth_ctx_t *ctx, hu_oauth_session_t *session);

bool hu_oauth_session_valid(const hu_oauth_session_t *session);

const char *hu_oauth_get_provider(hu_oauth_ctx_t *ctx);

#endif
