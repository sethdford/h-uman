/* vertex_auth — Shared Vertex AI / Google Cloud ADC OAuth2 token management.
 * Extracted from src/providers/gemini.c so multiple modules (Gemini, Imagen,
 * Veo, etc.) can reuse the same credential loading and token refresh logic. */

#include "human/core/vertex_auth.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_GOOGLE_TOKEN_URL        "https://oauth2.googleapis.com/token"
#define HU_ADC_REFRESH_MARGIN_SECS 120

hu_error_t hu_vertex_auth_load_adc(hu_vertex_auth_t *auth, hu_allocator_t *alloc) {
    if (!auth || !alloc)
        return HU_ERR_INVALID_ARGUMENT;

    const char *home = getenv("HOME");
    if (!home)
        return HU_ERR_CONFIG_NOT_FOUND;

    const char *cred_env = getenv("GOOGLE_APPLICATION_CREDENTIALS");
    char path[512];
    if (cred_env && strlen(cred_env) > 0) {
        int pn = snprintf(path, sizeof(path), "%s", cred_env);
        if (pn < 0 || (size_t)pn >= sizeof(path))
            return HU_ERR_INVALID_ARGUMENT;
    } else {
        int pn = snprintf(path, sizeof(path),
                          "%s/.config/gcloud/application_default_credentials.json", home);
        if (pn < 0 || (size_t)pn >= sizeof(path))
            return HU_ERR_INVALID_ARGUMENT;
    }

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)path;
    auth->alloc = alloc;
    auth->client_id = hu_strndup(alloc, "test-client-id", 14);
    auth->client_secret = hu_strndup(alloc, "test-client-secret", 18);
    auth->refresh_token = hu_strndup(alloc, "test-refresh-token", 18);
    auth->access_token = hu_strndup(alloc, "test-access-token", 17);
    auth->access_token_len = 17;
    auth->token_expires_at = time(NULL) + 3600;
    return HU_OK;
#else
    FILE *f = fopen(path, "r");
    if (!f)
        return HU_ERR_CONFIG_NOT_FOUND;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0)
        return HU_ERR_PARSE;
    buf[n] = '\0';

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, buf, n, &root);
    if (err != HU_OK)
        return err;

    const char *type_str = hu_json_get_string(root, "type");
    if (!type_str || strcmp(type_str, "authorized_user") != 0) {
        hu_json_free(alloc, root);
        return HU_ERR_NOT_SUPPORTED;
    }

    const char *cid = hu_json_get_string(root, "client_id");
    const char *csec = hu_json_get_string(root, "client_secret");
    const char *rtok = hu_json_get_string(root, "refresh_token");
    if (!cid || !csec || !rtok) {
        hu_json_free(alloc, root);
        return HU_ERR_PARSE;
    }

    auth->client_id = hu_strndup(alloc, cid, strlen(cid));
    auth->client_secret = hu_strndup(alloc, csec, strlen(csec));
    auth->refresh_token = hu_strndup(alloc, rtok, strlen(rtok));
    hu_json_free(alloc, root);

    if (!auth->client_id || !auth->client_secret || !auth->refresh_token)
        return HU_ERR_OUT_OF_MEMORY;

    auth->alloc = alloc;
    auth->access_token = NULL;
    auth->access_token_len = 0;
    auth->token_expires_at = 0;
    return HU_OK;
#endif
}

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
static hu_error_t vertex_auth_refresh(hu_vertex_auth_t *auth, hu_allocator_t *alloc) {
    if (!auth->client_id || !auth->client_secret || !auth->refresh_token)
        return HU_ERR_PROVIDER_AUTH;

    char body[2048];
    int blen = snprintf(body, sizeof(body),
                        "client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token",
                        auth->client_id, auth->client_secret, auth->refresh_token);
    if (blen <= 0 || (size_t)blen >= sizeof(body))
        return HU_ERR_INVALID_ARGUMENT;

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_request(alloc, HU_GOOGLE_TOKEN_URL, "POST",
                                     "Content-Type: application/x-www-form-urlencoded", body,
                                     (size_t)blen, &resp);
    if (err != HU_OK)
        return err;

    if (resp.status_code < 200 || resp.status_code >= 300) {
        hu_http_response_free(alloc, &resp);
        return HU_ERR_PROVIDER_AUTH;
    }

    hu_json_value_t *root = NULL;
    err = hu_json_parse(alloc, resp.body, resp.body_len, &root);
    hu_http_response_free(alloc, &resp);
    if (err != HU_OK)
        return err;

    const char *token = hu_json_get_string(root, "access_token");
    double expires_in = hu_json_get_number(root, "expires_in", 3600.0);
    if (!token) {
        hu_json_free(alloc, root);
        return HU_ERR_PROVIDER_AUTH;
    }

    if (!isfinite(expires_in) || expires_in < 0.0 || expires_in > (double)(86400 * 7))
        expires_in = 3600.0;

    if (auth->access_token)
        alloc->free(alloc->ctx, auth->access_token, auth->access_token_len + 1);
    size_t tlen = strlen(token);
    auth->access_token = hu_strndup(alloc, token, tlen);
    auth->access_token_len = auth->access_token ? tlen : 0;
    auth->token_expires_at = time(NULL) + (time_t)expires_in;
    hu_json_free(alloc, root);
    return auth->access_token ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}
#endif /* !HU_IS_TEST */

hu_error_t hu_vertex_auth_ensure_token(hu_vertex_auth_t *auth, hu_allocator_t *alloc) {
    if (!auth || !alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (!auth->refresh_token)
        return auth->access_token ? HU_OK : HU_ERR_PROVIDER_AUTH;
    if (auth->access_token && time(NULL) < auth->token_expires_at - HU_ADC_REFRESH_MARGIN_SECS)
        return HU_OK;
#if defined(HU_IS_TEST) && HU_IS_TEST
    if (!auth->access_token) {
        auth->access_token = hu_strndup(alloc, "test-access-token", 17);
        auth->access_token_len = 17;
        auth->token_expires_at = time(NULL) + 3600;
    }
    return HU_OK;
#else
    return vertex_auth_refresh(auth, alloc);
#endif
}

hu_error_t hu_vertex_auth_get_bearer(const hu_vertex_auth_t *auth, char *buf, size_t buf_cap) {
    /* Callers hand buf straight to the HTTP layer as a C string; on every
     * failure path it must be an empty string, never left uninitialized. */
    if (buf && buf_cap > 0)
        buf[0] = '\0';
    if (!auth || !buf || buf_cap < 16)
        return HU_ERR_INVALID_ARGUMENT;
    if (!auth->access_token || auth->access_token_len == 0)
        return HU_ERR_PROVIDER_AUTH;
    int n = snprintf(buf, buf_cap, "Bearer %.*s", (int)auth->access_token_len, auth->access_token);
    if (n <= 0 || (size_t)n >= buf_cap) {
        buf[0] = '\0';
        return HU_ERR_INVALID_ARGUMENT;
    }
    return HU_OK;
}

void hu_vertex_auth_free(hu_vertex_auth_t *auth) {
    if (!auth || !auth->alloc)
        return;
    hu_allocator_t *alloc = auth->alloc;
    if (auth->client_id)
        alloc->free(alloc->ctx, auth->client_id, strlen(auth->client_id) + 1);
    if (auth->client_secret)
        alloc->free(alloc->ctx, auth->client_secret, strlen(auth->client_secret) + 1);
    if (auth->refresh_token)
        alloc->free(alloc->ctx, auth->refresh_token, strlen(auth->refresh_token) + 1);
    if (auth->access_token)
        alloc->free(alloc->ctx, auth->access_token, auth->access_token_len + 1);
    memset(auth, 0, sizeof(*auth));
}
