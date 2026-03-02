#include "seaclaw/channel.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct sc_lark_ctx {
    sc_allocator_t *alloc;
    char *app_id;
    size_t app_id_len;
    char *app_secret;
    size_t app_secret_len;
    bool running;
} sc_lark_ctx_t;

static sc_error_t lark_start(void *ctx) {
    sc_lark_ctx_t *c = (sc_lark_ctx_t *)ctx;
    if (!c) return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void lark_stop(void *ctx) {
    sc_lark_ctx_t *c = (sc_lark_ctx_t *)ctx;
    if (c) c->running = false;
}

static sc_error_t lark_send(void *ctx,
    const char *target, size_t target_len,
    const char *message, size_t message_len,
    const char *const *media, size_t media_count)
{
    (void)media;
    (void)media_count;
#if SC_IS_TEST
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    return SC_OK;
#else
    sc_lark_ctx_t *c = (sc_lark_ctx_t *)ctx;
    if (!c || !c->alloc) return SC_ERR_INVALID_ARGUMENT;
    if (!c->app_id || c->app_id_len == 0 || !c->app_secret || c->app_secret_len == 0)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;

    /* Step 1: Get tenant_access_token */
    sc_json_buf_t token_buf;
    sc_error_t err = sc_json_buf_init(&token_buf, c->alloc);
    if (err) return err;
    err = sc_json_buf_append_raw(&token_buf, "{", 1);
    if (err) goto tok_fail;
    err = sc_json_append_key_value(&token_buf, "app_id", 6, c->app_id, c->app_id_len);
    if (err) goto tok_fail;
    err = sc_json_buf_append_raw(&token_buf, ",", 1);
    if (err) goto tok_fail;
    err = sc_json_append_key_value(&token_buf, "app_secret", 10, c->app_secret, c->app_secret_len);
    if (err) goto tok_fail;
    err = sc_json_buf_append_raw(&token_buf, "}", 1);
    if (err) goto tok_fail;

    sc_http_response_t token_resp = {0};
    err = sc_http_post_json(c->alloc,
        "https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal",
        NULL, token_buf.ptr, token_buf.len, &token_resp);
    sc_json_buf_free(&token_buf);
    if (err) {
        if (token_resp.owned && token_resp.body)
            sc_http_response_free(c->alloc, &token_resp);
        return SC_ERR_CHANNEL_SEND;
    }

    if (!token_resp.body || token_resp.body_len == 0) {
        if (token_resp.owned && token_resp.body)
            sc_http_response_free(c->alloc, &token_resp);
        return SC_ERR_CHANNEL_SEND;
    }
    sc_json_value_t *parsed = NULL;
    err = sc_json_parse(c->alloc, token_resp.body, token_resp.body_len, &parsed);
    if (token_resp.owned && token_resp.body)
        sc_http_response_free(c->alloc, &token_resp);
    if (err || !parsed) return SC_ERR_CHANNEL_SEND;

    const char *tenant_token = sc_json_get_string(parsed, "tenant_access_token");
    sc_json_free(c->alloc, parsed);
    if (!tenant_token || !tenant_token[0]) return SC_ERR_CHANNEL_SEND;

    /* Step 2: Send message */
    char auth_buf[512];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Bearer %s", tenant_token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf)) return SC_ERR_CHANNEL_SEND;

    /* Build inner content: {"text":"<message>"} — then use as content string value */
    sc_json_buf_t inner_buf;
    err = sc_json_buf_init(&inner_buf, c->alloc);
    if (err) return err;
    err = sc_json_buf_append_raw(&inner_buf, "{", 1);
    if (err) goto inner_fail;
    err = sc_json_append_key_value(&inner_buf, "text", 4, message, message_len);
    if (err) goto inner_fail;
    err = sc_json_buf_append_raw(&inner_buf, "}", 1);
    if (err) goto inner_fail;

    sc_json_buf_t msg_buf;
    err = sc_json_buf_init(&msg_buf, c->alloc);
    if (err) goto inner_fail;
    err = sc_json_buf_append_raw(&msg_buf, "{", 1);
    if (err) goto msg_fail;
    err = sc_json_append_key_value(&msg_buf, "receive_id", 10, target, target_len);
    if (err) goto msg_fail;
    err = sc_json_buf_append_raw(&msg_buf, ",", 1);
    if (err) goto msg_fail;
    err = sc_json_append_key_value(&msg_buf, "msg_type", 8, "text", 4);
    if (err) goto msg_fail;
    err = sc_json_buf_append_raw(&msg_buf, ",", 1);
    if (err) goto msg_fail;
    err = sc_json_append_key_value(&msg_buf, "content", 7, inner_buf.ptr, inner_buf.len);
    if (err) goto msg_fail;
    err = sc_json_buf_append_raw(&msg_buf, "}", 1);
    if (err) goto msg_fail;
    sc_json_buf_free(&inner_buf);

    sc_http_response_t msg_resp = {0};
    err = sc_http_post_json(c->alloc,
        "https://open.feishu.cn/open-apis/im/v1/messages?receive_id_type=chat_id",
        auth_buf, msg_buf.ptr, msg_buf.len, &msg_resp);
    sc_json_buf_free(&msg_buf);
    if (err) {
        if (msg_resp.owned && msg_resp.body)
            sc_http_response_free(c->alloc, &msg_resp);
        return SC_ERR_CHANNEL_SEND;
    }
    if (msg_resp.owned && msg_resp.body)
        sc_http_response_free(c->alloc, &msg_resp);
    if (msg_resp.status_code != 200)
        return SC_ERR_CHANNEL_SEND;
    return SC_OK;
msg_fail:
    sc_json_buf_free(&msg_buf);
inner_fail:
    sc_json_buf_free(&inner_buf);
    return err;
tok_fail:
    sc_json_buf_free(&token_buf);
    return err;
#endif
}

static const char *lark_name(void *ctx) { (void)ctx; return "lark"; }
static bool lark_health_check(void *ctx) { (void)ctx; return true; }

static const sc_channel_vtable_t lark_vtable = {
    .start = lark_start, .stop = lark_stop, .send = lark_send,
    .name = lark_name, .health_check = lark_health_check,
    .send_event = NULL, .start_typing = NULL, .stop_typing = NULL,
};

sc_error_t sc_lark_create(sc_allocator_t *alloc,
    const char *app_id, size_t app_id_len,
    const char *app_secret, size_t app_secret_len,
    sc_channel_t *out)
{
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    sc_lark_ctx_t *c = (sc_lark_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    if (app_id && app_id_len > 0) {
        c->app_id = (char *)malloc(app_id_len + 1);
        if (!c->app_id) { free(c); return SC_ERR_OUT_OF_MEMORY; }
        memcpy(c->app_id, app_id, app_id_len);
        c->app_id[app_id_len] = '\0';
        c->app_id_len = app_id_len;
    }
    if (app_secret && app_secret_len > 0) {
        c->app_secret = (char *)malloc(app_secret_len + 1);
        if (!c->app_secret) {
            if (c->app_id) free(c->app_id);
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->app_secret, app_secret, app_secret_len);
        c->app_secret[app_secret_len] = '\0';
        c->app_secret_len = app_secret_len;
    }
    out->ctx = c;
    out->vtable = &lark_vtable;
    return SC_OK;
}

void sc_lark_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_lark_ctx_t *c = (sc_lark_ctx_t *)ch->ctx;
        if (c->app_id) free(c->app_id);
        if (c->app_secret) free(c->app_secret);
        free(c);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
