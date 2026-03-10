#include "human/channels/google_chat.h"
#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GCHAT_QUEUE_MAX       32
#define GCHAT_SESSION_KEY_MAX 127
#define GCHAT_CONTENT_MAX     4095

typedef struct hu_google_chat_queued_msg {
    char session_key[128];
    char content[4096];
} hu_google_chat_queued_msg_t;

typedef struct hu_google_chat_ctx {
    hu_allocator_t *alloc;
    char *webhook_url;
    size_t webhook_url_len;
    bool running;
    hu_google_chat_queued_msg_t queue[GCHAT_QUEUE_MAX];
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
} hu_google_chat_ctx_t;

static hu_error_t google_chat_start(void *ctx) {
    hu_google_chat_ctx_t *c = (hu_google_chat_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
    return HU_OK;
}

static void google_chat_stop(void *ctx) {
    hu_google_chat_ctx_t *c = (hu_google_chat_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static hu_error_t google_chat_send(void *ctx, const char *target, size_t target_len,
                                   const char *message, size_t message_len,
                                   const char *const *media, size_t media_count) {
    (void)target;
    (void)target_len;
    hu_google_chat_ctx_t *c = (hu_google_chat_ctx_t *)ctx;

#if HU_IS_TEST
    if (!c->webhook_url || c->webhook_url_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
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
    if (!c->webhook_url || c->webhook_url_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    if (!message)
        return HU_ERR_INVALID_ARGUMENT;
    if (c->webhook_url_len < 9 || strncmp(c->webhook_url, "https://", 8) != 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;

    hu_json_buf_t jbuf;
    hu_error_t err = hu_json_buf_init(&jbuf, c->alloc);
    if (err)
        return err;

    if (media_count > 0 && media) {
        err = hu_json_buf_append_raw(&jbuf,
                                     "{\"cards\":[{\"sections\":[{\"widgets\":[{\"textParagraph\":{"
                                     "\"text\":",
                                     66);
        if (err)
            goto jfail;
        err = hu_json_append_string(&jbuf, message, message_len);
        if (err)
            goto jfail;
        err = hu_json_buf_append_raw(&jbuf, "}}", 2);
        if (err)
            goto jfail;
        for (size_t i = 0; i < media_count && media[i]; i++) {
            size_t url_len = strlen(media[i]);
            err = hu_json_buf_append_raw(&jbuf, ",{\"image\":{\"imageUrl\":", 22);
            if (err)
                goto jfail;
            err = hu_json_append_string(&jbuf, media[i], url_len);
            if (err)
                goto jfail;
            err = hu_json_buf_append_raw(&jbuf, "}}", 2);
            if (err)
                goto jfail;
        }
        err = hu_json_buf_append_raw(&jbuf, "]}]}]}", 6);
        if (err)
            goto jfail;
    } else {
        err = hu_json_buf_append_raw(&jbuf, "{\"text\":", 8);
        if (err)
            goto jfail;
        err = hu_json_append_string(&jbuf, message, message_len);
        if (err)
            goto jfail;
        err = hu_json_buf_append_raw(&jbuf, "}", 1);
        if (err)
            goto jfail;
    }

    hu_http_response_t resp = {0};
    err = hu_http_post_json(c->alloc, c->webhook_url, NULL, jbuf.ptr, jbuf.len, &resp);
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

static const char *google_chat_name(void *ctx) {
    (void)ctx;
    return "google_chat";
}

static bool google_chat_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static void google_chat_queue_push(hu_google_chat_ctx_t *c, const char *from, size_t from_len,
                                   const char *body, size_t body_len) {
    if (c->queue_count >= GCHAT_QUEUE_MAX)
        return;
    hu_google_chat_queued_msg_t *slot = &c->queue[c->queue_tail];
    size_t sk = from_len < GCHAT_SESSION_KEY_MAX ? from_len : GCHAT_SESSION_KEY_MAX;
    memcpy(slot->session_key, from, sk);
    slot->session_key[sk] = '\0';
    size_t ct = body_len < GCHAT_CONTENT_MAX ? body_len : GCHAT_CONTENT_MAX;
    memcpy(slot->content, body, ct);
    slot->content[ct] = '\0';
    c->queue_tail = (c->queue_tail + 1) % GCHAT_QUEUE_MAX;
    c->queue_count++;
}

static const hu_channel_vtable_t google_chat_vtable = {
    .start = google_chat_start,
    .stop = google_chat_stop,
    .send = google_chat_send,
    .name = google_chat_name,
    .health_check = google_chat_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

hu_error_t hu_google_chat_on_webhook(void *channel_ctx, hu_allocator_t *alloc, const char *body,
                                     size_t body_len) {
    hu_google_chat_ctx_t *c = (hu_google_chat_ctx_t *)channel_ctx;
    if (!c || !body || body_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    (void)alloc;
    google_chat_queue_push(c, "test-sender", 11, body, body_len);
    return HU_OK;
#else
    hu_json_value_t *parsed = NULL;
    hu_error_t err = hu_json_parse(alloc, body, body_len, &parsed);
    if (err != HU_OK || !parsed)
        return HU_OK;
    hu_json_value_t *msg = hu_json_object_get(parsed, "message");
    if (msg && msg->type == HU_JSON_OBJECT) {
        const char *text = hu_json_get_string(msg, "text");
        hu_json_value_t *sender = hu_json_object_get(msg, "sender");
        const char *sender_name = sender ? hu_json_get_string(sender, "name") : NULL;
        if (text && sender_name && strlen(text) > 0)
            google_chat_queue_push(c, sender_name, strlen(sender_name), text, strlen(text));
    }
    hu_json_free(alloc, parsed);
    return HU_OK;
#endif
}

hu_error_t hu_google_chat_poll(void *channel_ctx, hu_allocator_t *alloc,
                               hu_channel_loop_msg_t *msgs, size_t max_msgs, size_t *out_count) {
    (void)alloc;
    hu_google_chat_ctx_t *c = (hu_google_chat_ctx_t *)channel_ctx;
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
        hu_google_chat_queued_msg_t *slot = &c->queue[c->queue_head];
        memcpy(msgs[cnt].session_key, slot->session_key, sizeof(slot->session_key));
        memcpy(msgs[cnt].content, slot->content, sizeof(slot->content));
        c->queue_head = (c->queue_head + 1) % GCHAT_QUEUE_MAX;
        c->queue_count--;
        cnt++;
    }
    *out_count = cnt;
    return HU_OK;
}

bool hu_google_chat_is_configured(hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return false;
    hu_google_chat_ctx_t *c = (hu_google_chat_ctx_t *)ch->ctx;
    return c->webhook_url != NULL && c->webhook_url_len > 0;
}

hu_error_t hu_google_chat_create(hu_allocator_t *alloc, const char *webhook_url,
                                 size_t webhook_url_len, hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_google_chat_ctx_t *c = (hu_google_chat_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    if (webhook_url && webhook_url_len > 0) {
        c->webhook_url = (char *)malloc(webhook_url_len + 1);
        if (!c->webhook_url) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->webhook_url, webhook_url, webhook_url_len);
        c->webhook_url[webhook_url_len] = '\0';
        c->webhook_url_len = webhook_url_len;
    }
    out->ctx = c;
    out->vtable = &google_chat_vtable;
    return HU_OK;
}

void hu_google_chat_destroy(hu_channel_t *ch) {
    if (ch && ch->ctx) {
        hu_google_chat_ctx_t *c = (hu_google_chat_ctx_t *)ch->ctx;
        hu_allocator_t *a = c->alloc;
        if (c->webhook_url)
            free(c->webhook_url);
        if (a)
            a->free(a->ctx, c, sizeof(*c));
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

#if HU_IS_TEST
hu_error_t hu_google_chat_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                           size_t session_key_len, const char *content,
                                           size_t content_len) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_google_chat_ctx_t *c = (hu_google_chat_ctx_t *)ch->ctx;
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
const char *hu_google_chat_test_get_last_message(hu_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    hu_google_chat_ctx_t *c = (hu_google_chat_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif
