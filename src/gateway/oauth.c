#include "human/gateway/oauth.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct hu_oauth_ctx {
    hu_allocator_t *alloc;
    hu_oauth_config_t config;
};

#if defined(HU_HTTP_CURL) && !HU_IS_TEST
static int oauth_form_encode_char(char *out, size_t cap, size_t *j, unsigned char c) {
    if (*j + 4 > cap)
        return -1;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.' || c == '~') {
        out[(*j)++] = (char)c;
        return 0;
    }
    if (c == ' ') {
        out[(*j)++] = '+';
        return 0;
    }
    out[(*j)++] = '%';
    out[(*j)++] = (char)((c >> 4) < 10 ? '0' + (c >> 4) : 'A' + ((c >> 4) - 10));
    out[(*j)++] = (char)((c & 0x0f) < 10 ? '0' + (c & 0x0f) : 'A' + ((c & 0x0f) - 10));
    return 0;
}
#endif
hu_error_t hu_oauth_init(hu_allocator_t *alloc, const hu_oauth_config_t *config,
                         hu_oauth_ctx_t **out) {
    if (!alloc || !config || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_oauth_ctx_t *ctx = (hu_oauth_ctx_t *)alloc->alloc(alloc->ctx, sizeof(hu_oauth_ctx_t));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(*ctx));
    ctx->alloc = alloc;
    ctx->config = *config;
    *out = ctx;
    return HU_OK;
}

void hu_oauth_destroy(hu_oauth_ctx_t *ctx) {
    if (!ctx || !ctx->alloc)
        return;
    ctx->alloc->free(ctx->alloc->ctx, ctx, sizeof(hu_oauth_ctx_t));
}

hu_error_t hu_oauth_generate_pkce(hu_oauth_ctx_t *ctx, char *verifier, size_t verifier_size,
                                  char *challenge, size_t challenge_size) {
    if (!ctx || !verifier || verifier_size < 44 || !challenge || challenge_size < 44)
        return HU_ERR_INVALID_ARGUMENT;

    static const char b64url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)(uintptr_t)ctx;
    for (size_t i = 0; i < 43 && i < verifier_size - 1; i++) {
        seed = seed * 1103515245u + 12345u;
        verifier[i] = b64url[(seed >> 16) & 63];
    }
    verifier[43] = '\0';

#if HU_IS_TEST
    /* Simplified: copy verifier as challenge for testing (no hash in test builds) */
    size_t vlen = strlen(verifier);
    size_t clen = (vlen < challenge_size - 1) ? vlen : challenge_size - 1;
    memcpy(challenge, verifier, clen);
    challenge[clen] = '\0';
#else
    /* S256 challenge = base64url(SHA256(verifier)) */
    uint8_t hash[32];
    hu_sha256((const uint8_t *)verifier, strlen(verifier), hash);
    size_t ci = 0;
    for (size_t i = 0; i < 32 && ci < challenge_size - 2; i += 3) {
        uint32_t triple = ((uint32_t)hash[i] << 16);
        if (i + 1 < 32)
            triple |= ((uint32_t)hash[i + 1] << 8);
        if (i + 2 < 32)
            triple |= (uint32_t)hash[i + 2];
        if (ci < challenge_size - 1)
            challenge[ci++] = b64url[(triple >> 18) & 63];
        if (ci < challenge_size - 1)
            challenge[ci++] = b64url[(triple >> 12) & 63];
        if (ci < challenge_size - 1)
            challenge[ci++] = b64url[(triple >> 6) & 63];
        if (ci < challenge_size - 1)
            challenge[ci++] = b64url[triple & 63];
    }
    challenge[ci] = '\0';
#endif
    return HU_OK;
}

hu_error_t hu_oauth_build_auth_url(hu_oauth_ctx_t *ctx, const char *challenge, size_t challenge_len,
                                   const char *state, size_t state_len, char *url_out,
                                   size_t url_out_size) {
    if (!ctx || !challenge || !url_out || url_out_size < 256)
        return HU_ERR_INVALID_ARGUMENT;

    const char *auth_url = ctx->config.authorize_url;
    if (!auth_url) {
        if (ctx->config.provider && strcmp(ctx->config.provider, "google") == 0)
            auth_url = "https://accounts.google.com/o/oauth2/v2/auth";
        else if (ctx->config.provider && strcmp(ctx->config.provider, "github") == 0)
            auth_url = "https://github.com/login/oauth/authorize";
        else
            return HU_ERR_INVALID_ARGUMENT;
    }

    int n = snprintf(url_out, url_out_size,
                     "%s?client_id=%s&redirect_uri=%s&response_type=code"
                     "&code_challenge=%.*s&code_challenge_method=S256&state=%.*s&scope=%s",
                     auth_url, ctx->config.client_id ? ctx->config.client_id : "",
                     ctx->config.redirect_uri ? ctx->config.redirect_uri : "", (int)challenge_len,
                     challenge, (int)state_len, state,
                     ctx->config.scopes ? ctx->config.scopes : "openid email");

    if (n < 0 || (size_t)n >= url_out_size)
        return HU_ERR_INVALID_ARGUMENT;
    return HU_OK;
}

hu_error_t hu_oauth_exchange_code(hu_oauth_ctx_t *ctx, const char *code, size_t code_len,
                                  const char *verifier, size_t verifier_len,
                                  hu_oauth_session_t *session_out) {
    if (!ctx || !code || !verifier || !session_out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(session_out, 0, sizeof(*session_out));

#if HU_IS_TEST
    (void)code_len;
    (void)verifier_len;
    snprintf(session_out->session_id, sizeof(session_out->session_id), "test-session-001");
    snprintf(session_out->user_id, sizeof(session_out->user_id), "test-user");
    snprintf(session_out->access_token, sizeof(session_out->access_token), "test-access-token");
    snprintf(session_out->refresh_token, sizeof(session_out->refresh_token), "test-refresh-token");
    session_out->expires_at = (int64_t)time(NULL) + 3600;
    return HU_OK;
#elif defined(HU_HTTP_CURL)
    const char *token_url = ctx->config.token_url;
    if (!token_url) {
        if (ctx->config.provider && strcmp(ctx->config.provider, "google") == 0)
            token_url = "https://oauth2.googleapis.com/token";
        else if (ctx->config.provider && strcmp(ctx->config.provider, "github") == 0)
            token_url = "https://github.com/login/oauth/access_token";
        else
            return HU_ERR_INVALID_ARGUMENT;
    }

    char body[2048];
    size_t blen = 0;
    if (blen + 20 < sizeof(body))
        memcpy(body + blen, "grant_type=authorization_code&code=", 35), blen += 35;
    for (size_t i = 0; i < code_len && blen < sizeof(body) - 4; i++) {
        if (oauth_form_encode_char(body, sizeof(body), &blen, (unsigned char)code[i]) != 0)
            return HU_ERR_INVALID_ARGUMENT;
    }
    if (blen + 20 < sizeof(body))
        memcpy(body + blen, "&redirect_uri=", 14), blen += 14;
    for (const char *p = ctx->config.redirect_uri ? ctx->config.redirect_uri : "";
         *p && blen < sizeof(body) - 4; p++) {
        if (oauth_form_encode_char(body, sizeof(body), &blen, (unsigned char)*p) != 0)
            return HU_ERR_INVALID_ARGUMENT;
    }
    if (blen + 20 < sizeof(body))
        memcpy(body + blen, "&client_id=", 11), blen += 11;
    for (const char *p = ctx->config.client_id ? ctx->config.client_id : "";
         *p && blen < sizeof(body) - 4; p++) {
        if (oauth_form_encode_char(body, sizeof(body), &blen, (unsigned char)*p) != 0)
            return HU_ERR_INVALID_ARGUMENT;
    }
    if (ctx->config.client_secret && ctx->config.client_secret[0]) {
        if (blen + 20 < sizeof(body))
            memcpy(body + blen, "&client_secret=", 15), blen += 15;
        for (const char *p = ctx->config.client_secret; *p && blen < sizeof(body) - 4; p++) {
            if (oauth_form_encode_char(body, sizeof(body), &blen, (unsigned char)*p) != 0)
                return HU_ERR_INVALID_ARGUMENT;
        }
    }
    if (blen + 20 < sizeof(body))
        memcpy(body + blen, "&code_verifier=", 15), blen += 15;
    for (size_t i = 0; i < verifier_len && blen < sizeof(body) - 4; i++) {
        if (oauth_form_encode_char(body, sizeof(body), &blen, (unsigned char)verifier[i]) != 0)
            return HU_ERR_INVALID_ARGUMENT;
    }
    body[blen] = '\0';

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_request(ctx->alloc, token_url, "POST",
                                     "Content-Type: application/x-www-form-urlencoded\n"
                                     "Accept: application/json\nUser-Agent: Human/1.0",
                                     body, blen, &resp);
    if (err != HU_OK)
        return err;
    if (resp.status_code < 200 || resp.status_code >= 300) {
        if (resp.owned && resp.body)
            hu_http_response_free(ctx->alloc, &resp);
        return HU_ERR_PROVIDER_AUTH;
    }

    int64_t expires_at = (int64_t)time(NULL) + 3600;
    hu_json_value_t *root = NULL;
    if (resp.body && resp.body_len > 0) {
        if (hu_json_parse(ctx->alloc, resp.body, resp.body_len, &root) == HU_OK && root) {
            const char *at = hu_json_get_string(root, "access_token");
            const char *rt = hu_json_get_string(root, "refresh_token");
            double exp = hu_json_get_number(root, "expires_in", 3600.0);
            if (at) {
                size_t atlen = strlen(at);
                if (atlen >= sizeof(session_out->access_token))
                    atlen = sizeof(session_out->access_token) - 1;
                memcpy(session_out->access_token, at, atlen);
                session_out->access_token[atlen] = '\0';
            }
            if (rt) {
                size_t rtlen = strlen(rt);
                if (rtlen >= sizeof(session_out->refresh_token))
                    rtlen = sizeof(session_out->refresh_token) - 1;
                memcpy(session_out->refresh_token, rt, rtlen);
                session_out->refresh_token[rtlen] = '\0';
            }
            if (exp > 0)
                expires_at = (int64_t)time(NULL) + (int64_t)exp;
            hu_json_free(ctx->alloc, root);
        }
    }

    snprintf(session_out->session_id, sizeof(session_out->session_id), "sess-%lld",
             (long long)time(NULL));
    session_out->expires_at = expires_at;

    if (resp.owned && resp.body)
        hu_http_response_free(ctx->alloc, &resp);
    return HU_OK;
#else
    (void)code_len;
    (void)verifier_len;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

hu_error_t hu_oauth_refresh_token(hu_oauth_ctx_t *ctx, hu_oauth_session_t *session) {
    if (!ctx || !session)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    session->expires_at = (int64_t)time(NULL) + 3600;
    snprintf(session->access_token, sizeof(session->access_token), "refreshed-test-token");
    return HU_OK;
#elif defined(HU_HTTP_CURL)
    const char *token_url = ctx->config.token_url;
    if (!token_url) {
        if (ctx->config.provider && strcmp(ctx->config.provider, "google") == 0)
            token_url = "https://oauth2.googleapis.com/token";
        else if (ctx->config.provider && strcmp(ctx->config.provider, "github") == 0)
            token_url = "https://github.com/login/oauth/access_token";
        else
            return HU_ERR_INVALID_ARGUMENT;
    }

    char body[2048];
    size_t blen = 0;
    if (blen + 40 < sizeof(body))
        memcpy(body + blen, "grant_type=refresh_token&refresh_token=", 39), blen += 39;
    for (const char *p = session->refresh_token; *p && blen < sizeof(body) - 4; p++) {
        if (oauth_form_encode_char(body, sizeof(body), &blen, (unsigned char)*p) != 0)
            return HU_ERR_INVALID_ARGUMENT;
    }
    if (blen + 20 < sizeof(body))
        memcpy(body + blen, "&client_id=", 11), blen += 11;
    for (const char *p = ctx->config.client_id ? ctx->config.client_id : "";
         *p && blen < sizeof(body) - 4; p++) {
        if (oauth_form_encode_char(body, sizeof(body), &blen, (unsigned char)*p) != 0)
            return HU_ERR_INVALID_ARGUMENT;
    }
    if (ctx->config.client_secret && ctx->config.client_secret[0]) {
        if (blen + 20 < sizeof(body))
            memcpy(body + blen, "&client_secret=", 15), blen += 15;
        for (const char *p = ctx->config.client_secret; *p && blen < sizeof(body) - 4; p++) {
            if (oauth_form_encode_char(body, sizeof(body), &blen, (unsigned char)*p) != 0)
                return HU_ERR_INVALID_ARGUMENT;
        }
    }
    body[blen] = '\0';

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_request(ctx->alloc, token_url, "POST",
                                     "Content-Type: application/x-www-form-urlencoded\n"
                                     "Accept: application/json\nUser-Agent: Human/1.0",
                                     body, blen, &resp);
    if (err != HU_OK)
        return err;
    if (resp.status_code < 200 || resp.status_code >= 300) {
        if (resp.owned && resp.body)
            hu_http_response_free(ctx->alloc, &resp);
        return HU_ERR_PROVIDER_AUTH;
    }

    int64_t expires_at = (int64_t)time(NULL) + 3600;
    hu_json_value_t *root = NULL;
    if (resp.body && resp.body_len > 0) {
        if (hu_json_parse(ctx->alloc, resp.body, resp.body_len, &root) == HU_OK && root) {
            const char *at = hu_json_get_string(root, "access_token");
            const char *rt = hu_json_get_string(root, "refresh_token");
            double exp = hu_json_get_number(root, "expires_in", 3600.0);
            if (at) {
                size_t atlen = strlen(at);
                if (atlen >= sizeof(session->access_token))
                    atlen = sizeof(session->access_token) - 1;
                memcpy(session->access_token, at, atlen);
                session->access_token[atlen] = '\0';
            }
            if (rt) {
                size_t rtlen = strlen(rt);
                if (rtlen >= sizeof(session->refresh_token))
                    rtlen = sizeof(session->refresh_token) - 1;
                memcpy(session->refresh_token, rt, rtlen);
                session->refresh_token[rtlen] = '\0';
            }
            if (exp > 0)
                expires_at = (int64_t)time(NULL) + (int64_t)exp;
            hu_json_free(ctx->alloc, root);
        }
    }

    session->expires_at = expires_at;

    if (resp.owned && resp.body)
        hu_http_response_free(ctx->alloc, &resp);
    return HU_OK;
#else
    (void)ctx;
    (void)session;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

bool hu_oauth_session_valid(const hu_oauth_session_t *session) {
    if (!session || session->session_id[0] == '\0')
        return false;
    return (int64_t)time(NULL) < session->expires_at;
}

const char *hu_oauth_get_provider(hu_oauth_ctx_t *ctx) {
    if (!ctx)
        return NULL;
    return ctx->config.provider;
}
