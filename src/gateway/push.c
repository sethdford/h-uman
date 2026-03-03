#include "seaclaw/gateway/push.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef SC_HAS_TLS
#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#endif

#define SC_PUSH_INITIAL_CAP 4
#define SC_FCM_URL          "https://fcm.googleapis.com/fcm/send"
#define SC_APNS_URL_BASE    "https://api.push.apple.com/3/device/"

#ifndef SC_IS_TEST

static const char *fcm_url(const sc_push_config_t *config) {
    if (config->endpoint && config->endpoint[0])
        return config->endpoint;
    return SC_FCM_URL;
}

/* ── Base64url encoding (no padding) for JWT ─────────────────────────────── */

static const char b64url_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static size_t base64url_encode(const unsigned char *in, size_t in_len, char *out, size_t out_cap) {
    size_t pos = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < in_len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < in_len) v |= (uint32_t)in[i + 2];

        if (pos < out_cap) out[pos++] = b64url_table[(v >> 18) & 0x3F];
        if (pos < out_cap) out[pos++] = b64url_table[(v >> 12) & 0x3F];
        if (i + 1 < in_len && pos < out_cap) out[pos++] = b64url_table[(v >> 6) & 0x3F];
        if (i + 2 < in_len && pos < out_cap) out[pos++] = b64url_table[v & 0x3F];
    }
    if (pos < out_cap) out[pos] = '\0';
    return pos;
}

#ifdef SC_HAS_TLS
/* Build ES256 JWT for APNS authentication.
 * Returns heap-allocated JWT string or NULL on failure. */
static char *apns_build_jwt(sc_allocator_t *alloc, const char *key_pem, size_t key_len,
                            const char *key_id, const char *team_id) {
    if (!key_pem || key_len == 0 || !key_id || !team_id)
        return NULL;

    /* Parse P8 PEM key */
    BIO *bio = BIO_new_mem_buf(key_pem, (int)key_len);
    if (!bio)
        return NULL;
    EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
    BIO_free(bio);
    if (!pkey)
        return NULL;

    /* JWT header: {"alg":"ES256","kid":"<key_id>"} */
    char header_json[128];
    int hlen = snprintf(header_json, sizeof(header_json),
                        "{\"alg\":\"ES256\",\"kid\":\"%s\"}", key_id);
    if (hlen <= 0 || (size_t)hlen >= sizeof(header_json)) {
        EVP_PKEY_free(pkey);
        return NULL;
    }

    /* JWT payload: {"iss":"<team_id>","iat":<timestamp>} */
    char payload_json[128];
    int plen = snprintf(payload_json, sizeof(payload_json),
                        "{\"iss\":\"%s\",\"iat\":%lld}", team_id, (long long)time(NULL));
    if (plen <= 0 || (size_t)plen >= sizeof(payload_json)) {
        EVP_PKEY_free(pkey);
        return NULL;
    }

    /* Base64url encode header and payload */
    char header_b64[256], payload_b64[256];
    size_t h64 = base64url_encode((const unsigned char *)header_json, (size_t)hlen,
                                  header_b64, sizeof(header_b64));
    size_t p64 = base64url_encode((const unsigned char *)payload_json, (size_t)plen,
                                  payload_b64, sizeof(payload_b64));

    /* Signing input: header.payload */
    char signing_input[600];
    int si_len = snprintf(signing_input, sizeof(signing_input), "%.*s.%.*s",
                          (int)h64, header_b64, (int)p64, payload_b64);
    if (si_len <= 0 || (size_t)si_len >= sizeof(signing_input)) {
        EVP_PKEY_free(pkey);
        return NULL;
    }

    /* Sign with ES256 (ECDSA + SHA-256) using EVP API */
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
        EVP_PKEY_free(pkey);
        return NULL;
    }

    unsigned char sig_der[256];
    size_t sig_der_len = sizeof(sig_der);
    int rc = EVP_DigestSignInit(mdctx, NULL, EVP_sha256(), NULL, pkey);
    if (rc != 1) {
        EVP_MD_CTX_free(mdctx);
        EVP_PKEY_free(pkey);
        return NULL;
    }
    rc = EVP_DigestSign(mdctx, sig_der, &sig_der_len,
                        (const unsigned char *)signing_input, (size_t)si_len);
    EVP_MD_CTX_free(mdctx);
    EVP_PKEY_free(pkey);
    if (rc != 1)
        return NULL;

    /* Convert DER signature to fixed 64-byte R||S for JWS ES256.
     * DER: 30 <len> 02 <rlen> <R> 02 <slen> <S> */
    unsigned char r_s[64];
    memset(r_s, 0, 64);
    if (sig_der_len < 8 || sig_der[0] != 0x30) return NULL;
    const unsigned char *p = sig_der + 2;
    if (*p != 0x02) return NULL;
    p++;
    size_t rlen = *p++;
    const unsigned char *r_ptr = p;
    p += rlen;
    if (*p != 0x02) return NULL;
    p++;
    size_t slen = *p++;
    const unsigned char *s_ptr = p;

    /* Copy R (right-aligned into 32 bytes, skip leading zero) */
    if (rlen > 32 && r_ptr[0] == 0x00) { r_ptr++; rlen--; }
    if (rlen <= 32) memcpy(r_s + 32 - rlen, r_ptr, rlen);
    /* Copy S (right-aligned into 32 bytes, skip leading zero) */
    if (slen > 32 && s_ptr[0] == 0x00) { s_ptr++; slen--; }
    if (slen <= 32) memcpy(r_s + 64 - slen, s_ptr, slen);

    /* Base64url encode the 64-byte R||S */
    char sig_b64[128];
    size_t s64 = base64url_encode(r_s, 64, sig_b64, sizeof(sig_b64));

    /* Assemble JWT: header.payload.signature */
    size_t jwt_len = (size_t)si_len + 1 + s64;
    char *jwt = (char *)alloc->alloc(alloc->ctx, jwt_len + 1);
    if (!jwt) return NULL;
    snprintf(jwt, jwt_len + 1, "%s.%.*s", signing_input, (int)s64, sig_b64);
    return jwt;
}
#endif /* SC_HAS_TLS */

static sc_error_t sc_push_apns_send(sc_push_manager_t *mgr, const char *device_token,
                                    const char *title, const char *body, const char *data_json) {
    (void)data_json;
#if !defined(SC_HAS_TLS)
    (void)mgr; (void)device_token; (void)title; (void)body;
    return SC_ERR_NOT_SUPPORTED;
#else
    if (!mgr->config.server_key || mgr->config.server_key_len == 0)
        return SC_ERR_INVALID_ARGUMENT;
    if (!mgr->config.key_id || !mgr->config.team_id)
        return SC_ERR_INVALID_ARGUMENT;

    char *jwt = apns_build_jwt(mgr->alloc, mgr->config.server_key, mgr->config.server_key_len,
                               mgr->config.key_id, mgr->config.team_id);
    if (!jwt)
        return SC_ERR_INVALID_ARGUMENT;

    /* Build URL */
    const char *base = (mgr->config.endpoint && mgr->config.endpoint[0])
                           ? mgr->config.endpoint : SC_APNS_URL_BASE;
    size_t url_cap = strlen(base) + strlen(device_token) + 1;
    char *url = (char *)mgr->alloc->alloc(mgr->alloc->ctx, url_cap);
    if (!url) {
        mgr->alloc->free(mgr->alloc->ctx, jwt, strlen(jwt) + 1);
        return SC_ERR_OUT_OF_MEMORY;
    }
    snprintf(url, url_cap, "%s%s", base, device_token);

    /* Build JSON payload */
    sc_json_buf_t buf;
    if (sc_json_buf_init(&buf, mgr->alloc) != SC_OK) {
        mgr->alloc->free(mgr->alloc->ctx, url, url_cap);
        mgr->alloc->free(mgr->alloc->ctx, jwt, strlen(jwt) + 1);
        return SC_ERR_OUT_OF_MEMORY;
    }
    sc_error_t err = sc_json_buf_append_raw(&buf, "{\"aps\":{\"alert\":{\"title\":", 28);
    if (err == SC_OK)
        err = sc_json_append_string(&buf, title ? title : "", title ? strlen(title) : 0);
    if (err == SC_OK)
        err = sc_json_buf_append_raw(&buf, ",\"body\":", 8);
    if (err == SC_OK)
        err = sc_json_append_string(&buf, body ? body : "", body ? strlen(body) : 0);
    if (err == SC_OK)
        err = sc_json_buf_append_raw(&buf, "},\"sound\":\"default\"}}", 22);
    if (err != SC_OK) {
        sc_json_buf_free(&buf);
        mgr->alloc->free(mgr->alloc->ctx, url, url_cap);
        mgr->alloc->free(mgr->alloc->ctx, jwt, strlen(jwt) + 1);
        return err;
    }

    /* Build auth header: "bearer <jwt>" */
    size_t jwt_len = strlen(jwt);
    size_t auth_cap = 7 + jwt_len + 1;
    char *auth = (char *)mgr->alloc->alloc(mgr->alloc->ctx, auth_cap);
    if (!auth) {
        sc_json_buf_free(&buf);
        mgr->alloc->free(mgr->alloc->ctx, url, url_cap);
        mgr->alloc->free(mgr->alloc->ctx, jwt, strlen(jwt) + 1);
        return SC_ERR_OUT_OF_MEMORY;
    }
    snprintf(auth, auth_cap, "bearer %s", jwt);
    mgr->alloc->free(mgr->alloc->ctx, jwt, jwt_len + 1);

    /* POST to APNS */
    sc_http_response_t resp = {0};
    err = sc_http_post_json(mgr->alloc, url, auth, buf.ptr, buf.len, &resp);
    mgr->alloc->free(mgr->alloc->ctx, auth, auth_cap);
    mgr->alloc->free(mgr->alloc->ctx, url, url_cap);
    sc_json_buf_free(&buf);

    if (err != SC_OK)
        return err;
    bool ok = (resp.status_code >= 200 && resp.status_code < 300);
    sc_http_response_free(mgr->alloc, &resp);
    return ok ? SC_OK : SC_ERR_PROVIDER_RESPONSE;
#endif /* SC_HAS_TLS */
}

#endif /* !SC_IS_TEST */

sc_error_t sc_push_init(sc_push_manager_t *mgr, sc_allocator_t *alloc,
                        const sc_push_config_t *config) {
    if (!mgr || !alloc || !config)
        return SC_ERR_INVALID_ARGUMENT;

    mgr->alloc = alloc;
    mgr->config = *config;
    mgr->token_count = 0;
    mgr->token_cap = SC_PUSH_INITIAL_CAP;
    mgr->tokens =
        (sc_push_token_t *)alloc->alloc(alloc->ctx, SC_PUSH_INITIAL_CAP * sizeof(sc_push_token_t));
    if (!mgr->tokens)
        return SC_ERR_OUT_OF_MEMORY;
    memset(mgr->tokens, 0, SC_PUSH_INITIAL_CAP * sizeof(sc_push_token_t));
    return SC_OK;
}

void sc_push_deinit(sc_push_manager_t *mgr) {
    if (!mgr)
        return;
    if (mgr->tokens && mgr->alloc) {
        for (size_t i = 0; i < mgr->token_count; i++) {
            if (mgr->tokens[i].device_token) {
                mgr->alloc->free(mgr->alloc->ctx, mgr->tokens[i].device_token,
                                 strlen(mgr->tokens[i].device_token) + 1);
            }
        }
        mgr->alloc->free(mgr->alloc->ctx, mgr->tokens, mgr->token_cap * sizeof(sc_push_token_t));
    }
    mgr->tokens = NULL;
    mgr->token_count = 0;
    mgr->token_cap = 0;
}

sc_error_t sc_push_register_token(sc_push_manager_t *mgr, const char *device_token,
                                  sc_push_provider_t provider) {
    if (!mgr || !device_token || !device_token[0])
        return SC_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < mgr->token_count; i++) {
        if (mgr->tokens[i].device_token && strcmp(mgr->tokens[i].device_token, device_token) == 0)
            return SC_OK; /* duplicate, no-op */
    }

    if (mgr->token_count >= mgr->token_cap) {
        size_t new_cap = mgr->token_cap * 2;
        sc_push_token_t *new_tokens = (sc_push_token_t *)mgr->alloc->realloc(
            mgr->alloc->ctx, mgr->tokens, mgr->token_cap * sizeof(sc_push_token_t),
            new_cap * sizeof(sc_push_token_t));
        if (!new_tokens)
            return SC_ERR_OUT_OF_MEMORY;
        mgr->tokens = new_tokens;
        mgr->token_cap = new_cap;
        memset(mgr->tokens + mgr->token_count, 0,
               (new_cap - mgr->token_count) * sizeof(sc_push_token_t));
    }

    char *dup = sc_strdup(mgr->alloc, device_token);
    if (!dup)
        return SC_ERR_OUT_OF_MEMORY;
    mgr->tokens[mgr->token_count].device_token = dup;
    mgr->tokens[mgr->token_count].provider = provider;
    mgr->token_count++;
    return SC_OK;
}

sc_error_t sc_push_unregister_token(sc_push_manager_t *mgr, const char *device_token) {
    if (!mgr || !device_token)
        return SC_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < mgr->token_count; i++) {
        if (mgr->tokens[i].device_token && strcmp(mgr->tokens[i].device_token, device_token) == 0) {
            mgr->alloc->free(mgr->alloc->ctx, mgr->tokens[i].device_token,
                             strlen(mgr->tokens[i].device_token) + 1);
            mgr->tokens[i].device_token = NULL;
            for (size_t j = i + 1; j < mgr->token_count; j++)
                mgr->tokens[j - 1] = mgr->tokens[j];
            mgr->tokens[mgr->token_count - 1].device_token = NULL;
            mgr->tokens[mgr->token_count - 1].provider = SC_PUSH_NONE;
            mgr->token_count--;
            return SC_OK;
        }
    }
    return SC_OK; /* not found is not an error */
}

sc_error_t sc_push_send(sc_push_manager_t *mgr, const char *title, const char *body,
                        const char *data_json) {
    if (!mgr)
        return SC_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < mgr->token_count; i++) {
        if (mgr->tokens[i].device_token) {
            sc_error_t err =
                sc_push_send_to(mgr, mgr->tokens[i].device_token, title, body, data_json);
            if (err != SC_OK)
                return err;
        }
    }
    return SC_OK;
}

sc_error_t sc_push_send_to(sc_push_manager_t *mgr, const char *device_token, const char *title,
                           const char *body, const char *data_json) {
    if (!mgr || !device_token)
        return SC_ERR_INVALID_ARGUMENT;

#ifdef SC_IS_TEST
    (void)title;
    (void)body;
    (void)data_json;
    return SC_OK;
#else
    switch (mgr->config.provider) {
    case SC_PUSH_NONE:
        return SC_OK;
    case SC_PUSH_APNS:
        return sc_push_apns_send(mgr, device_token, title, body, data_json);
    case SC_PUSH_FCM: {
        if (!mgr->config.server_key || mgr->config.server_key_len == 0)
            return SC_ERR_INVALID_ARGUMENT;

        sc_json_buf_t buf;
        if (sc_json_buf_init(&buf, mgr->alloc) != SC_OK)
            return SC_ERR_OUT_OF_MEMORY;

        sc_error_t err = sc_json_buf_append_raw(&buf, "{\"to\":", 6);
        if (err == SC_OK)
            err = sc_json_append_string(&buf, device_token, strlen(device_token));
        if (err == SC_OK)
            err = sc_json_buf_append_raw(&buf, ",\"notification\":{\"title\":", 26);
        if (err == SC_OK)
            err = sc_json_append_string(&buf, title ? title : "", title ? strlen(title) : 0);
        if (err == SC_OK)
            err = sc_json_buf_append_raw(&buf, ",\"body\":", 8);
        if (err == SC_OK)
            err = sc_json_append_string(&buf, body ? body : "", body ? strlen(body) : 0);
        if (err == SC_OK)
            err = sc_json_buf_append_raw(&buf, "}", 1);
        if (err == SC_OK && data_json && data_json[0]) {
            err = sc_json_buf_append_raw(&buf, ",\"data\":", 8);
            if (err == SC_OK)
                err = sc_json_buf_append_raw(&buf, data_json, strlen(data_json));
        }
        if (err == SC_OK)
            err = sc_json_buf_append_raw(&buf, "}", 1);

        if (err != SC_OK) {
            sc_json_buf_free(&buf);
            return err;
        }

        /* Build auth header: "key=SERVER_KEY" */
        size_t auth_len = 4 + mgr->config.server_key_len + 1;
        char *auth_buf = (char *)mgr->alloc->alloc(mgr->alloc->ctx, auth_len);
        if (!auth_buf) {
            sc_json_buf_free(&buf);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(auth_buf, "key=", 4);
        memcpy(auth_buf + 4, mgr->config.server_key, mgr->config.server_key_len);
        auth_buf[4 + mgr->config.server_key_len] = '\0';

        sc_http_response_t resp = {0};
        err =
            sc_http_post_json(mgr->alloc, fcm_url(&mgr->config), auth_buf, buf.ptr, buf.len, &resp);
        mgr->alloc->free(mgr->alloc->ctx, auth_buf, auth_len);
        sc_json_buf_free(&buf);

        if (err != SC_OK)
            return err;
        sc_http_response_free(mgr->alloc, &resp);
        return SC_OK;
    }
    default:
        return SC_ERR_INVALID_ARGUMENT;
    }
#endif
}
