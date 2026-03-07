#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MATRIX_SESSION_KEY_MAX 127
#define MATRIX_CONTENT_MAX     4095

typedef struct sc_matrix_ctx {
    sc_allocator_t *alloc;
    char *homeserver;
    size_t homeserver_len;
    char *access_token;
    size_t access_token_len;
    bool running;
    char *since_token;
    char *user_id;
#if SC_IS_TEST
    char last_message[4096];
    size_t last_message_len;
    struct {
        char session_key[128];
        char content[4096];
    } mock_msgs[8];
    size_t mock_count;
#endif
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
    {
        size_t len = message_len > 4095 ? 4095 : message_len;
        if (message && len > 0)
            memcpy(c->last_message, message, len);
        c->last_message[len] = '\0';
        c->last_message_len = len;
        return SC_OK;
    }
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

sc_error_t sc_matrix_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                          size_t max_msgs, size_t *out_count) {
    sc_matrix_ctx_t *ctx = (sc_matrix_ctx_t *)channel_ctx;
    if (!ctx || !msgs || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out_count = 0;
#if SC_IS_TEST
    {
        size_t n = ctx->mock_count < max_msgs ? ctx->mock_count : max_msgs;
        for (size_t i = 0; i < n; i++) {
            memcpy(msgs[i].session_key, ctx->mock_msgs[i].session_key, 128);
            memcpy(msgs[i].content, ctx->mock_msgs[i].content, 4096);
        }
        *out_count = n;
        ctx->mock_count = 0;
        return SC_OK;
    }
#else
#if defined(SC_HTTP_CURL)
    if (!ctx->homeserver || ctx->homeserver_len == 0)
        return SC_OK;
    if (!ctx->access_token || ctx->access_token_len == 0)
        return SC_OK;
    if (!ctx->running)
        return SC_OK;
    char url_buf[1024];
    int nu;
    if (ctx->since_token)
        nu = snprintf(url_buf, sizeof(url_buf),
                      "%.*s/_matrix/client/v3/"
                      "sync?timeout=5000&since=%s&filter={\"room\":{\"timeline\":{\"limit\":10}}}",
                      (int)ctx->homeserver_len, ctx->homeserver, ctx->since_token);
    else
        nu = snprintf(
            url_buf, sizeof(url_buf),
            "%.*s/_matrix/client/v3/sync?timeout=0&filter={\"room\":{\"timeline\":{\"limit\":10}}}",
            (int)ctx->homeserver_len, ctx->homeserver);
    if (nu < 0 || (size_t)nu >= sizeof(url_buf))
        return SC_ERR_INTERNAL;
    char auth_buf[512];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Authorization: Bearer %.*s",
                      (int)ctx->access_token_len, ctx->access_token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf))
        return SC_ERR_INTERNAL;
    sc_http_response_t resp = {0};
    sc_error_t err = sc_http_get(alloc, url_buf, auth_buf, &resp);
    if (err != SC_OK) {
        if (resp.owned && resp.body)
            sc_http_response_free(alloc, &resp);
        return SC_OK;
    }
    if (resp.status_code != 200 || !resp.body) {
        if (resp.owned && resp.body)
            sc_http_response_free(alloc, &resp);
        return SC_OK;
    }
    sc_json_value_t *parsed = NULL;
    err = sc_json_parse(alloc, resp.body, resp.body_len, &parsed);
    if (resp.owned && resp.body)
        sc_http_response_free(alloc, &resp);
    if (err != SC_OK || !parsed)
        return SC_OK;
    const char *next_batch = sc_json_get_string(parsed, "next_batch");
    if (next_batch) {
        if (ctx->since_token)
            ctx->alloc->free(ctx->alloc->ctx, ctx->since_token, strlen(ctx->since_token) + 1);
        ctx->since_token = sc_strndup(ctx->alloc, next_batch, strlen(next_batch));
    }
    size_t cnt = 0;
    sc_json_value_t *rooms = sc_json_object_get(parsed, "rooms");
    if (rooms) {
        sc_json_value_t *join = sc_json_object_get(rooms, "join");
        if (join && join->type == SC_JSON_OBJECT && join->data.object.pairs) {
            for (size_t r = 0; r < join->data.object.len && cnt < max_msgs; r++) {
                const char *room_id = join->data.object.pairs[r].key;
                sc_json_value_t *room = join->data.object.pairs[r].value;
                if (!room || room->type != SC_JSON_OBJECT)
                    continue;
                sc_json_value_t *tl = sc_json_object_get(room, "timeline");
                if (!tl)
                    continue;
                sc_json_value_t *events = sc_json_object_get(tl, "events");
                if (!events || events->type != SC_JSON_ARRAY)
                    continue;
                for (size_t e = 0; e < events->data.array.len && cnt < max_msgs; e++) {
                    sc_json_value_t *ev = events->data.array.items[e];
                    if (!ev || ev->type != SC_JSON_OBJECT)
                        continue;
                    const char *ev_type = sc_json_get_string(ev, "type");
                    if (!ev_type || strcmp(ev_type, "m.room.message") != 0)
                        continue;
                    const char *sender = sc_json_get_string(ev, "sender");
                    if (sender && ctx->user_id && strcmp(sender, ctx->user_id) == 0)
                        continue;
                    sc_json_value_t *content = sc_json_object_get(ev, "content");
                    if (!content)
                        continue;
                    const char *body = sc_json_get_string(content, "body");
                    if (!body || strlen(body) == 0)
                        continue;
                    size_t sk_len = room_id ? strlen(room_id) : 0;
                    if (sk_len > MATRIX_SESSION_KEY_MAX)
                        sk_len = MATRIX_SESSION_KEY_MAX;
                    if (room_id)
                        memcpy(msgs[cnt].session_key, room_id, sk_len);
                    msgs[cnt].session_key[sk_len] = '\0';
                    size_t ct_len = strlen(body);
                    if (ct_len > MATRIX_CONTENT_MAX)
                        ct_len = MATRIX_CONTENT_MAX;
                    memcpy(msgs[cnt].content, body, ct_len);
                    msgs[cnt].content[ct_len] = '\0';
                    cnt++;
                }
            }
        }
    }
    sc_json_free(alloc, parsed);
    *out_count = cnt;
    return SC_OK;
#else
    (void)alloc;
    (void)max_msgs;
    return SC_ERR_NOT_SUPPORTED;
#endif
#endif
}

sc_error_t sc_matrix_create(sc_allocator_t *alloc, const char *homeserver, size_t homeserver_len,
                            const char *access_token, size_t access_token_len, sc_channel_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_matrix_ctx_t *c = (sc_matrix_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    if (homeserver && homeserver_len > 0) {
        c->homeserver = (char *)alloc->alloc(alloc->ctx, homeserver_len + 1);
        if (!c->homeserver) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->homeserver, homeserver, homeserver_len);
        c->homeserver[homeserver_len] = '\0';
        c->homeserver_len = homeserver_len;
    }
    if (access_token && access_token_len > 0) {
        c->access_token = (char *)alloc->alloc(alloc->ctx, access_token_len + 1);
        if (!c->access_token) {
            if (c->homeserver)
                alloc->free(alloc->ctx, c->homeserver, homeserver_len + 1);
            alloc->free(alloc->ctx, c, sizeof(*c));
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
        if (c->alloc) {
            if (c->homeserver)
                c->alloc->free(c->alloc->ctx, c->homeserver, c->homeserver_len + 1);
            if (c->access_token)
                c->alloc->free(c->alloc->ctx, c->access_token, c->access_token_len + 1);
            if (c->since_token)
                c->alloc->free(c->alloc->ctx, c->since_token, strlen(c->since_token) + 1);
            if (c->user_id)
                c->alloc->free(c->alloc->ctx, c->user_id, strlen(c->user_id) + 1);
            c->alloc->free(c->alloc->ctx, c, sizeof(*c));
        }
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

#if SC_IS_TEST
sc_error_t sc_matrix_test_inject_mock(sc_channel_t *ch, const char *session_key,
                                      size_t session_key_len, const char *content,
                                      size_t content_len) {
    if (!ch || !ch->ctx)
        return SC_ERR_INVALID_ARGUMENT;
    sc_matrix_ctx_t *c = (sc_matrix_ctx_t *)ch->ctx;
    if (c->mock_count >= 8)
        return SC_ERR_OUT_OF_MEMORY;
    size_t i = c->mock_count++;
    size_t sk = session_key_len > 127 ? 127 : session_key_len;
    size_t ct = content_len > 4095 ? 4095 : content_len;
    if (session_key && sk > 0)
        memcpy(c->mock_msgs[i].session_key, session_key, sk);
    c->mock_msgs[i].session_key[sk] = '\0';
    if (content && ct > 0)
        memcpy(c->mock_msgs[i].content, content, ct);
    c->mock_msgs[i].content[ct] = '\0';
    return SC_OK;
}
const char *sc_matrix_test_get_last_message(sc_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    sc_matrix_ctx_t *c = (sc_matrix_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif
