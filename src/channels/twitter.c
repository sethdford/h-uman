#include "human/channels/twitter.h"
#include "human/channel.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TWITTER_API_BASE        "https://api.x.com/2/"
#define TWITTER_QUEUE_MAX       32
#define TWITTER_SESSION_KEY_MAX 127
#define TWITTER_CONTENT_MAX     4095

typedef struct hu_twitter_queued_msg {
    char session_key[128];
    char content[4096];
} hu_twitter_queued_msg_t;

typedef struct hu_twitter_ctx {
    hu_allocator_t *alloc;
    char *bearer_token;
    size_t bearer_token_len;
    bool running;
    hu_twitter_queued_msg_t queue[TWITTER_QUEUE_MAX];
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
} hu_twitter_ctx_t;

static hu_error_t twitter_start(void *ctx) {
    hu_twitter_ctx_t *c = (hu_twitter_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
    return HU_OK;
}

static void twitter_stop(void *ctx) {
    hu_twitter_ctx_t *c = (hu_twitter_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static hu_error_t twitter_send(void *ctx, const char *target, size_t target_len,
                               const char *message, size_t message_len, const char *const *media,
                               size_t media_count) {
    (void)target;
    (void)target_len;
    (void)media;
    (void)media_count;
    hu_twitter_ctx_t *c = (hu_twitter_ctx_t *)ctx;

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
    if (!c->bearer_token || c->bearer_token_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    if (!target || target_len == 0 || !message)
        return HU_ERR_INVALID_ARGUMENT;

    char url_buf[512];
    int n = snprintf(url_buf, sizeof(url_buf), "%sdm_conversations/with/%.*s/messages",
                     TWITTER_API_BASE, (int)target_len, target);
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return HU_ERR_INTERNAL;

    hu_json_buf_t jbuf;
    hu_error_t err = hu_json_buf_init(&jbuf, c->alloc);
    if (err)
        return err;

    err = hu_json_buf_append_raw(&jbuf, "{\"text\":\"", 8);
    if (err)
        goto jfail;
    err = hu_json_append_string(&jbuf, message, message_len);
    if (err)
        goto jfail;
    err = hu_json_buf_append_raw(&jbuf, "\"}", 2);
    if (err)
        goto jfail;

    char auth_buf[512];
    n = snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)c->bearer_token_len,
                 c->bearer_token);
    if (n <= 0 || (size_t)n >= sizeof(auth_buf)) {
        hu_json_buf_free(&jbuf);
        return HU_ERR_INTERNAL;
    }

    hu_http_response_t resp = {0};
    err = hu_http_post_json(c->alloc, url_buf, auth_buf, jbuf.ptr, jbuf.len, &resp);
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

static void twitter_queue_push(hu_twitter_ctx_t *c, const char *from, size_t from_len,
                               const char *body, size_t body_len) {
    if (!c || !from || !body || c->queue_count >= TWITTER_QUEUE_MAX)
        return;
    hu_twitter_queued_msg_t *slot = &c->queue[c->queue_tail];
    size_t sk = from_len < TWITTER_SESSION_KEY_MAX ? from_len : TWITTER_SESSION_KEY_MAX;
    memcpy(slot->session_key, from, sk);
    slot->session_key[sk] = '\0';
    size_t ct = body_len < TWITTER_CONTENT_MAX ? body_len : TWITTER_CONTENT_MAX;
    memcpy(slot->content, body, ct);
    slot->content[ct] = '\0';
    c->queue_tail = (c->queue_tail + 1) % TWITTER_QUEUE_MAX;
    c->queue_count++;
}

hu_error_t hu_twitter_on_webhook(void *channel_ctx, hu_allocator_t *alloc, const char *body,
                                 size_t body_len) {
    hu_twitter_ctx_t *c = (hu_twitter_ctx_t *)channel_ctx;
    if (!c || !body || body_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    (void)alloc;
    twitter_queue_push(c, "test-sender", 11, body, body_len);
    return HU_OK;
#else
    hu_json_value_t *parsed = NULL;
    hu_error_t err = hu_json_parse(alloc, body, body_len, &parsed);
    if (err != HU_OK || !parsed)
        return HU_OK;
    hu_json_value_t *dme = hu_json_object_get(parsed, "direct_message_events");
    if (!dme || dme->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, parsed);
        return HU_OK;
    }
    for (size_t i = 0; i < dme->data.array.len; i++) {
        hu_json_value_t *ev = dme->data.array.items[i];
        if (!ev || ev->type != HU_JSON_OBJECT)
            continue;
        hu_json_value_t *msg_create = hu_json_object_get(ev, "message_create");
        if (!msg_create || msg_create->type != HU_JSON_OBJECT)
            continue;
        hu_json_value_t *msg_data = hu_json_object_get(msg_create, "message_data");
        if (!msg_data || msg_data->type != HU_JSON_OBJECT)
            continue;
        const char *text_body = hu_json_get_string(msg_data, "text");
        if (!text_body || strlen(text_body) == 0)
            continue;
        const char *sender_id = hu_json_get_string(msg_create, "sender_id");
        if (!sender_id)
            continue;
        twitter_queue_push(c, sender_id, strlen(sender_id), text_body, strlen(text_body));
    }
    hu_json_free(alloc, parsed);
    return HU_OK;
#endif
}

hu_error_t hu_twitter_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                           size_t max_msgs, size_t *out_count) {
    (void)alloc;
    hu_twitter_ctx_t *c = (hu_twitter_ctx_t *)channel_ctx;
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
        hu_twitter_queued_msg_t *slot = &c->queue[c->queue_head];
        memcpy(msgs[cnt].session_key, slot->session_key, sizeof(slot->session_key));
        memcpy(msgs[cnt].content, slot->content, sizeof(slot->content));
        c->queue_head = (c->queue_head + 1) % TWITTER_QUEUE_MAX;
        c->queue_count--;
        cnt++;
    }
    *out_count = cnt;
    return HU_OK;
}

static const char *twitter_name(void *ctx) {
    (void)ctx;
    return "twitter";
}
static bool twitter_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static const hu_channel_vtable_t twitter_vtable = {
    .start = twitter_start,
    .stop = twitter_stop,
    .send = twitter_send,
    .name = twitter_name,
    .health_check = twitter_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

hu_error_t hu_twitter_create(hu_allocator_t *alloc, const char *bearer_token,
                             size_t bearer_token_len, hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_twitter_ctx_t *c = (hu_twitter_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    if (bearer_token && bearer_token_len > 0) {
        c->bearer_token = hu_strndup(alloc, bearer_token, bearer_token_len);
        if (!c->bearer_token) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        c->bearer_token_len = bearer_token_len;
    }
    out->ctx = c;
    out->vtable = &twitter_vtable;
    return HU_OK;
}

void hu_twitter_destroy(hu_channel_t *ch) {
    if (ch && ch->ctx) {
        hu_twitter_ctx_t *c = (hu_twitter_ctx_t *)ch->ctx;
        if (c->alloc) {
            if (c->bearer_token)
                hu_str_free(c->alloc, c->bearer_token);
            c->alloc->free(c->alloc->ctx, c, sizeof(*c));
        }
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

#if HU_IS_TEST
hu_error_t hu_twitter_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                       size_t session_key_len, const char *content,
                                       size_t content_len) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_twitter_ctx_t *c = (hu_twitter_ctx_t *)ch->ctx;
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
const char *hu_twitter_test_get_last_message(hu_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    hu_twitter_ctx_t *c = (hu_twitter_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif
