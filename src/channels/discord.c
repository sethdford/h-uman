#include "seaclaw/channel.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define DISCORD_API_BASE "https://discord.com/api/v10/channels"

typedef struct sc_discord_ctx {
    sc_allocator_t *alloc;
    char *token;
    size_t token_len;
    bool running;
} sc_discord_ctx_t;

#if !SC_IS_TEST
static sc_error_t build_discord_body(sc_allocator_t *alloc,
    const char *content, size_t content_len,
    char **out, size_t *out_len)
{
    sc_json_buf_t jbuf;
    sc_error_t err = sc_json_buf_init(&jbuf, alloc);
    if (err) return err;
    err = sc_json_buf_append_raw(&jbuf, "{\"content\":", 11);
    if (err) goto fail;
    err = sc_json_append_string(&jbuf, content, content_len);
    if (err) goto fail;
    err = sc_json_buf_append_raw(&jbuf, "}", 1);
    if (err) goto fail;
    *out_len = jbuf.len;
    *out = (char *)alloc->alloc(alloc->ctx, jbuf.len + 1);
    if (!*out) { err = SC_ERR_OUT_OF_MEMORY; goto fail; }
    memcpy(*out, jbuf.ptr, jbuf.len + 1);
    sc_json_buf_free(&jbuf);
    return SC_OK;
fail:
    sc_json_buf_free(&jbuf);
    return err;
}
#endif

static sc_error_t discord_start(void *ctx) {
    sc_discord_ctx_t *c = (sc_discord_ctx_t *)ctx;
    if (!c) return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void discord_stop(void *ctx) {
    sc_discord_ctx_t *c = (sc_discord_ctx_t *)ctx;
    if (c) c->running = false;
}

static sc_error_t discord_send(void *ctx,
    const char *target, size_t target_len,
    const char *message, size_t message_len,
    const char *const *media, size_t media_count)
{
    (void)media;
    (void)media_count;
    sc_discord_ctx_t *c = (sc_discord_ctx_t *)ctx;
    if (!c || !c->alloc) return SC_ERR_INVALID_ARGUMENT;
    if (!c->token || c->token_len == 0) return SC_ERR_CHANNEL_NOT_CONFIGURED;
    if (!target || target_len == 0 || !message) return SC_ERR_INVALID_ARGUMENT;

#if SC_IS_TEST
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    (void)media;
    (void)media_count;
    return SC_OK;
#else
    char url_buf[512];
    int n = snprintf(url_buf, sizeof(url_buf), "%s/%.*s/messages",
        DISCORD_API_BASE, (int)target_len, target);
    if (n < 0 || (size_t)n >= sizeof(url_buf)) return SC_ERR_INTERNAL;

    char *body = NULL;
    size_t body_len = 0;
    sc_error_t err = build_discord_body(c->alloc, message, message_len,
        &body, &body_len);
    if (err) return err;

    char auth_buf[256];
    n = snprintf(auth_buf, sizeof(auth_buf), "Authorization: Bot %.*s",
        (int)c->token_len, c->token);
    if (n <= 0 || (size_t)n >= sizeof(auth_buf)) {
        if (body) c->alloc->free(c->alloc->ctx, body, body_len + 1);
        return SC_ERR_INTERNAL;
    }

    sc_http_response_t resp = {0};
    err = sc_http_post_json(c->alloc, url_buf, auth_buf, body, body_len, &resp);
    if (body) c->alloc->free(c->alloc->ctx, body, body_len + 1);
    if (err != SC_OK) {
        if (resp.owned && resp.body) sc_http_response_free(c->alloc, &resp);
        return SC_ERR_CHANNEL_SEND;
    }
    if (resp.owned && resp.body) sc_http_response_free(c->alloc, &resp);
    if (resp.status_code < 200 || resp.status_code >= 300)
        return SC_ERR_CHANNEL_SEND;
    return SC_OK;
#endif
}

static const char *discord_name(void *ctx) { (void)ctx; return "discord"; }
static bool discord_health_check(void *ctx) { (void)ctx; return true; }

static const sc_channel_vtable_t discord_vtable = {
    .start = discord_start, .stop = discord_stop, .send = discord_send,
    .name = discord_name, .health_check = discord_health_check,
    .send_event = NULL, .start_typing = NULL, .stop_typing = NULL,
};

sc_error_t sc_discord_create(sc_allocator_t *alloc,
    const char *token, size_t token_len,
    sc_channel_t *out)
{
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    sc_discord_ctx_t *c = (sc_discord_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    if (token && token_len > 0) {
        c->token = (char *)malloc(token_len + 1);
        if (!c->token) { free(c); return SC_ERR_OUT_OF_MEMORY; }
        memcpy(c->token, token, token_len);
        c->token[token_len] = '\0';
        c->token_len = token_len;
    }
    out->ctx = c;
    out->vtable = &discord_vtable;
    return SC_OK;
}

void sc_discord_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_discord_ctx_t *c = (sc_discord_ctx_t *)ch->ctx;
        if (c->token) free(c->token);
        free(c);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
