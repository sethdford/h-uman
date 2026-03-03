#include "seaclaw/channel.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct sc_matrix_ctx {
    sc_allocator_t *alloc;
    char *homeserver;
    size_t homeserver_len;
    char *access_token;
    size_t access_token_len;
    bool running;
} sc_matrix_ctx_t;

static sc_error_t matrix_start(void *ctx) {
    sc_matrix_ctx_t *c = (sc_matrix_ctx_t *)ctx;
    if (!c)
        return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void matrix_stop(void *ctx) {
    sc_matrix_ctx_t *c = (sc_matrix_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static sc_error_t matrix_send(void *ctx, const char *target, size_t target_len, const char *message,
                              size_t message_len, const char *const *media, size_t media_count) {
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    (void)media;
    (void)media_count;
    sc_matrix_ctx_t *c = (sc_matrix_ctx_t *)ctx;

#if SC_IS_TEST
    (void)c;
    return SC_OK;
#else
    if (!c || !c->alloc)
        return SC_ERR_INVALID_ARGUMENT;
    if (!c->homeserver || c->homeserver_len == 0)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;
    if (!c->access_token || c->access_token_len == 0)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;
    if (!target || target_len == 0 || !message)
        return SC_ERR_INVALID_ARGUMENT;

    unsigned long txn_id = (unsigned long)time(NULL);
    char url_buf[1024];
    int n = snprintf(url_buf, sizeof(url_buf),
                     "%.*s/_matrix/client/r0/rooms/%.*s/send/m.room.message/%lu",
                     (int)c->homeserver_len, c->homeserver, (int)target_len, target, txn_id);
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return SC_ERR_INTERNAL;

    sc_json_buf_t jbuf;
    sc_error_t err = sc_json_buf_init(&jbuf, c->alloc);
    if (err)
        return err;

    err = sc_json_buf_append_raw(&jbuf, "{\"msgtype\":\"m.text\",", 20);
    if (err)
        goto jfail;
    err = sc_json_append_key_value(&jbuf, "body", 4, message, message_len);
    if (err)
        goto jfail;
    err = sc_json_buf_append_raw(&jbuf, "}", 1);
    if (err)
        goto jfail;

    char headers_buf[600];
    n = snprintf(headers_buf, sizeof(headers_buf),
                 "Authorization: Bearer %.*s\nContent-Type: application/json",
                 (int)c->access_token_len, c->access_token);
    if (n <= 0 || (size_t)n >= sizeof(headers_buf)) {
        sc_json_buf_free(&jbuf);
        return SC_ERR_INTERNAL;
    }

    sc_http_response_t resp = {0};
    err = sc_http_request(c->alloc, url_buf, "PUT", headers_buf, jbuf.ptr, jbuf.len, &resp);
    sc_json_buf_free(&jbuf);
    if (err != SC_OK) {
        if (resp.owned && resp.body)
            sc_http_response_free(c->alloc, &resp);
        return SC_ERR_CHANNEL_SEND;
    }
    if (resp.owned && resp.body)
        sc_http_response_free(c->alloc, &resp);
    if (resp.status_code < 200 || resp.status_code >= 300)
        return SC_ERR_CHANNEL_SEND;
    return SC_OK;
jfail:
    sc_json_buf_free(&jbuf);
    return err;
#endif
}

static const char *matrix_name(void *ctx) {
    (void)ctx;
    return "matrix";
}
static bool matrix_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static const sc_channel_vtable_t matrix_vtable = {
    .start = matrix_start,
    .stop = matrix_stop,
    .send = matrix_send,
    .name = matrix_name,
    .health_check = matrix_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

sc_error_t sc_matrix_create(sc_allocator_t *alloc, const char *homeserver, size_t homeserver_len,
                            const char *access_token, size_t access_token_len, sc_channel_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_matrix_ctx_t *c = (sc_matrix_ctx_t *)calloc(1, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    if (homeserver && homeserver_len > 0) {
        c->homeserver = (char *)malloc(homeserver_len + 1);
        if (!c->homeserver) {
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->homeserver, homeserver, homeserver_len);
        c->homeserver[homeserver_len] = '\0';
        c->homeserver_len = homeserver_len;
    }
    if (access_token && access_token_len > 0) {
        c->access_token = (char *)malloc(access_token_len + 1);
        if (!c->access_token) {
            if (c->homeserver)
                free(c->homeserver);
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->access_token, access_token, access_token_len);
        c->access_token[access_token_len] = '\0';
        c->access_token_len = access_token_len;
    }
    out->ctx = c;
    out->vtable = &matrix_vtable;
    return SC_OK;
}

void sc_matrix_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_matrix_ctx_t *c = (sc_matrix_ctx_t *)ch->ctx;
        if (c->homeserver)
            free(c->homeserver);
        if (c->access_token)
            free(c->access_token);
        free(c);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
