#include "seaclaw/channels/line.h"
#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_PUSH_URL "https://api.line.me/v2/bot/message/push"

#define LINE_QUEUE_MAX       32
#define LINE_SESSION_KEY_MAX 127
#define LINE_CONTENT_MAX     4095

typedef struct sc_line_queued_msg {
    char session_key[128];
    char content[4096];
} sc_line_queued_msg_t;

typedef struct sc_line_ctx {
    sc_allocator_t *alloc;
    char *channel_token;
    size_t channel_token_len;
    char *user_id;
    size_t user_id_len;
    bool running;
    sc_line_queued_msg_t queue[LINE_QUEUE_MAX];
    size_t queue_head;
    size_t queue_tail;
    size_t queue_count;
} sc_line_ctx_t;

static sc_error_t line_start(void *ctx) {
    sc_line_ctx_t *c = (sc_line_ctx_t *)ctx;
    if (!c)
        return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void line_stop(void *ctx) {
    sc_line_ctx_t *c = (sc_line_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static void line_queue_push(sc_line_ctx_t *c, const char *from, size_t from_len, const char *body,
                            size_t body_len) {
    if (c->queue_count >= LINE_QUEUE_MAX)
        return;
    sc_line_queued_msg_t *slot = &c->queue[c->queue_tail];
    size_t sk = from_len < LINE_SESSION_KEY_MAX ? from_len : LINE_SESSION_KEY_MAX;
    memcpy(slot->session_key, from, sk);
    slot->session_key[sk] = '\0';
    size_t ct = body_len < LINE_CONTENT_MAX ? body_len : LINE_CONTENT_MAX;
    memcpy(slot->content, body, ct);
    slot->content[ct] = '\0';
    c->queue_tail = (c->queue_tail + 1) % LINE_QUEUE_MAX;
    c->queue_count++;
}

static sc_error_t line_send(void *ctx, const char *target, size_t target_len, const char *message,
                            size_t message_len, const char *const *media, size_t media_count) {
    sc_line_ctx_t *c = (sc_line_ctx_t *)ctx;

#if SC_IS_TEST
    (void)media;
    (void)media_count;
    if (!c || !message)
        return SC_ERR_INVALID_ARGUMENT;
    if (!c->channel_token || c->channel_token_len == 0)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;
    if ((!target || target_len == 0) && (!c->user_id || c->user_id_len == 0))
        return SC_ERR_CHANNEL_NOT_CONFIGURED;
    line_queue_push(c, "test-sender", 11, message, message_len);
    return SC_OK;
#else
    const char *to = (target && target_len > 0) ? target : c->user_id;
    size_t to_len = (target && target_len > 0) ? target_len : c->user_id_len;

    if (!c || !c->alloc)
        return SC_ERR_INVALID_ARGUMENT;
    if (!c->channel_token || c->channel_token_len == 0)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;
    if (!to || to_len == 0 || !message)
        return SC_ERR_INVALID_ARGUMENT;

    sc_json_buf_t jbuf;
    sc_error_t err = sc_json_buf_init(&jbuf, c->alloc);
    if (err)
        return err;

    err = sc_json_append_key_value(&jbuf, "to", 2, to, to_len);
    if (err)
        goto jfail;
    err = sc_json_buf_append_raw(&jbuf, ",\"messages\":[{\"type\":\"text\",", 28);
    if (err)
        goto jfail;
    err = sc_json_append_key_value(&jbuf, "text", 4, message, message_len);
    if (err)
        goto jfail;
    for (size_t i = 0; i < media_count && media && media[i]; i++) {
        size_t url_len = strlen(media[i]);
        err = sc_json_buf_append_raw(&jbuf, "},{\"type\":\"image\",", 17);
        if (err)
            goto jfail;
        err = sc_json_append_key_value(&jbuf, "originalContentUrl", 17, media[i], url_len);
        if (err)
            goto jfail;
        err = sc_json_buf_append_raw(&jbuf, ",", 1);
        if (err)
            goto jfail;
        err = sc_json_append_key_value(&jbuf, "previewImageUrl", 14, media[i], url_len);
        if (err)
            goto jfail;
    }
    err = sc_json_buf_append_raw(&jbuf, media_count > 0 ? "}}]}" : "}]}", media_count > 0 ? 4 : 3);
    if (err)
        goto jfail;

    char auth_buf[512];
    int n = snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)c->channel_token_len,
                     c->channel_token);
    if (n <= 0 || (size_t)n >= sizeof(auth_buf)) {
        sc_json_buf_free(&jbuf);
        return SC_ERR_INTERNAL;
    }

    sc_http_response_t resp = {0};
    err = sc_http_post_json(c->alloc, LINE_PUSH_URL, auth_buf, jbuf.ptr, jbuf.len, &resp);
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

static const char *line_name(void *ctx) {
    (void)ctx;
    return "line";
}
static bool line_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static const sc_channel_vtable_t line_vtable = {
    .start = line_start,
    .stop = line_stop,
    .send = line_send,
    .name = line_name,
    .health_check = line_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

sc_error_t sc_line_on_webhook(void *channel_ctx, sc_allocator_t *alloc, const char *body,
                              size_t body_len) {
    sc_line_ctx_t *c = (sc_line_ctx_t *)channel_ctx;
    if (!c || !body || body_len == 0)
        return SC_ERR_INVALID_ARGUMENT;
#if SC_IS_TEST
    (void)alloc;
    line_queue_push(c, "test-sender", 11, body, body_len);
    return SC_OK;
#else
    sc_json_value_t *parsed = NULL;
    sc_error_t err = sc_json_parse(alloc, body, body_len, &parsed);
    if (err != SC_OK || !parsed)
        return SC_OK;
    sc_json_value_t *events = sc_json_object_get(parsed, "events");
    if (events && events->type == SC_JSON_ARRAY) {
        for (size_t i = 0; i < events->data.array.len; i++) {
            sc_json_value_t *ev = events->data.array.items[i];
            if (!ev || ev->type != SC_JSON_OBJECT)
                continue;
            const char *ev_type = sc_json_get_string(ev, "type");
            if (!ev_type || strcmp(ev_type, "message") != 0)
                continue;
            sc_json_value_t *source = sc_json_object_get(ev, "source");
            const char *user_id = source ? sc_json_get_string(source, "userId") : NULL;
            if (!user_id)
                continue;
            sc_json_value_t *msg = sc_json_object_get(ev, "message");
            if (!msg || msg->type != SC_JSON_OBJECT)
                continue;
            const char *msg_type = sc_json_get_string(msg, "type");
            if (!msg_type || strcmp(msg_type, "text") != 0)
                continue;
            const char *text = sc_json_get_string(msg, "text");
            if (text && strlen(text) > 0)
                line_queue_push(c, user_id, strlen(user_id), text, strlen(text));
        }
    }
    sc_json_free(alloc, parsed);
    return SC_OK;
#endif
}

sc_error_t sc_line_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                        size_t max_msgs, size_t *out_count) {
    (void)alloc;
    sc_line_ctx_t *c = (sc_line_ctx_t *)channel_ctx;
    if (!c || !msgs || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    size_t cnt = 0;
    while (c->queue_count > 0 && cnt < max_msgs) {
        sc_line_queued_msg_t *slot = &c->queue[c->queue_head];
        memcpy(msgs[cnt].session_key, slot->session_key, sizeof(slot->session_key));
        memcpy(msgs[cnt].content, slot->content, sizeof(slot->content));
        c->queue_head = (c->queue_head + 1) % LINE_QUEUE_MAX;
        c->queue_count--;
        cnt++;
    }
    *out_count = cnt;
    return SC_OK;
}

sc_error_t sc_line_create(sc_allocator_t *alloc, const char *channel_token,
                          size_t channel_token_len, sc_channel_t *out) {
    return sc_line_create_ex(alloc, channel_token, channel_token_len, NULL, 0, out);
}

sc_error_t sc_line_create_ex(sc_allocator_t *alloc, const char *channel_token,
                             size_t channel_token_len, const char *user_id, size_t user_id_len,
                             sc_channel_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_line_ctx_t *c = (sc_line_ctx_t *)calloc(1, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    if (channel_token && channel_token_len > 0) {
        c->channel_token = (char *)malloc(channel_token_len + 1);
        if (!c->channel_token) {
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->channel_token, channel_token, channel_token_len);
        c->channel_token[channel_token_len] = '\0';
        c->channel_token_len = channel_token_len;
    }
    if (user_id && user_id_len > 0) {
        c->user_id = (char *)malloc(user_id_len + 1);
        if (!c->user_id) {
            if (c->channel_token)
                free(c->channel_token);
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->user_id, user_id, user_id_len);
        c->user_id[user_id_len] = '\0';
        c->user_id_len = user_id_len;
    }
    out->ctx = c;
    out->vtable = &line_vtable;
    return SC_OK;
}

bool sc_line_is_configured(sc_channel_t *ch) {
    if (!ch || !ch->ctx)
        return false;
    sc_line_ctx_t *c = (sc_line_ctx_t *)ch->ctx;
    return c->channel_token != NULL && c->channel_token_len > 0 &&
           (c->user_id != NULL && c->user_id_len > 0);
}

void sc_line_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_line_ctx_t *c = (sc_line_ctx_t *)ch->ctx;
        if (c->channel_token)
            free(c->channel_token);
        if (c->user_id)
            free(c->user_id);
        free(c);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
