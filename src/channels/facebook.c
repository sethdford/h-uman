#include "seaclaw/channels/facebook.h"
#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/channels/meta_common.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FACEBOOK_QUEUE_MAX       32
#define FACEBOOK_SESSION_KEY_MAX 127
#define FACEBOOK_CONTENT_MAX     4095
#define FACEBOOK_ENDPOINT        "me/messages"
#define FACEBOOK_ENDPOINT_LEN    12

typedef struct sc_facebook_queued_msg {
    char session_key[128];
    char content[4096];
} sc_facebook_queued_msg_t;

typedef struct sc_facebook_ctx {
    sc_allocator_t *alloc;
    char *page_id;
    size_t page_id_len;
    char *page_access_token;
    size_t page_access_token_len;
    char *app_secret;
    size_t app_secret_len;
    bool running;
    sc_facebook_queued_msg_t queue[FACEBOOK_QUEUE_MAX];
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
} sc_facebook_ctx_t;

static sc_error_t facebook_start(void *ctx) {
    sc_facebook_ctx_t *c = (sc_facebook_ctx_t *)ctx;
    if (!c)
        return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void facebook_stop(void *ctx) {
    sc_facebook_ctx_t *c = (sc_facebook_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static sc_error_t facebook_send(void *ctx, const char *target, size_t target_len,
                                const char *message, size_t message_len, const char *const *media,
                                size_t media_count) {
    (void)media;
    (void)media_count;
    sc_facebook_ctx_t *c = (sc_facebook_ctx_t *)ctx;

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
    if (!c->page_access_token || c->page_access_token_len == 0)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;
    if (!target || target_len == 0 || !message)
        return SC_ERR_INVALID_ARGUMENT;

    sc_json_buf_t jbuf;
    sc_error_t err = sc_json_buf_init(&jbuf, c->alloc);
    if (err)
        return err;

    err = sc_json_buf_append_raw(&jbuf, "{\"recipient\":{", 13);
    if (err)
        goto jfail;
    err = sc_json_append_key_value(&jbuf, "id", 2, target, target_len);
    if (err)
        goto jfail;
    err = sc_json_buf_append_raw(&jbuf, "},\"message\":{", 13);
    if (err)
        goto jfail;
    err = sc_json_append_key_value(&jbuf, "text", 4, message, message_len);
    if (err)
        goto jfail;
    err = sc_json_buf_append_raw(&jbuf, "}}", 2);
    if (err)
        goto jfail;

    err = sc_meta_graph_send(c->alloc, c->page_access_token, c->page_access_token_len,
                             FACEBOOK_ENDPOINT, FACEBOOK_ENDPOINT_LEN, jbuf.ptr, jbuf.len);
    sc_json_buf_free(&jbuf);
    if (err != SC_OK)
        return SC_ERR_CHANNEL_SEND;
    return SC_OK;
jfail:
    sc_json_buf_free(&jbuf);
    return err;
#endif
}

static void facebook_queue_push(sc_facebook_ctx_t *c, const char *from, size_t from_len,
                                const char *body, size_t body_len) {
    if (!c || !from || !body || c->queue_count >= FACEBOOK_QUEUE_MAX)
        return;
    sc_facebook_queued_msg_t *slot = &c->queue[c->queue_tail];
    size_t sk = from_len < FACEBOOK_SESSION_KEY_MAX ? from_len : FACEBOOK_SESSION_KEY_MAX;
    memcpy(slot->session_key, from, sk);
    slot->session_key[sk] = '\0';
    size_t ct = body_len < FACEBOOK_CONTENT_MAX ? body_len : FACEBOOK_CONTENT_MAX;
    memcpy(slot->content, body, ct);
    slot->content[ct] = '\0';
    c->queue_tail = (c->queue_tail + 1) % FACEBOOK_QUEUE_MAX;
    c->queue_count++;
}

sc_error_t sc_facebook_on_webhook(void *channel_ctx, sc_allocator_t *alloc, const char *body,
                                  size_t body_len) {
    sc_facebook_ctx_t *c = (sc_facebook_ctx_t *)channel_ctx;
    if (!c || !body || body_len == 0)
        return SC_ERR_INVALID_ARGUMENT;
#if SC_IS_TEST
    (void)alloc;
    facebook_queue_push(c, "test-sender", 11, body, body_len);
    return SC_OK;
#else
    sc_json_value_t *parsed = NULL;
    sc_error_t err = sc_json_parse(alloc, body, body_len, &parsed);
    if (err != SC_OK || !parsed)
        return SC_OK;

    sc_json_value_t *entry = sc_json_object_get(parsed, "entry");
    if (!entry || entry->type != SC_JSON_ARRAY) {
        sc_json_free(alloc, parsed);
        return SC_OK;
    }

    for (size_t e = 0; e < entry->data.array.len; e++) {
        sc_json_value_t *ent = entry->data.array.items[e];
        if (!ent || ent->type != SC_JSON_OBJECT)
            continue;
        sc_json_value_t *messaging = sc_json_object_get(ent, "messaging");
        if (!messaging || messaging->type != SC_JSON_ARRAY)
            continue;

        for (size_t m = 0; m < messaging->data.array.len; m++) {
            sc_json_value_t *msg = messaging->data.array.items[m];
            if (!msg || msg->type != SC_JSON_OBJECT)
                continue;
            sc_json_value_t *message_obj = sc_json_object_get(msg, "message");
            if (!message_obj || message_obj->type != SC_JSON_OBJECT)
                continue;
            const char *text = sc_json_get_string(message_obj, "text");
            if (!text || strlen(text) == 0)
                continue;
            sc_json_value_t *sender = sc_json_object_get(msg, "sender");
            if (!sender || sender->type != SC_JSON_OBJECT)
                continue;
            const char *sender_id = sc_json_get_string(sender, "id");
            if (!sender_id)
                continue;
            facebook_queue_push(c, sender_id, strlen(sender_id), text, strlen(text));
        }
    }
    sc_json_free(alloc, parsed);
    return SC_OK;
#endif
}

sc_error_t sc_facebook_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                            size_t max_msgs, size_t *out_count) {
    (void)alloc;
    sc_facebook_ctx_t *c = (sc_facebook_ctx_t *)channel_ctx;
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
        sc_facebook_queued_msg_t *slot = &c->queue[c->queue_head];
        memcpy(msgs[cnt].session_key, slot->session_key, sizeof(slot->session_key));
        memcpy(msgs[cnt].content, slot->content, sizeof(slot->content));
        c->queue_head = (c->queue_head + 1) % FACEBOOK_QUEUE_MAX;
        c->queue_count--;
        cnt++;
    }
    *out_count = cnt;
    return SC_OK;
}

static const char *facebook_name(void *ctx) {
    (void)ctx;
    return "facebook";
}

static bool facebook_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static const sc_channel_vtable_t facebook_vtable = {
    .start = facebook_start,
    .stop = facebook_stop,
    .send = facebook_send,
    .name = facebook_name,
    .health_check = facebook_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

sc_error_t sc_facebook_create(sc_allocator_t *alloc, const char *page_id, size_t page_id_len,
                              const char *page_access_token, size_t token_len,
                              const char *app_secret, size_t secret_len, sc_channel_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;

    sc_facebook_ctx_t *c = (sc_facebook_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;

    if (page_id && page_id_len > 0) {
        c->page_id = (char *)alloc->alloc(alloc->ctx, page_id_len + 1);
        if (!c->page_id) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->page_id, page_id, page_id_len);
        c->page_id[page_id_len] = '\0';
        c->page_id_len = page_id_len;
    }

    if (page_access_token && token_len > 0) {
        c->page_access_token = (char *)alloc->alloc(alloc->ctx, token_len + 1);
        if (!c->page_access_token) {
            if (c->page_id)
                alloc->free(alloc->ctx, c->page_id, c->page_id_len + 1);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->page_access_token, page_access_token, token_len);
        c->page_access_token[token_len] = '\0';
        c->page_access_token_len = token_len;
    }

    if (app_secret && secret_len > 0) {
        c->app_secret = (char *)alloc->alloc(alloc->ctx, secret_len + 1);
        if (!c->app_secret) {
            if (c->page_id)
                alloc->free(alloc->ctx, c->page_id, c->page_id_len + 1);
            if (c->page_access_token)
                alloc->free(alloc->ctx, c->page_access_token, c->page_access_token_len + 1);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->app_secret, app_secret, secret_len);
        c->app_secret[secret_len] = '\0';
        c->app_secret_len = secret_len;
    }

    out->ctx = c;
    out->vtable = &facebook_vtable;
    return SC_OK;
}

void sc_facebook_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_facebook_ctx_t *c = (sc_facebook_ctx_t *)ch->ctx;
        sc_allocator_t *a = c->alloc;
        if (a) {
            if (c->page_id)
                a->free(a->ctx, c->page_id, c->page_id_len + 1);
            if (c->page_access_token)
                a->free(a->ctx, c->page_access_token, c->page_access_token_len + 1);
            if (c->app_secret)
                a->free(a->ctx, c->app_secret, c->app_secret_len + 1);
            a->free(a->ctx, c, sizeof(*c));
        }
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

#if SC_IS_TEST
sc_error_t sc_facebook_test_inject_mock(sc_channel_t *ch, const char *session_key,
                                        size_t session_key_len, const char *content,
                                        size_t content_len) {
    if (!ch || !ch->ctx)
        return SC_ERR_INVALID_ARGUMENT;
    sc_facebook_ctx_t *c = (sc_facebook_ctx_t *)ch->ctx;
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
const char *sc_facebook_test_get_last_message(sc_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    sc_facebook_ctx_t *c = (sc_facebook_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif
