#include "seaclaw/channel.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct sc_mattermost_ctx {
    sc_allocator_t *alloc;
    char *url;
    size_t url_len;
    char *token;
    size_t token_len;
    bool running;
} sc_mattermost_ctx_t;

static sc_error_t mattermost_start(void *ctx) {
    sc_mattermost_ctx_t *c = (sc_mattermost_ctx_t *)ctx;
    if (!c) return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void mattermost_stop(void *ctx) {
    sc_mattermost_ctx_t *c = (sc_mattermost_ctx_t *)ctx;
    if (c) c->running = false;
}

static sc_error_t mattermost_send(void *ctx,
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
    sc_mattermost_ctx_t *c = (sc_mattermost_ctx_t *)ctx;

#if SC_IS_TEST
    (void)c;
    return SC_OK;
#else
    if (!c || !c->alloc) return SC_ERR_INVALID_ARGUMENT;
    if (!c->url || c->url_len == 0) return SC_ERR_CHANNEL_NOT_CONFIGURED;
    if (!c->token || c->token_len == 0) return SC_ERR_CHANNEL_NOT_CONFIGURED;
    if (!target || target_len == 0 || !message) return SC_ERR_INVALID_ARGUMENT;

    char url_buf[1024];
    int n = snprintf(url_buf, sizeof(url_buf), "%.*s/api/v4/posts",
        (int)c->url_len, c->url);
    if (n < 0 || (size_t)n >= sizeof(url_buf)) return SC_ERR_INTERNAL;

    sc_json_buf_t jbuf;
    sc_error_t err = sc_json_buf_init(&jbuf, c->alloc);
    if (err) return err;

    err = sc_json_append_key_value(&jbuf, "channel_id", 10, target, target_len);
    if (err) goto jfail;
    err = sc_json_buf_append_raw(&jbuf, ",", 1);
    if (err) goto jfail;
    err = sc_json_append_key_value(&jbuf, "message", 7, message, message_len);
    if (err) goto jfail;
    err = sc_json_buf_append_raw(&jbuf, "}", 1);
    if (err) goto jfail;

    char auth_buf[512];
    n = snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)c->token_len, c->token);
    if (n <= 0 || (size_t)n >= sizeof(auth_buf)) {
        sc_json_buf_free(&jbuf);
        return SC_ERR_INTERNAL;
    }

    sc_http_response_t resp = {0};
    err = sc_http_post_json(c->alloc, url_buf, auth_buf, jbuf.ptr, jbuf.len, &resp);
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

static const char *mattermost_name(void *ctx) { (void)ctx; return "mattermost"; }
static bool mattermost_health_check(void *ctx) { (void)ctx; return true; }

static const sc_channel_vtable_t mattermost_vtable = {
    .start = mattermost_start, .stop = mattermost_stop, .send = mattermost_send,
    .name = mattermost_name, .health_check = mattermost_health_check,
    .send_event = NULL, .start_typing = NULL, .stop_typing = NULL,
};

sc_error_t sc_mattermost_create(sc_allocator_t *alloc,
    const char *url, size_t url_len,
    const char *token, size_t token_len,
    sc_channel_t *out)
{
    (void)alloc;
    sc_mattermost_ctx_t *c = (sc_mattermost_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    if (url && url_len > 0) {
        c->url = (char *)malloc(url_len + 1);
        if (!c->url) { free(c); return SC_ERR_OUT_OF_MEMORY; }
        memcpy(c->url, url, url_len);
        c->url[url_len] = '\0';
        c->url_len = url_len;
    }
    if (token && token_len > 0) {
        c->token = (char *)malloc(token_len + 1);
        if (!c->token) {
            if (c->url) free(c->url);
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->token, token, token_len);
        c->token[token_len] = '\0';
        c->token_len = token_len;
    }
    out->ctx = c;
    out->vtable = &mattermost_vtable;
    return SC_OK;
}

void sc_mattermost_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_mattermost_ctx_t *c = (sc_mattermost_ctx_t *)ch->ctx;
        if (c->url) free(c->url);
        if (c->token) free(c->token);
        free(c);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
