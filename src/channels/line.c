#include "human/channels/line.h"
#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_PUSH_URL "https://api.line.me/v2/bot/message/push"

#define LINE_QUEUE_MAX       32
#define LINE_SESSION_KEY_MAX 127
#define LINE_CONTENT_MAX     4095

typedef struct hu_line_queued_msg {
    char session_key[128];
    char content[4096];
} hu_line_queued_msg_t;

typedef struct hu_line_ctx {
    hu_allocator_t *alloc;
    char *channel_token;
    size_t channel_token_len;
    char *user_id;
    size_t user_id_len;
    bool running;
    hu_line_queued_msg_t queue[LINE_QUEUE_MAX];
    size_t queue_head;
    size_t queue_tail;
    size_t queue_count;
#if HU_IS_TEST
    char last_message[4096];
    size_t last_message_len;
    struct {
        char session_key[128];
        char content[4096];
    } mock_msgs[8];
    size_t mock_count;
#endif
} hu_line_ctx_t;

static hu_error_t line_start(void *ctx) {
    hu_line_ctx_t *c = (hu_line_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
    return HU_OK;
}

static void line_stop(void *ctx) {
    hu_line_ctx_t *c = (hu_line_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static void line_queue_push(hu_line_ctx_t *c, const char *from, size_t from_len, const char *body,
                            size_t body_len) {
    if (c->queue_count >= LINE_QUEUE_MAX)
        return;
    hu_line_queued_msg_t *slot = &c->queue[c->queue_tail];
    size_t sk = from_len < LINE_SESSION_KEY_MAX ? from_len : LINE_SESSION_KEY_MAX;
    memcpy(slot->session_key, from, sk);
    slot->session_key[sk] = '\0';
    size_t ct = body_len < LINE_CONTENT_MAX ? body_len : LINE_CONTENT_MAX;
    memcpy(slot->content, body, ct);
    slot->content[ct] = '\0';
    c->queue_tail = (c->queue_tail + 1) % LINE_QUEUE_MAX;
    c->queue_count++;
}

static hu_error_t line_send(void *ctx, const char *target, size_t target_len, const char *message,
                            size_t message_len, const char *const *media, size_t media_count) {
    hu_line_ctx_t *c = (hu_line_ctx_t *)ctx;

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
    const char *to = (target && target_len > 0) ? target : c->user_id;
    size_t to_len = (target && target_len > 0) ? target_len : c->user_id_len;

    if (!c || !c->alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (!c->channel_token || c->channel_token_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    if (!to || to_len == 0 || !message)
        return HU_ERR_INVALID_ARGUMENT;

    hu_json_buf_t jbuf;
    hu_error_t err = hu_json_buf_init(&jbuf, c->alloc);
    if (err)
        return err;

    err = hu_json_append_key_value(&jbuf, "to", 2, to, to_len);
    if (err)
        goto jfail;
    err = hu_json_buf_append_raw(&jbuf, ",\"messages\":[{\"type\":\"text\",", 28);
    if (err)
        goto jfail;
    err = hu_json_append_key_value(&jbuf, "text", 4, message, message_len);
    if (err)
        goto jfail;
    for (size_t i = 0; i < media_count && media && media[i]; i++) {
        size_t url_len = strlen(media[i]);
        err = hu_json_buf_append_raw(&jbuf, "},{\"type\":\"image\",", 17);
        if (err)
            goto jfail;
        err = hu_json_append_key_value(&jbuf, "originalContentUrl", 17, media[i], url_len);
        if (err)
            goto jfail;
        err = hu_json_buf_append_raw(&jbuf, ",", 1);
        if (err)
            goto jfail;
        err = hu_json_append_key_value(&jbuf, "previewImageUrl", 14, media[i], url_len);
        if (err)
            goto jfail;
    }
    err = hu_json_buf_append_raw(&jbuf, media_count > 0 ? "}}]}" : "}]}", media_count > 0 ? 4 : 3);
    if (err)
        goto jfail;

    char auth_buf[512];
    int n = snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)c->channel_token_len,
                     c->channel_token);
    if (n <= 0 || (size_t)n >= sizeof(auth_buf)) {
        hu_json_buf_free(&jbuf);
        return HU_ERR_INTERNAL;
    }

    hu_http_response_t resp = {0};
    err = hu_http_post_json(c->alloc, LINE_PUSH_URL, auth_buf, jbuf.ptr, jbuf.len, &resp);
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

static const char *line_name(void *ctx) {
    (void)ctx;
    return "line";
}
static bool line_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static const hu_channel_vtable_t line_vtable = {
    .start = line_start,
    .stop = line_stop,
    .send = line_send,
    .name = line_name,
    .health_check = line_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

hu_error_t hu_line_on_webhook(void *channel_ctx, hu_allocator_t *alloc, const char *body,
                              size_t body_len) {
    hu_line_ctx_t *c = (hu_line_ctx_t *)channel_ctx;
    if (!c || !body || body_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    (void)alloc;
    line_queue_push(c, "test-sender", 11, body, body_len);
    return HU_OK;
#else
    hu_json_value_t *parsed = NULL;
    hu_error_t err = hu_json_parse(alloc, body, body_len, &parsed);
    if (err != HU_OK || !parsed)
        return HU_OK;
    hu_json_value_t *events = hu_json_object_get(parsed, "events");
    if (events && events->type == HU_JSON_ARRAY) {
        for (size_t i = 0; i < events->data.array.len; i++) {
            hu_json_value_t *ev = events->data.array.items[i];
            if (!ev || ev->type != HU_JSON_OBJECT)
                continue;
            const char *ev_type = hu_json_get_string(ev, "type");
            if (!ev_type || strcmp(ev_type, "message") != 0)
                continue;
            hu_json_value_t *source = hu_json_object_get(ev, "source");
            const char *user_id = source ? hu_json_get_string(source, "userId") : NULL;
            if (!user_id)
                continue;
            hu_json_value_t *msg = hu_json_object_get(ev, "message");
            if (!msg || msg->type != HU_JSON_OBJECT)
                continue;
            const char *msg_type = hu_json_get_string(msg, "type");
            if (!msg_type || strcmp(msg_type, "text") != 0)
                continue;
            const char *text = hu_json_get_string(msg, "text");
            if (text && strlen(text) > 0)
                line_queue_push(c, user_id, strlen(user_id), text, strlen(text));
        }
    }
    hu_json_free(alloc, parsed);
    return HU_OK;
#endif
}

hu_error_t hu_line_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                        size_t max_msgs, size_t *out_count) {
    (void)alloc;
    hu_line_ctx_t *c = (hu_line_ctx_t *)channel_ctx;
    if (!c || !msgs || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;
#if HU_IS_TEST
    if (c->mock_count > 0) {
        size_t n = c->mock_count < max_msgs ? c->mock_count : max_msgs;
        for (size_t i = 0; i < n; i++) {
            memcpy(msgs[i].session_key, c->mock_msgs[i].session_key, 128);
            memcpy(msgs[i].content, c->mock_msgs[i].content, 4096);
        }
        *out_count = n;
        c->mock_count = 0;
        return HU_OK;
    }
#endif
    size_t cnt = 0;
    while (c->queue_count > 0 && cnt < max_msgs) {
        hu_line_queued_msg_t *slot = &c->queue[c->queue_head];
        memcpy(msgs[cnt].session_key, slot->session_key, sizeof(slot->session_key));
        memcpy(msgs[cnt].content, slot->content, sizeof(slot->content));
        c->queue_head = (c->queue_head + 1) % LINE_QUEUE_MAX;
        c->queue_count--;
        cnt++;
    }
    *out_count = cnt;
    return HU_OK;
}

hu_error_t hu_line_create(hu_allocator_t *alloc, const char *channel_token,
                          size_t channel_token_len, hu_channel_t *out) {
    return hu_line_create_ex(alloc, channel_token, channel_token_len, NULL, 0, out);
}

hu_error_t hu_line_create_ex(hu_allocator_t *alloc, const char *channel_token,
                             size_t channel_token_len, const char *user_id, size_t user_id_len,
                             hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_line_ctx_t *c = (hu_line_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    if (channel_token && channel_token_len > 0) {
        c->channel_token = (char *)malloc(channel_token_len + 1);
        if (!c->channel_token) {
            free(c);
            return HU_ERR_OUT_OF_MEMORY;
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
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->user_id, user_id, user_id_len);
        c->user_id[user_id_len] = '\0';
        c->user_id_len = user_id_len;
    }
    out->ctx = c;
    out->vtable = &line_vtable;
    return HU_OK;
}

bool hu_line_is_configured(hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return false;
    hu_line_ctx_t *c = (hu_line_ctx_t *)ch->ctx;
    return c->channel_token != NULL && c->channel_token_len > 0 &&
           (c->user_id != NULL && c->user_id_len > 0);
}

void hu_line_destroy(hu_channel_t *ch) {
    if (ch && ch->ctx) {
        hu_line_ctx_t *c = (hu_line_ctx_t *)ch->ctx;
        hu_allocator_t *a = c->alloc;
        if (c->channel_token)
            free(c->channel_token);
        if (c->user_id)
            free(c->user_id);
        a->free(a->ctx, c, sizeof(*c));
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

#if HU_IS_TEST
hu_error_t hu_line_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                    size_t session_key_len, const char *content,
                                    size_t content_len) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_line_ctx_t *c = (hu_line_ctx_t *)ch->ctx;
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
const char *hu_line_test_get_last_message(hu_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    hu_line_ctx_t *c = (hu_line_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif
