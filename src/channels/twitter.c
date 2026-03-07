#include "seaclaw/channels/twitter.h"
#include "seaclaw/channel.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TWITTER_API_BASE        "https://api.x.com/2/"
#define TWITTER_QUEUE_MAX       32
#define TWITTER_SESSION_KEY_MAX 127
#define TWITTER_CONTENT_MAX     4095

typedef struct sc_twitter_queued_msg {
    char session_key[128];
    char content[4096];
} sc_twitter_queued_msg_t;

typedef struct sc_twitter_ctx {
    sc_allocator_t *alloc;
    char *bearer_token;
    size_t bearer_token_len;
    bool running;
    sc_twitter_queued_msg_t queue[TWITTER_QUEUE_MAX];
    size_t queue_head;
    size_t queue_tail;
    size_t queue_count;
#if SC_IS_TEST
    char last_message[4096];
    size_t last_message_len;
    struct {
        char session_key[128];
        char content[4096];
    } mock_msgs[8];
    size_t mock_count;
#endif
} sc_twitter_ctx_t;

static sc_error_t twitter_start(void *ctx) {
    sc_twitter_ctx_t *c = (sc_twitter_ctx_t *)ctx;
    if (!c)
        return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void twitter_stop(void *ctx) {
    sc_twitter_ctx_t *c = (sc_twitter_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static sc_error_t twitter_send(void *ctx, const char *target, size_t target_len,
                               const char *message, size_t message_len, const char *const *media,
                               size_t media_count) {
    (void)media;
    (void)media_count;
    sc_twitter_ctx_t *c = (sc_twitter_ctx_t *)ctx;

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
    if (!c->bearer_token || c->bearer_token_len == 0)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;
    if (!target || target_len == 0 || !message)
        return SC_ERR_INVALID_ARGUMENT;

    char url_buf[512];
    int n = snprintf(url_buf, sizeof(url_buf), "%sdm_conversations/with/%.*s/messages",
                     TWITTER_API_BASE, (int)target_len, target);
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return SC_ERR_INTERNAL;

    sc_json_buf_t jbuf;
    sc_error_t err = sc_json_buf_init(&jbuf, c->alloc);
    if (err)
        return err;

    err = sc_json_buf_append_raw(&jbuf, "{\"text\":\"", 8);
    if (err)
        goto jfail;
    err = sc_json_append_string(&jbuf, message, message_len);
    if (err)
        goto jfail;
    err = sc_json_buf_append_raw(&jbuf, "\"}", 2);
    if (err)
        goto jfail;

    char auth_buf[512];
    n = snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)c->bearer_token_len,
                 c->bearer_token);
    if (n <= 0 || (size_t)n >= sizeof(auth_buf)) {
        sc_json_buf_free(&jbuf);
        return SC_ERR_INTERNAL;
    }

    sc_http_response_t resp = {0};
    err = sc_http_post_json(c->alloc, url_buf, auth_buf, jbuf.ptr, jbuf.len, &resp);
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

static void twitter_queue_push(sc_twitter_ctx_t *c, const char *from, size_t from_len,
                               const char *body, size_t body_len) {
    if (!c || !from || !body || c->queue_count >= TWITTER_QUEUE_MAX)
        return;
    sc_twitter_queued_msg_t *slot = &c->queue[c->queue_tail];
    size_t sk = from_len < TWITTER_SESSION_KEY_MAX ? from_len : TWITTER_SESSION_KEY_MAX;
    memcpy(slot->session_key, from, sk);
    slot->session_key[sk] = '\0';
    size_t ct = body_len < TWITTER_CONTENT_MAX ? body_len : TWITTER_CONTENT_MAX;
    memcpy(slot->content, body, ct);
    slot->content[ct] = '\0';
    c->queue_tail = (c->queue_tail + 1) % TWITTER_QUEUE_MAX;
    c->queue_count++;
}

sc_error_t sc_twitter_on_webhook(void *channel_ctx, sc_allocator_t *alloc, const char *body,
                                 size_t body_len) {
    sc_twitter_ctx_t *c = (sc_twitter_ctx_t *)channel_ctx;
    if (!c || !body || body_len == 0)
        return SC_ERR_INVALID_ARGUMENT;
#if SC_IS_TEST
    (void)alloc;
    twitter_queue_push(c, "test-sender", 11, body, body_len);
    return SC_OK;
#else
    sc_json_value_t *parsed = NULL;
    sc_error_t err = sc_json_parse(alloc, body, body_len, &parsed);
    if (err != SC_OK || !parsed)
        return SC_OK;
    sc_json_value_t *dme = sc_json_object_get(parsed, "direct_message_events");
    if (!dme || dme->type != SC_JSON_ARRAY) {
        sc_json_free(alloc, parsed);
        return SC_OK;
    }
    for (size_t i = 0; i < dme->data.array.len; i++) {
        sc_json_value_t *ev = dme->data.array.items[i];
        if (!ev || ev->type != SC_JSON_OBJECT)
            continue;
        sc_json_value_t *msg_create = sc_json_object_get(ev, "message_create");
        if (!msg_create || msg_create->type != SC_JSON_OBJECT)
            continue;
        sc_json_value_t *msg_data = sc_json_object_get(msg_create, "message_data");
        if (!msg_data || msg_data->type != SC_JSON_OBJECT)
            continue;
        const char *text_body = sc_json_get_string(msg_data, "text");
        if (!text_body || strlen(text_body) == 0)
            continue;
        const char *sender_id = sc_json_get_string(msg_create, "sender_id");
        if (!sender_id)
            continue;
        twitter_queue_push(c, sender_id, strlen(sender_id), text_body, strlen(text_body));
    }
    sc_json_free(alloc, parsed);
    return SC_OK;
#endif
}

sc_error_t sc_twitter_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                           size_t max_msgs, size_t *out_count) {
    (void)alloc;
    sc_twitter_ctx_t *c = (sc_twitter_ctx_t *)channel_ctx;
    if (!c || !msgs || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out_count = 0;
#if SC_IS_TEST
    {
        size_t n = c->mock_count < max_msgs ? c->mock_count : max_msgs;
        for (size_t i = 0; i < n; i++) {
            memcpy(msgs[i].session_key, c->mock_msgs[i].session_key, 128);
            memcpy(msgs[i].content, c->mock_msgs[i].content, 4096);
        }
        *out_count = n;
        c->mock_count = 0;
        return SC_OK;
    }
#endif
    size_t cnt = 0;
    while (c->queue_count > 0 && cnt < max_msgs) {
        sc_twitter_queued_msg_t *slot = &c->queue[c->queue_head];
        memcpy(msgs[cnt].session_key, slot->session_key, sizeof(slot->session_key));
        memcpy(msgs[cnt].content, slot->content, sizeof(slot->content));
        c->queue_head = (c->queue_head + 1) % TWITTER_QUEUE_MAX;
        c->queue_count--;
        cnt++;
    }
    *out_count = cnt;
    return SC_OK;
}

static const char *twitter_name(void *ctx) {
    (void)ctx;
    return "twitter";
}
static bool twitter_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static const sc_channel_vtable_t twitter_vtable = {
    .start = twitter_start,
    .stop = twitter_stop,
    .send = twitter_send,
    .name = twitter_name,
    .health_check = twitter_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

sc_error_t sc_twitter_create(sc_allocator_t *alloc, const char *bearer_token,
                             size_t bearer_token_len, sc_channel_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_twitter_ctx_t *c = (sc_twitter_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    if (bearer_token && bearer_token_len > 0) {
        c->bearer_token = sc_strndup(alloc, bearer_token, bearer_token_len);
        if (!c->bearer_token) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return SC_ERR_OUT_OF_MEMORY;
        }
        c->bearer_token_len = bearer_token_len;
    }
    out->ctx = c;
    out->vtable = &twitter_vtable;
    return SC_OK;
}

void sc_twitter_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_twitter_ctx_t *c = (sc_twitter_ctx_t *)ch->ctx;
        if (c->alloc) {
            if (c->bearer_token)
                sc_str_free(c->alloc, c->bearer_token);
            c->alloc->free(c->alloc->ctx, c, sizeof(*c));
        }
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

#if SC_IS_TEST
sc_error_t sc_twitter_test_inject_mock(sc_channel_t *ch, const char *session_key,
                                       size_t session_key_len, const char *content,
                                       size_t content_len) {
    if (!ch || !ch->ctx)
        return SC_ERR_INVALID_ARGUMENT;
    sc_twitter_ctx_t *c = (sc_twitter_ctx_t *)ch->ctx;
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
const char *sc_twitter_test_get_last_message(sc_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    sc_twitter_ctx_t *c = (sc_twitter_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif
