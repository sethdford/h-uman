#include "seaclaw/channel.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct sc_onebot_ctx {
    sc_allocator_t *alloc;
    char *api_base;
    size_t api_base_len;
    char *access_token;
    size_t access_token_len;
    bool running;
} sc_onebot_ctx_t;

static sc_error_t onebot_start(void *ctx) {
    sc_onebot_ctx_t *c = (sc_onebot_ctx_t *)ctx;
    if (!c) return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void onebot_stop(void *ctx) {
    sc_onebot_ctx_t *c = (sc_onebot_ctx_t *)ctx;
    if (c) c->running = false;
}

static sc_error_t onebot_send(void *ctx,
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
    sc_onebot_ctx_t *c = (sc_onebot_ctx_t *)ctx;
    if (!c || !c->alloc || !c->api_base || c->api_base_len == 0)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;
    if (!target || target_len == 0 || !message) return SC_ERR_INVALID_ARGUMENT;

    sc_json_buf_t jbuf;
    sc_error_t err = sc_json_buf_init(&jbuf, c->alloc);
    if (err) return err;

    /* OneBot 11: group message by default */
    err = sc_json_buf_append_raw(&jbuf, "{\"message_type\":\"group\",", 26);
    if (err) goto fail;
    err = sc_json_append_key_value(&jbuf, "group_id", 8, target, target_len);
    if (err) goto fail;
    err = sc_json_buf_append_raw(&jbuf, ",\"message\":", 11);
    if (err) goto fail;
    err = sc_json_append_string(&jbuf, message, message_len);
    if (err) goto fail;
    err = sc_json_buf_append_raw(&jbuf, "}", 1);
    if (err) goto fail;

    char url_buf[512];
    size_t base_len = c->api_base_len;
    int n = snprintf(url_buf, sizeof(url_buf), "%.*s%s/send_msg",
        (int)base_len, c->api_base,
        (base_len > 0 && c->api_base[base_len - 1] == '/') ? "" : "/");
    if (n < 0 || (size_t)n >= sizeof(url_buf)) { err = SC_ERR_INTERNAL; goto fail; }

    char *auth = NULL;
    if (c->access_token && c->access_token_len > 0) {
        auth = (char *)c->alloc->alloc(c->alloc->ctx, 8 + c->access_token_len + 1);
        if (!auth) { err = SC_ERR_OUT_OF_MEMORY; goto fail; }
        n = snprintf(auth, 8 + c->access_token_len + 1, "Bearer %.*s",
            (int)c->access_token_len, c->access_token);
        (void)n;
    }

    sc_http_response_t resp = {0};
    err = sc_http_post_json(c->alloc, url_buf, auth, jbuf.ptr, jbuf.len, &resp);
    if (auth) c->alloc->free(c->alloc->ctx, auth, 8 + c->access_token_len + 1);
    sc_json_buf_free(&jbuf);
    if (err) {
        if (resp.owned && resp.body) sc_http_response_free(c->alloc, &resp);
        return SC_ERR_CHANNEL_SEND;
    }
    if (resp.owned && resp.body) sc_http_response_free(c->alloc, &resp);
    if (resp.status_code < 200 || resp.status_code >= 300)
        return SC_ERR_CHANNEL_SEND;
    return SC_OK;
fail:
    sc_json_buf_free(&jbuf);
    return err;
#endif
}

static const char *onebot_name(void *ctx) { (void)ctx; return "onebot"; }
static bool onebot_health_check(void *ctx) { (void)ctx; return true; }

static const sc_channel_vtable_t onebot_vtable = {
    .start = onebot_start, .stop = onebot_stop, .send = onebot_send,
    .name = onebot_name, .health_check = onebot_health_check,
    .send_event = NULL, .start_typing = NULL, .stop_typing = NULL,
};

sc_error_t sc_onebot_create(sc_allocator_t *alloc,
    const char *api_base, size_t api_base_len,
    const char *access_token, size_t access_token_len,
    sc_channel_t *out)
{
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    sc_onebot_ctx_t *c = (sc_onebot_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    if (api_base && api_base_len > 0) {
        c->api_base = (char *)malloc(api_base_len + 1);
        if (!c->api_base) { free(c); return SC_ERR_OUT_OF_MEMORY; }
        memcpy(c->api_base, api_base, api_base_len);
        c->api_base[api_base_len] = '\0';
        c->api_base_len = api_base_len;
    }
    if (access_token && access_token_len > 0) {
        c->access_token = (char *)malloc(access_token_len + 1);
        if (!c->access_token) {
            if (c->api_base) free(c->api_base);
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->access_token, access_token, access_token_len);
        c->access_token[access_token_len] = '\0';
        c->access_token_len = access_token_len;
    }
    out->ctx = c;
    out->vtable = &onebot_vtable;
    return SC_OK;
}

void sc_onebot_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_onebot_ctx_t *c = (sc_onebot_ctx_t *)ch->ctx;
        if (c->api_base) free(c->api_base);
        if (c->access_token) free(c->access_token);
        free(c);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
