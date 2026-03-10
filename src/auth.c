#include "human/auth.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/platform.h"
#include "human/security.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if defined(HU_HTTP_CURL) && !HU_IS_TEST
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#endif

#define HU_AUTH_DIR      ".human"
#define HU_AUTH_FILE     "auth.json"
#define HU_AUTH_MAX_BODY 65536

#if defined(HU_HTTP_CURL) && !HU_IS_TEST
static int url_encode_char(char *out, size_t cap, size_t *j, unsigned char c) {
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
    out[(*j)++] = (char)(c < 10 ? '0' + c : 'A' + (c - 10));
    out[(*j)++] = (char)((c & 0x0f) < 10 ? '0' + (c & 0x0f) : 'A' + ((c & 0x0f) - 10));
    return 0;
}
#endif

void hu_oauth_token_deinit(hu_oauth_token_t *t, hu_allocator_t *alloc) {
    if (!t || !alloc)
        return;
    if (t->access_token) {
        alloc->free(alloc->ctx, t->access_token, strlen(t->access_token) + 1);
        t->access_token = NULL;
    }
    if (t->refresh_token) {
        alloc->free(alloc->ctx, t->refresh_token, strlen(t->refresh_token) + 1);
        t->refresh_token = NULL;
    }
    if (t->token_type) {
        alloc->free(alloc->ctx, t->token_type, strlen(t->token_type) + 1);
        t->token_type = NULL;
    }
}

bool hu_oauth_token_is_expired(const hu_oauth_token_t *t) {
    if (!t || t->expires_at == 0)
        return false;
    return (int64_t)time(NULL) + 300 >= t->expires_at;
}

static char *auth_file_path(hu_allocator_t *alloc) {
    char *home = hu_platform_get_home_dir(alloc);
    if (!home)
        return NULL;
    size_t hlen = strlen(home), need = hlen + strlen(HU_AUTH_DIR) + strlen(HU_AUTH_FILE) + 4;
    char *path = alloc->alloc(alloc->ctx, need);
    if (!path) {
        alloc->free(alloc->ctx, home, hlen + 1);
        return NULL;
    }
    snprintf(path, need, "%s/%s/%s", home, HU_AUTH_DIR, HU_AUTH_FILE);
    alloc->free(alloc->ctx, home, hlen + 1);
    return path;
}

hu_error_t hu_auth_save_credential(hu_allocator_t *alloc, const char *provider,
                                   const hu_oauth_token_t *token) {
    if (!alloc || !provider || !token)
        return HU_ERR_INVALID_ARGUMENT;
    char *path = auth_file_path(alloc);
    if (!path)
        return HU_ERR_IO;
    /* Encrypt credentials via hu_secret_store (falls back to plaintext if unavailable) */
    char config_dir[512];
    snprintf(config_dir, sizeof(config_dir), "%.*s",
             (int)(strrchr(path, '/') ? (size_t)(strrchr(path, '/') - path) : 0), path);
    hu_secret_store_t *store = hu_secret_store_create(alloc, config_dir, true);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    alloc->free(alloc->ctx, path, strlen(path) + 1);
    if (fd < 0) {
        if (store)
            hu_secret_store_destroy(store, alloc);
        return HU_ERR_IO;
    }
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        if (store)
            hu_secret_store_destroy(store, alloc);
        return HU_ERR_IO;
    }

    char *enc_access = NULL;
    char *enc_refresh = NULL;
    if (store && token->access_token) {
        hu_error_t enc_err =
            hu_secret_store_encrypt(store, alloc, token->access_token, &enc_access);
        if (enc_err != HU_OK) {
            enc_access = NULL; /* fall back to plaintext */
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
            fprintf(stderr, "[auth] encrypt access_token failed: %s\n", hu_error_string(enc_err));
#endif
        }
    }
    if (store && token->refresh_token) {
        hu_error_t enc_err =
            hu_secret_store_encrypt(store, alloc, token->refresh_token, &enc_refresh);
        if (enc_err != HU_OK) {
            enc_refresh = NULL; /* fall back to plaintext */
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
            fprintf(stderr, "[auth] encrypt refresh_token failed: %s\n", hu_error_string(enc_err));
#endif
        }
    }
    const char *access_out = enc_access ? enc_access : token->access_token;
    const char *refresh_out = enc_refresh ? enc_refresh : token->refresh_token;

    fprintf(f, "{\"%s\":{", provider);
    fprintf(f, "\"access_token\":\"");
    for (const char *p = access_out; p && *p; p++) {
        if (*p == '"' || *p == '\\')
            fputc('\\', f);
        fputc(*p, f);
    }
    fprintf(f, "\",\"expires_at\":%lld,\"token_type\":\"%s\"", (long long)token->expires_at,
            token->token_type ? token->token_type : "Bearer");
    if (refresh_out) {
        fprintf(f, ",\"refresh_token\":\"");
        for (const char *p = refresh_out; p && *p; p++) {
            if (*p == '"' || *p == '\\')
                fputc('\\', f);
            fputc(*p, f);
        }
        fprintf(f, "\"");
    }
    fprintf(f, "}}\n");
    fclose(f);
    if (enc_access)
        alloc->free(alloc->ctx, enc_access, strlen(enc_access) + 1);
    if (enc_refresh)
        alloc->free(alloc->ctx, enc_refresh, strlen(enc_refresh) + 1);
    if (store)
        hu_secret_store_destroy(store, alloc);
    return HU_OK;
}

hu_error_t hu_auth_load_credential(hu_allocator_t *alloc, const char *provider,
                                   hu_oauth_token_t *token_out) {
    if (!alloc || !provider || !token_out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(token_out, 0, sizeof(*token_out));
    char *path = auth_file_path(alloc);
    if (!path)
        return HU_OK; /* No home = no credential */
    FILE *f = fopen(path, "rb");
    alloc->free(alloc->ctx, path, strlen(path) + 1);
    if (!f)
        return HU_OK; /* No file = no credential */
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (long)HU_AUTH_MAX_BODY) {
        fclose(f);
        return HU_OK;
    }
    char *buf = alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t nr = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nr] = '\0';
    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, buf, nr, &root);
    alloc->free(alloc->ctx, buf, (size_t)sz + 1);
    if (err != HU_OK || !root)
        return err;
    hu_json_value_t *prov = hu_json_object_get(root, provider);
    if (!prov || prov->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return HU_OK;
    }
    const char *at = hu_json_get_string(prov, "access_token");
    if (!at || !at[0]) {
        hu_json_free(alloc, root);
        return HU_OK;
    }

    /* Decrypt if encrypted (enc2: prefix) */
    char *decrypted_at = NULL;
    char *decrypted_rt = NULL;
    hu_secret_store_t *store = NULL;
    if (strncmp(at, "enc2:", 5) == 0) {
        char *lpath = auth_file_path(alloc);
        if (lpath) {
            char cdir[512];
            const char *slash = strrchr(lpath, '/');
            if (slash) {
                size_t dlen = (size_t)(slash - lpath);
                if (dlen >= sizeof(cdir))
                    dlen = sizeof(cdir) - 1;
                memcpy(cdir, lpath, dlen);
                cdir[dlen] = '\0';
            } else {
                cdir[0] = '.';
                cdir[1] = '\0';
            }
            alloc->free(alloc->ctx, lpath, strlen(lpath) + 1);
            store = hu_secret_store_create(alloc, cdir, false);
            if (store) {
                hu_error_t dec_err = hu_secret_store_decrypt(store, alloc, at, &decrypted_at);
                if (dec_err != HU_OK)
                    decrypted_at = NULL; /* fall back to raw value */
            }
        }
    }
    token_out->access_token = decrypted_at ? decrypted_at : hu_strdup(alloc, at);
    if (!token_out->access_token) {
        if (store)
            hu_secret_store_destroy(store, alloc);
        hu_json_free(alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }
    const char *rt = hu_json_get_string(prov, "refresh_token");
    if (rt && rt[0]) {
        if (store && strncmp(rt, "enc2:", 5) == 0) {
            hu_error_t dec_err = hu_secret_store_decrypt(store, alloc, rt, &decrypted_rt);
            if (dec_err != HU_OK)
                decrypted_rt = NULL; /* keep encrypted value, caller sees it */
        }
        token_out->refresh_token = decrypted_rt ? decrypted_rt : hu_strdup(alloc, rt);
    }
    if (store)
        hu_secret_store_destroy(store, alloc);
    token_out->expires_at = (int64_t)hu_json_get_number(prov, "expires_at", 0);
    const char *tt = hu_json_get_string(prov, "token_type");
    token_out->token_type = hu_strdup(alloc, tt && tt[0] ? tt : "Bearer");
    if (!token_out->token_type) {
        hu_oauth_token_deinit(token_out, alloc);
        hu_json_free(alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }
    hu_json_free(alloc, root);
    if (hu_oauth_token_is_expired(token_out)) {
        hu_oauth_token_deinit(token_out, alloc);
        return HU_OK; /* Expired = treat as not found */
    }
    return HU_OK;
}

hu_error_t hu_auth_delete_credential(hu_allocator_t *alloc, const char *provider, bool *was_found) {
    if (!alloc || !provider)
        return HU_ERR_INVALID_ARGUMENT;
    if (was_found)
        *was_found = false;
    hu_oauth_token_t tok;
    if (hu_auth_load_credential(alloc, provider, &tok) != HU_OK)
        return HU_OK;
    if (!tok.access_token)
        return HU_OK; /* Not found */
    hu_oauth_token_deinit(&tok, alloc);
    if (was_found)
        *was_found = true;
    /* Save empty credential to remove */
    char *path = auth_file_path(alloc);
    if (!path)
        return HU_OK;
    FILE *f = fopen(path, "wb");
    alloc->free(alloc->ctx, path, strlen(path) + 1);
    if (f) {
        fprintf(f, "{}\n");
        fclose(f);
    }
    return HU_OK;
}

char *hu_auth_get_api_key(hu_allocator_t *alloc, const char *provider) {
    if (!alloc || !provider)
        return NULL;
    hu_oauth_token_t tok;
    if (hu_auth_load_credential(alloc, provider, &tok) != HU_OK)
        return NULL;
    char *key = tok.access_token;
    tok.access_token = NULL;
    hu_oauth_token_deinit(&tok, alloc);
    if (key && hu_secret_store_is_encrypted(key)) {
        char *home = hu_platform_get_home_dir(alloc);
        if (home) {
            char config_dir[512];
            snprintf(config_dir, sizeof(config_dir), "%s/.human", home);
            alloc->free(alloc->ctx, home, strlen(home) + 1);
            hu_secret_store_t *store = hu_secret_store_create(alloc, config_dir, true);
            if (store) {
                char *plain = NULL;
                if (hu_secret_store_decrypt(store, alloc, key, &plain) == HU_OK && plain) {
                    alloc->free(alloc->ctx, key, strlen(key) + 1);
                    key = plain;
                }
                hu_secret_store_destroy(store, alloc);
            }
        }
    }
    return key;
}

hu_error_t hu_auth_set_api_key(hu_allocator_t *alloc, const char *provider, const char *api_key) {
    if (!alloc || !provider)
        return HU_ERR_INVALID_ARGUMENT;
    hu_oauth_token_t tok = {0};
    tok.access_token = api_key ? hu_strdup(alloc, api_key) : NULL;
    tok.token_type = hu_strdup(alloc, "Bearer");
    tok.expires_at = 0;
    if (!tok.access_token && api_key)
        return HU_ERR_OUT_OF_MEMORY;
    if (!tok.token_type) {
        if (tok.access_token)
            alloc->free(alloc->ctx, tok.access_token, strlen(tok.access_token) + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    hu_error_t err = hu_auth_save_credential(alloc, provider, &tok);
    hu_oauth_token_deinit(&tok, alloc);
    return err;
}

void hu_device_code_deinit(hu_device_code_t *dc, hu_allocator_t *alloc) {
    if (!dc || !alloc)
        return;
    if (dc->device_code) {
        alloc->free(alloc->ctx, dc->device_code, strlen(dc->device_code) + 1);
        dc->device_code = NULL;
    }
    if (dc->user_code) {
        alloc->free(alloc->ctx, dc->user_code, strlen(dc->user_code) + 1);
        dc->user_code = NULL;
    }
    if (dc->verification_uri) {
        alloc->free(alloc->ctx, dc->verification_uri, strlen(dc->verification_uri) + 1);
        dc->verification_uri = NULL;
    }
}

hu_error_t hu_auth_start_device_flow(hu_allocator_t *alloc, const char *client_id,
                                     const char *device_auth_url, const char *scope,
                                     hu_device_code_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    (void)client_id;
    (void)device_auth_url;
    (void)scope;
#if HU_IS_TEST
    if (out) {
        out->device_code = hu_strdup(alloc, "mock-device-code");
        out->user_code = hu_strdup(alloc, "MOCK-1234");
        out->verification_uri = hu_strdup(alloc, "https://example.com/activate");
        out->interval = 5;
        out->expires_in = 900;
    }
    return HU_OK;
#elif defined(HU_HTTP_CURL)
    /* POST client_id and scope to device authorization URL (form-urlencoded) */
    char body[4096];
    size_t j = 0;
    if (j + 12 < sizeof(body))
        memcpy(body + j, "client_id=", 10), j += 10;
    for (const char *p = client_id ? client_id : ""; *p && j < sizeof(body) - 4;) {
        if (url_encode_char(body, sizeof(body), &j, (unsigned char)*p) != 0)
            return HU_ERR_INVALID_ARGUMENT;
        p++;
    }
    if (j + 8 < sizeof(body))
        memcpy(body + j, "&scope=", 7), j += 7;
    for (const char *p = scope ? scope : ""; *p && j < sizeof(body) - 4;) {
        if (url_encode_char(body, sizeof(body), &j, (unsigned char)*p) != 0)
            return HU_ERR_INVALID_ARGUMENT;
        p++;
    }
    body[j] = '\0';

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_request(
        alloc, device_auth_url, "POST",
        "Content-Type: application/x-www-form-urlencoded\nUser-Agent: Human/1.0", body, j, &resp);
    if (err != HU_OK)
        return err;

    hu_json_value_t *root = NULL;
    err = hu_json_parse(alloc, resp.body, resp.body_len, &root);
    hu_http_response_free(alloc, &resp);
    if (err != HU_OK || !root)
        return err;

    const char *dc = hu_json_get_string(root, "device_code");
    const char *uc = hu_json_get_string(root, "user_code");
    const char *vu = hu_json_get_string(root, "verification_uri");
    if (!vu)
        vu = hu_json_get_string(root, "verification_url");
    if (!dc || !uc || !vu) {
        hu_json_free(alloc, root);
        return HU_ERR_INVALID_ARGUMENT;
    }

    out->device_code = hu_strdup(alloc, dc);
    out->user_code = hu_strdup(alloc, uc);
    out->verification_uri = hu_strdup(alloc, vu);
    out->interval = (uint32_t)hu_json_get_number(root, "interval", 5);
    out->expires_in = (uint32_t)hu_json_get_number(root, "expires_in", 900);
    hu_json_free(alloc, root);

    if (!out->device_code || !out->user_code || !out->verification_uri) {
        hu_device_code_deinit(out, alloc);
        return HU_ERR_OUT_OF_MEMORY;
    }
    return HU_OK;
#else
    (void)client_id;
    (void)device_auth_url;
    (void)scope;
    return HU_ERR_NOT_SUPPORTED; /* HTTP client (libcurl) required for device flow */
#endif
}

hu_error_t hu_auth_poll_device_code(hu_allocator_t *alloc, const char *token_url,
                                    const char *client_id, const char *device_code,
                                    uint32_t interval_secs, hu_oauth_token_t *token_out) {
    if (!alloc || !token_out)
        return HU_ERR_INVALID_ARGUMENT;
    (void)token_url;
    (void)client_id;
    (void)device_code;
    (void)interval_secs;
#if HU_IS_TEST
    if (token_out) {
        token_out->access_token = hu_strdup(alloc, "mock-access-token");
        token_out->refresh_token = hu_strdup(alloc, "mock-refresh-token");
        token_out->expires_at = time(NULL) + 3600;
        token_out->token_type = hu_strdup(alloc, "Bearer");
    }
    return HU_OK;
#elif defined(HU_HTTP_CURL)
    char body[1024];
    int n = snprintf(
        body, sizeof(body),
        "client_id=%s&device_code=%s&grant_type=urn:ietf:params:oauth:grant-type:device_code",
        client_id ? client_id : "", device_code ? device_code : "");
    if (n <= 0 || (size_t)n >= sizeof(body))
        return HU_ERR_INVALID_ARGUMENT;

    for (unsigned i = 0; i < 120; i++) {
        if (i > 0) {
#ifdef _WIN32
            Sleep((DWORD)(interval_secs * 1000));
#else
            sleep(interval_secs);
#endif
        }

        hu_http_response_t resp = {0};
        hu_error_t err = hu_http_request(
            alloc, token_url, "POST",
            "Content-Type: application/x-www-form-urlencoded\nUser-Agent: Human/1.0", body,
            (size_t)n, &resp);
        if (err != HU_OK)
            return err;

        if (resp.status_code == 200) {
            hu_json_value_t *root = NULL;
            err = hu_json_parse(alloc, resp.body, resp.body_len, &root);
            hu_http_response_free(alloc, &resp);
            if (err != HU_OK || !root)
                continue;

            const char *at = hu_json_get_string(root, "access_token");
            const char *rt = hu_json_get_string(root, "refresh_token");
            const char *tt = hu_json_get_string(root, "token_type");
            int64_t exp_in = (int64_t)hu_json_get_number(root, "expires_in", 3600);
            hu_json_free(alloc, root);
            if (!at || !at[0])
                continue;

            token_out->access_token = hu_strdup(alloc, at);
            token_out->refresh_token = (rt && rt[0]) ? hu_strdup(alloc, rt) : NULL;
            token_out->token_type = hu_strdup(alloc, tt && tt[0] ? tt : "Bearer");
            token_out->expires_at = time(NULL) + exp_in;
            if (!token_out->access_token || !token_out->token_type) {
                hu_oauth_token_deinit(token_out, alloc);
                return HU_ERR_OUT_OF_MEMORY;
            }
            return HU_OK;
        }

        /* Check for authorization_pending / slow_down — keep polling */
        hu_json_value_t *root = NULL;
        if (hu_json_parse(alloc, resp.body, resp.body_len, &root) == HU_OK && root) {
            const char *err_str = hu_json_get_string(root, "error");
            hu_json_free(alloc, root);
            if (err_str && (strcmp(err_str, "authorization_pending") == 0 ||
                            strcmp(err_str, "slow_down") == 0))
                continue;
        }
        hu_http_response_free(alloc, &resp);
        return HU_ERR_INVALID_ARGUMENT; /* access_denied, expired_token, etc. */
    }
    return HU_ERR_TIMEOUT;
#else
    (void)token_url;
    (void)client_id;
    (void)device_code;
    (void)interval_secs;
    return HU_ERR_NOT_SUPPORTED; /* HTTP client required */
#endif
}

hu_error_t hu_auth_refresh_token(hu_allocator_t *alloc, const char *token_url,
                                 const char *client_id, const char *refresh_token,
                                 hu_oauth_token_t *token_out) {
    if (!alloc || !token_out)
        return HU_ERR_INVALID_ARGUMENT;
    (void)token_url;
    (void)client_id;
#if HU_IS_TEST
    if (token_out) {
        token_out->access_token = hu_strdup(alloc, "mock-refreshed-token");
        token_out->refresh_token = hu_strdup(alloc, refresh_token ? refresh_token : "mock-rt");
        token_out->expires_at = time(NULL) + 3600;
        token_out->token_type = hu_strdup(alloc, "Bearer");
    }
    return HU_OK;
#elif defined(HU_HTTP_CURL)
    char body[4096];
    size_t j = 0;
    if (j + 24 < sizeof(body))
        memcpy(body + j, "grant_type=refresh_token", 24), j += 24;
    if (j + 16 < sizeof(body))
        memcpy(body + j, "&refresh_token=", 15), j += 15;
    for (const char *p = refresh_token ? refresh_token : ""; *p && j < sizeof(body) - 4;) {
        if (url_encode_char(body, sizeof(body), &j, (unsigned char)*p) != 0)
            return HU_ERR_INVALID_ARGUMENT;
        p++;
    }
    if (j + 12 < sizeof(body))
        memcpy(body + j, "&client_id=", 10), j += 10;
    for (const char *p = client_id ? client_id : ""; *p && j < sizeof(body) - 4;) {
        if (url_encode_char(body, sizeof(body), &j, (unsigned char)*p) != 0)
            return HU_ERR_INVALID_ARGUMENT;
        p++;
    }
    body[j] = '\0';

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_request(
        alloc, token_url, "POST",
        "Content-Type: application/x-www-form-urlencoded\nUser-Agent: Human/1.0", body, j, &resp);
    if (err != HU_OK)
        return err;

    if (resp.status_code != 200) {
        hu_http_response_free(alloc, &resp);
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_json_value_t *root = NULL;
    err = hu_json_parse(alloc, resp.body, resp.body_len, &root);
    hu_http_response_free(alloc, &resp);
    if (err != HU_OK || !root)
        return err;

    const char *at = hu_json_get_string(root, "access_token");
    const char *rt = hu_json_get_string(root, "refresh_token");
    const char *tt = hu_json_get_string(root, "token_type");
    int64_t exp_in = (int64_t)hu_json_get_number(root, "expires_in", 3600);
    hu_json_free(alloc, root);

    if (!at || !at[0])
        return HU_ERR_INVALID_ARGUMENT;

    token_out->access_token = hu_strdup(alloc, at);
    token_out->refresh_token = (rt && rt[0]) ? hu_strdup(alloc, rt)
                               : (refresh_token && refresh_token[0])
                                   ? hu_strdup(alloc, refresh_token)
                                   : NULL;
    token_out->token_type = hu_strdup(alloc, tt && tt[0] ? tt : "Bearer");
    token_out->expires_at = time(NULL) + exp_in;

    if (!token_out->access_token || !token_out->token_type) {
        hu_oauth_token_deinit(token_out, alloc);
        return HU_ERR_OUT_OF_MEMORY;
    }
    return HU_OK;
#else
    (void)token_url;
    (void)client_id;
    (void)refresh_token;
    return HU_ERR_NOT_SUPPORTED; /* HTTP client required for token refresh */
#endif
}
