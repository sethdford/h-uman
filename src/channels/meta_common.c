#include "seaclaw/channels/meta_common.h"
#include "seaclaw/core/http.h"
#include "seaclaw/crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_META_GRAPH_BASE     "https://graph.facebook.com/v21.0/"
#define SC_META_GRAPH_BASE_LEN 35
#define SC_META_SIG_PREFIX     "sha256="
#define SC_META_SIG_PREFIX_LEN 7
#define SC_META_HMAC_HEX_LEN   64

bool sc_meta_verify_webhook(const char *body, size_t body_len, const char *signature,
                            const char *app_secret) {
#if SC_IS_TEST
    (void)body;
    (void)body_len;
    (void)signature;
    (void)app_secret;
    return true;
#else
    if (!body || !signature || !app_secret)
        return false;
    if (strncmp(signature, SC_META_SIG_PREFIX, SC_META_SIG_PREFIX_LEN) != 0)
        return false;
    const char *sig_hex = signature + SC_META_SIG_PREFIX_LEN;
    size_t sig_hex_len = strlen(sig_hex);
    if (sig_hex_len < SC_META_HMAC_HEX_LEN)
        return false;

    uint8_t computed[32];
    sc_hmac_sha256((const uint8_t *)app_secret, strlen(app_secret), (const uint8_t *)body, body_len,
                   computed);

    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", computed[i]);
    hex[64] = '\0';

    unsigned char diff = 0;
    for (int i = 0; i < SC_META_HMAC_HEX_LEN; i++)
        diff |= (unsigned char)sig_hex[i] ^ (unsigned char)hex[i];
    return diff == 0;
#endif
}

sc_error_t sc_meta_graph_send(sc_allocator_t *alloc, const char *access_token,
                              size_t access_token_len, const char *endpoint, size_t endpoint_len,
                              const char *json_body, size_t json_body_len) {
#if SC_IS_TEST
    (void)alloc;
    (void)access_token;
    (void)access_token_len;
    (void)endpoint;
    (void)endpoint_len;
    (void)json_body;
    (void)json_body_len;
    return SC_OK;
#else
    if (!alloc || !access_token || access_token_len == 0 || !endpoint || endpoint_len == 0 ||
        !json_body)
        return SC_ERR_INVALID_ARGUMENT;

    char *url = NULL;
    size_t url_len = 0;
    sc_error_t err = sc_meta_graph_url(alloc, endpoint, endpoint_len, &url, &url_len);
    if (err != SC_OK)
        return err;

    size_t auth_len = 7 + access_token_len;
    char *auth = (char *)alloc->alloc(alloc->ctx, auth_len + 1);
    if (!auth) {
        alloc->free(alloc->ctx, url, url_len + 1);
        return SC_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(auth, auth_len + 1, "Bearer %.*s", (int)access_token_len, access_token);
    if (n <= 0 || (size_t)n > auth_len) {
        alloc->free(alloc->ctx, auth, auth_len + 1);
        alloc->free(alloc->ctx, url, url_len + 1);
        return SC_ERR_INTERNAL;
    }

    sc_http_response_t resp = {0};
    err = sc_http_post_json(alloc, url, auth, json_body, json_body_len, &resp);
    alloc->free(alloc->ctx, auth, auth_len + 1);
    alloc->free(alloc->ctx, url, url_len + 1);

    if (err != SC_OK) {
        if (resp.owned && resp.body)
            sc_http_response_free(alloc, &resp);
        return SC_ERR_CHANNEL_SEND;
    }
    if (resp.owned && resp.body)
        sc_http_response_free(alloc, &resp);
    if (resp.status_code < 200 || resp.status_code >= 300)
        return SC_ERR_CHANNEL_SEND;
    return SC_OK;
#endif
}

sc_error_t sc_meta_graph_url(sc_allocator_t *alloc, const char *path, size_t path_len,
                             char **out_url, size_t *out_url_len) {
    if (!alloc || !path || !out_url || !out_url_len)
        return SC_ERR_INVALID_ARGUMENT;

    size_t total = SC_META_GRAPH_BASE_LEN + path_len + 1;
    char *url = (char *)alloc->alloc(alloc->ctx, total);
    if (!url)
        return SC_ERR_OUT_OF_MEMORY;

    memcpy(url, SC_META_GRAPH_BASE, SC_META_GRAPH_BASE_LEN);
    memcpy(url + SC_META_GRAPH_BASE_LEN, path, path_len);
    url[total - 1] = '\0';

    *out_url = url;
    *out_url_len = total - 1;
    return SC_OK;
}
