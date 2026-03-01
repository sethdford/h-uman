#include "seaclaw/channels/qq.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define QQ_API_BASE "https://api.sgroup.qq.com"
#define QQ_SANDBOX_BASE "https://sandbox.api.sgroup.qq.com"
#define QQ_MAX_MSG 4096

typedef struct sc_qq_ctx {
    sc_allocator_t *alloc;
    char *app_id;
    char *bot_token;
    bool sandbox;
    bool running;
} sc_qq_ctx_t;

static sc_error_t qq_start(void *ctx) {
    sc_qq_ctx_t *c = (sc_qq_ctx_t *)ctx;
    if (!c) return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void qq_stop(void *ctx) {
    sc_qq_ctx_t *c = (sc_qq_ctx_t *)ctx;
    if (c) c->running = false;
}

static sc_error_t qq_send(void *ctx,
    const char *target, size_t target_len,
    const char *message, size_t message_len,
    const char *const *media, size_t media_count)
{
    (void)media;
    (void)media_count;
    sc_qq_ctx_t *c = (sc_qq_ctx_t *)ctx;
    if (!c || !c->alloc) return SC_ERR_INVALID_ARGUMENT;
    if (!c->bot_token || !target || target_len == 0 || !message) return SC_ERR_INVALID_ARGUMENT;

#if SC_IS_TEST
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    (void)media;
    (void)media_count;
    return SC_OK;
#else
    const char *base = c->sandbox ? QQ_SANDBOX_BASE : QQ_API_BASE;
    char url_buf[512];
    int n = snprintf(url_buf, sizeof(url_buf), "%s/channels/%.*s/messages",
        base, (int)target_len, target);
    if (n < 0 || (size_t)n >= sizeof(url_buf)) return SC_ERR_INTERNAL;

    sc_json_buf_t jbuf;
    sc_error_t err = sc_json_buf_init(&jbuf, c->alloc);
    if (err) return err;
    err = sc_json_buf_append_raw(&jbuf, "{\"content\":", 11);
    if (err) goto jfail;
    err = sc_json_append_string(&jbuf, message, message_len);
    if (err) goto jfail;
    err = sc_json_buf_append_raw(&jbuf, "}", 1);
    if (err) goto jfail;

    char *body = (char *)c->alloc->alloc(c->alloc->ctx, jbuf.len + 1);
    if (!body) { err = SC_ERR_OUT_OF_MEMORY; goto jfail; }
    memcpy(body, jbuf.ptr, jbuf.len + 1);
    size_t body_len = jbuf.len;
    sc_json_buf_free(&jbuf);

    char auth_buf[256];
    int ab = snprintf(auth_buf, sizeof(auth_buf), "Authorization: Bot %s.%s",
        c->app_id ? c->app_id : "", c->bot_token);
    (void)ab;

    sc_http_response_t resp = {0};
    err = sc_http_post_json(c->alloc, url_buf, auth_buf, body, body_len, &resp);
    c->alloc->free(c->alloc->ctx, body, body_len + 1);
    if (err) {
        if (resp.owned && resp.body) sc_http_response_free(c->alloc, &resp);
        return SC_ERR_CHANNEL_SEND;
    }
    if (resp.owned && resp.body) sc_http_response_free(c->alloc, &resp);
    return SC_OK;
jfail:
    sc_json_buf_free(&jbuf);
    return err;
#endif
}

static const char *qq_name(void *ctx) { (void)ctx; return "qq"; }
static bool qq_health_check(void *ctx) {
    sc_qq_ctx_t *c = (sc_qq_ctx_t *)ctx;
    return c && c->running;
}

static const sc_channel_vtable_t qq_vtable = {
    .start = qq_start, .stop = qq_stop, .send = qq_send,
    .name = qq_name, .health_check = qq_health_check,
    .send_event = NULL, .start_typing = NULL, .stop_typing = NULL,
};

sc_error_t sc_qq_create(sc_allocator_t *alloc,
    const char *app_id, size_t app_id_len,
    const char *bot_token, size_t bot_token_len,
    bool sandbox,
    sc_channel_t *out)
{
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    sc_qq_ctx_t *c = (sc_qq_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    c->sandbox = sandbox;
    if (app_id && app_id_len > 0) {
        c->app_id = (char *)malloc(app_id_len + 1);
        if (!c->app_id) { free(c); return SC_ERR_OUT_OF_MEMORY; }
        memcpy(c->app_id, app_id, app_id_len);
        c->app_id[app_id_len] = '\0';
    }
    if (bot_token && bot_token_len > 0) {
        c->bot_token = (char *)malloc(bot_token_len + 1);
        if (!c->bot_token) {
            if (c->app_id) free(c->app_id);
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->bot_token, bot_token, bot_token_len);
        c->bot_token[bot_token_len] = '\0';
    }
    out->ctx = c;
    out->vtable = &qq_vtable;
    return SC_OK;
}

void sc_qq_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_qq_ctx_t *c = (sc_qq_ctx_t *)ch->ctx;
        if (c->app_id) free(c->app_id);
        if (c->bot_token) free(c->bot_token);
        free(c);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
