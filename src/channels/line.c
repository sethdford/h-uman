#include "seaclaw/channel.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define LINE_PUSH_URL "https://api.line.me/v2/bot/message/push"

typedef struct sc_line_ctx {
    sc_allocator_t *alloc;
    char *channel_token;
    size_t channel_token_len;
    bool running;
} sc_line_ctx_t;

static sc_error_t line_start(void *ctx) {
    sc_line_ctx_t *c = (sc_line_ctx_t *)ctx;
    if (!c) return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void line_stop(void *ctx) {
    sc_line_ctx_t *c = (sc_line_ctx_t *)ctx;
    if (c) c->running = false;
}

static sc_error_t line_send(void *ctx,
    const char *target, size_t target_len,
    const char *message, size_t message_len,
    const char *const *media, size_t media_count)
{
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    (void)media;
    (void)media_count;
    sc_line_ctx_t *c = (sc_line_ctx_t *)ctx;

#if SC_IS_TEST
    (void)c;
    return SC_OK;
#else
    if (!c || !c->alloc) return SC_ERR_INVALID_ARGUMENT;
    if (!c->channel_token || c->channel_token_len == 0) return SC_ERR_CHANNEL_NOT_CONFIGURED;
    if (!target || target_len == 0 || !message) return SC_ERR_INVALID_ARGUMENT;

    sc_json_buf_t jbuf;
    sc_error_t err = sc_json_buf_init(&jbuf, c->alloc);
    if (err) return err;

    err = sc_json_append_key_value(&jbuf, "to", 2, target, target_len);
    if (err) goto jfail;
    err = sc_json_buf_append_raw(&jbuf, ",\"messages\":[{\"type\":\"text\",", 30);
    if (err) goto jfail;
    err = sc_json_append_key_value(&jbuf, "text", 4, message, message_len);
    if (err) goto jfail;
    err = sc_json_buf_append_raw(&jbuf, "}]}", 3);
    if (err) goto jfail;

    char auth_buf[512];
    int n = snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)c->channel_token_len, c->channel_token);
    if (n <= 0 || (size_t)n >= sizeof(auth_buf)) {
        sc_json_buf_free(&jbuf);
        return SC_ERR_INTERNAL;
    }

    sc_http_response_t resp = {0};
    err = sc_http_post_json(c->alloc, LINE_PUSH_URL, auth_buf, jbuf.ptr, jbuf.len, &resp);
    sc_json_buf_free(&jbuf);
    if (err != SC_OK) {
        if (resp.owned && resp.body) sc_http_response_free(c->alloc, &resp);
        return SC_ERR_CHANNEL_SEND;
    }
    if (resp.owned && resp.body) sc_http_response_free(c->alloc, &resp);
    if (resp.status_code < 200 || resp.status_code >= 300)
        return SC_ERR_CHANNEL_SEND;
    return SC_OK;
jfail:
    sc_json_buf_free(&jbuf);
    return err;
#endif
}

static const char *line_name(void *ctx) { (void)ctx; return "line"; }
static bool line_health_check(void *ctx) { (void)ctx; return true; }

static const sc_channel_vtable_t line_vtable = {
    .start = line_start, .stop = line_stop, .send = line_send,
    .name = line_name, .health_check = line_health_check,
    .send_event = NULL, .start_typing = NULL, .stop_typing = NULL,
};

sc_error_t sc_line_create(sc_allocator_t *alloc,
    const char *channel_token, size_t channel_token_len,
    sc_channel_t *out)
{
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    sc_line_ctx_t *c = (sc_line_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    if (channel_token && channel_token_len > 0) {
        c->channel_token = (char *)malloc(channel_token_len + 1);
        if (!c->channel_token) { free(c); return SC_ERR_OUT_OF_MEMORY; }
        memcpy(c->channel_token, channel_token, channel_token_len);
        c->channel_token[channel_token_len] = '\0';
        c->channel_token_len = channel_token_len;
    }
    out->ctx = c;
    out->vtable = &line_vtable;
    return SC_OK;
}

void sc_line_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_line_ctx_t *c = (sc_line_ctx_t *)ch->ctx;
        if (c->channel_token) free(c->channel_token);
        free(c);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
