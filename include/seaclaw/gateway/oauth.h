#ifndef SC_GATEWAY_OAUTH_H
#define SC_GATEWAY_OAUTH_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct sc_oauth_config {
    const char *provider;      /* "google", "github", or custom */
    const char *client_id;
    const char *client_secret;
    const char *redirect_uri;
    const char *scopes;
    const char *authorize_url;
    const char *token_url;
} sc_oauth_config_t;

typedef struct sc_oauth_session {
    char session_id[65];
    char user_id[256];
    char access_token[2048];
    char refresh_token[2048];
    int64_t expires_at;
} sc_oauth_session_t;

typedef struct sc_oauth_ctx sc_oauth_ctx_t;

sc_error_t sc_oauth_init(sc_allocator_t *alloc, const sc_oauth_config_t *config,
                         sc_oauth_ctx_t **out);
void sc_oauth_destroy(sc_oauth_ctx_t *ctx);

sc_error_t sc_oauth_generate_pkce(sc_oauth_ctx_t *ctx, char *verifier, size_t verifier_size,
                                   char *challenge, size_t challenge_size);

sc_error_t sc_oauth_build_auth_url(sc_oauth_ctx_t *ctx, const char *challenge, size_t challenge_len,
                                    const char *state, size_t state_len,
                                    char *url_out, size_t url_out_size);

sc_error_t sc_oauth_exchange_code(sc_oauth_ctx_t *ctx, const char *code, size_t code_len,
                                  const char *verifier, size_t verifier_len,
                                  sc_oauth_session_t *session_out);

sc_error_t sc_oauth_refresh_token(sc_oauth_ctx_t *ctx, sc_oauth_session_t *session);

bool sc_oauth_session_valid(const sc_oauth_session_t *session);

#endif
