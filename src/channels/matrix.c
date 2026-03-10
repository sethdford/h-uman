#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MATRIX_SESSION_KEY_MAX 127
#define MATRIX_CONTENT_MAX     4095

typedef struct hu_matrix_ctx {
    hu_allocator_t *alloc;
    char *homeserver;
    size_t homeserver_len;
    char *access_token;
    size_t access_token_len;
    bool running;
    char *since_token;
    char *user_id;
#if HU_IS_TEST
    char last_message[4096];
    size_t last_message_len;
    struct {
        char session_key[128];
        char content[4096];
    } mock_msgs[8];
    size_t mock_count;
#endif
} hu_matrix_ctx_t;

static hu_error_t matrix_start(void *ctx) {
    hu_matrix_ctx_t *c = (hu_matrix_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
    return HU_OK;
}

static void matrix_stop(void *ctx) {
    hu_matrix_ctx_t *c = (hu_matrix_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static hu_error_t matrix_send(void *ctx, const char *target, size_t target_len, const char *message,
                              size_t message_len, const char *const *media, size_t media_count) {
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    (void)media;
    (void)media_count;
    hu_matrix_ctx_t *c = (hu_matrix_ctx_t *)ctx;

#if HU_IS_TEST
    {
        size_t len = message_len > 4095 ? 4095 : message_len;
        if (message && len > 0)
            memcpy(c->last_message, message, len);
        c->last_message[len] = '\0';
        c->last_message_len = len;
        return HU_OK;
    }
#else
    if (!c || !c->alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (!c->homeserver || c->homeserver_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    if (!c->access_token || c->access_token_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    if (!target || target_len == 0 || !message)
        return HU_ERR_INVALID_ARGUMENT;

    unsigned long txn_id = (unsigned long)time(NULL);
    char url_buf[1024];
    int n = snprintf(url_buf, sizeof(url_buf),
                     "%.*s/_matrix/client/r0/rooms/%.*s/send/m.room.message/%lu",
                     (int)c->homeserver_len, c->homeserver, (int)target_len, target, txn_id);
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return HU_ERR_INTERNAL;

    hu_json_buf_t jbuf;
    hu_error_t err = hu_json_buf_init(&jbuf, c->alloc);
    if (err)
        return err;

    err = hu_json_buf_append_raw(&jbuf, "{\"msgtype\":\"m.text\",", 20);
    if (err)
        goto jfail;
    err = hu_json_append_key_value(&jbuf, "body", 4, message, message_len);
    if (err)
        goto jfail;
    err = hu_json_buf_append_raw(&jbuf, "}", 1);
    if (err)
        goto jfail;

    char headers_buf[600];
    n = snprintf(headers_buf, sizeof(headers_buf),
                 "Authorization: Bearer %.*s\nContent-Type: application/json",
                 (int)c->access_token_len, c->access_token);
    if (n <= 0 || (size_t)n >= sizeof(headers_buf)) {
        hu_json_buf_free(&jbuf);
        return HU_ERR_INTERNAL;
    }

    hu_http_response_t resp = {0};
    err = hu_http_request(c->alloc, url_buf, "PUT", headers_buf, jbuf.ptr, jbuf.len, &resp);
    hu_json_buf_free(&jbuf);
    if (err != HU_OK) {
        if (resp.owned && resp.body)
            hu_http_response_free(c->alloc, &resp);
        return HU_ERR_CHANNEL_SEND;
    }
    if (resp.owned && resp.body)
        hu_http_response_free(c->alloc, &resp);
    if (resp.status_code < 200 || resp.status_code >= 300)
        return HU_ERR_CHANNEL_SEND;
    return HU_OK;
jfail:
    hu_json_buf_free(&jbuf);
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

static const hu_channel_vtable_t matrix_vtable = {
    .start = matrix_start,
    .stop = matrix_stop,
    .send = matrix_send,
    .name = matrix_name,
    .health_check = matrix_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

hu_error_t hu_matrix_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                          size_t max_msgs, size_t *out_count) {
    hu_matrix_ctx_t *ctx = (hu_matrix_ctx_t *)channel_ctx;
    if (!ctx || !msgs || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;
#if HU_IS_TEST
    if (ctx->mock_count > 0) {
        size_t n = ctx->mock_count < max_msgs ? ctx->mock_count : max_msgs;
        for (size_t i = 0; i < n; i++) {
            memcpy(msgs[i].session_key, ctx->mock_msgs[i].session_key, 128);
            memcpy(msgs[i].content, ctx->mock_msgs[i].content, 4096);
        }
        *out_count = n;
        ctx->mock_count = 0;
        return HU_OK;
    }
    return HU_OK;
#else
#if defined(HU_HTTP_CURL)
    if (!ctx->homeserver || ctx->homeserver_len == 0)
        return HU_OK;
    if (!ctx->access_token || ctx->access_token_len == 0)
        return HU_OK;
    if (!ctx->running)
        return HU_OK;
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
        return HU_ERR_INTERNAL;
    char auth_buf[512];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Authorization: Bearer %.*s",
                      (int)ctx->access_token_len, ctx->access_token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf))
        return HU_ERR_INTERNAL;
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(alloc, url_buf, auth_buf, &resp);
    if (err != HU_OK) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return HU_OK;
    }
    if (resp.status_code != 200 || !resp.body) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return HU_OK;
    }
    hu_json_value_t *parsed = NULL;
    err = hu_json_parse(alloc, resp.body, resp.body_len, &parsed);
    if (resp.owned && resp.body)
        hu_http_response_free(alloc, &resp);
    if (err != HU_OK || !parsed)
        return HU_OK;
    const char *next_batch = hu_json_get_string(parsed, "next_batch");
    if (next_batch) {
        if (ctx->since_token)
            ctx->alloc->free(ctx->alloc->ctx, ctx->since_token, strlen(ctx->since_token) + 1);
        ctx->since_token = hu_strndup(ctx->alloc, next_batch, strlen(next_batch));
    }
    size_t cnt = 0;
    hu_json_value_t *rooms = hu_json_object_get(parsed, "rooms");
    if (rooms) {
        hu_json_value_t *join = hu_json_object_get(rooms, "join");
        if (join && join->type == HU_JSON_OBJECT && join->data.object.pairs) {
            for (size_t r = 0; r < join->data.object.len && cnt < max_msgs; r++) {
                const char *room_id = join->data.object.pairs[r].key;
                hu_json_value_t *room = join->data.object.pairs[r].value;
                if (!room || room->type != HU_JSON_OBJECT)
                    continue;
                hu_json_value_t *tl = hu_json_object_get(room, "timeline");
                if (!tl)
                    continue;
                hu_json_value_t *events = hu_json_object_get(tl, "events");
                if (!events || events->type != HU_JSON_ARRAY)
                    continue;
                for (size_t e = 0; e < events->data.array.len && cnt < max_msgs; e++) {
                    hu_json_value_t *ev = events->data.array.items[e];
                    if (!ev || ev->type != HU_JSON_OBJECT)
                        continue;
                    const char *ev_type = hu_json_get_string(ev, "type");
                    if (!ev_type || strcmp(ev_type, "m.room.message") != 0)
                        continue;
                    const char *sender = hu_json_get_string(ev, "sender");
                    if (sender && ctx->user_id && strcmp(sender, ctx->user_id) == 0)
                        continue;
                    hu_json_value_t *content = hu_json_object_get(ev, "content");
                    if (!content)
                        continue;
                    const char *body = hu_json_get_string(content, "body");
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
    hu_json_free(alloc, parsed);
    *out_count = cnt;
    return HU_OK;
#else
    (void)alloc;
    (void)max_msgs;
    return HU_ERR_NOT_SUPPORTED;
#endif
#endif
}

hu_error_t hu_matrix_create(hu_allocator_t *alloc, const char *homeserver, size_t homeserver_len,
                            const char *access_token, size_t access_token_len, hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_matrix_ctx_t *c = (hu_matrix_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    if (homeserver && homeserver_len > 0) {
        c->homeserver = (char *)alloc->alloc(alloc->ctx, homeserver_len + 1);
        if (!c->homeserver) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
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
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->access_token, access_token, access_token_len);
        c->access_token[access_token_len] = '\0';
        c->access_token_len = access_token_len;
    }
    out->ctx = c;
    out->vtable = &matrix_vtable;
    return HU_OK;
}

void hu_matrix_destroy(hu_channel_t *ch) {
    if (ch && ch->ctx) {
        hu_matrix_ctx_t *c = (hu_matrix_ctx_t *)ch->ctx;
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

#if HU_IS_TEST
hu_error_t hu_matrix_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                      size_t session_key_len, const char *content,
                                      size_t content_len) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_matrix_ctx_t *c = (hu_matrix_ctx_t *)ch->ctx;
    if (c->mock_count >= 8)
        return HU_ERR_OUT_OF_MEMORY;
    size_t i = c->mock_count++;
    size_t sk = session_key_len > 127 ? 127 : session_key_len;
    size_t ct = content_len > 4095 ? 4095 : content_len;
    if (session_key && sk > 0)
        memcpy(c->mock_msgs[i].session_key, session_key, sk);
    c->mock_msgs[i].session_key[sk] = '\0';
    if (content && ct > 0)
        memcpy(c->mock_msgs[i].content, content, ct);
    c->mock_msgs[i].content[ct] = '\0';
    return HU_OK;
}
const char *hu_matrix_test_get_last_message(hu_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    hu_matrix_ctx_t *c = (hu_matrix_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif
