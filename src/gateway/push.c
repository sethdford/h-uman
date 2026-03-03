#include "seaclaw/gateway/push.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <string.h>

#define SC_PUSH_INITIAL_CAP 4
#define SC_FCM_URL          "https://fcm.googleapis.com/fcm/send"

#ifndef SC_IS_TEST
static const char *fcm_url(const sc_push_config_t *config) {
    if (config->endpoint && config->endpoint[0])
        return config->endpoint;
    return SC_FCM_URL;
}
#endif

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
        return SC_ERR_NOT_SUPPORTED;
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
