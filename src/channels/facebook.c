#include "human/channels/facebook.h"
#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/channels/meta_common.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FACEBOOK_QUEUE_MAX       32
#define FACEBOOK_SESSION_KEY_MAX 127
#define FACEBOOK_CONTENT_MAX     4095
#define FACEBOOK_ENDPOINT        "me/messages"
#define FACEBOOK_ENDPOINT_LEN    12

typedef struct hu_facebook_queued_msg {
    char session_key[128];
    char content[4096];
} hu_facebook_queued_msg_t;

typedef struct hu_facebook_ctx {
    hu_allocator_t *alloc;
    char *page_id;
    size_t page_id_len;
    char *page_access_token;
    size_t page_access_token_len;
    char *app_secret;
    size_t app_secret_len;
    bool running;
    hu_facebook_queued_msg_t queue[FACEBOOK_QUEUE_MAX];
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
} hu_facebook_ctx_t;

static hu_error_t facebook_start(void *ctx) {
    hu_facebook_ctx_t *c = (hu_facebook_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
    return HU_OK;
}

static void facebook_stop(void *ctx) {
    hu_facebook_ctx_t *c = (hu_facebook_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static hu_error_t facebook_send(void *ctx, const char *target, size_t target_len,
                                const char *message, size_t message_len, const char *const *media,
                                size_t media_count) {
    (void)target;
    (void)target_len;
    (void)media;
    (void)media_count;
    hu_facebook_ctx_t *c = (hu_facebook_ctx_t *)ctx;

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
    if (!c->page_access_token || c->page_access_token_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    if (!target || target_len == 0 || !message)
        return HU_ERR_INVALID_ARGUMENT;

    hu_json_buf_t jbuf;
    hu_error_t err = hu_json_buf_init(&jbuf, c->alloc);
    if (err)
        return err;

    err = hu_json_buf_append_raw(&jbuf, "{\"recipient\":{", 13);
    if (err)
        goto jfail;
    err = hu_json_append_key_value(&jbuf, "id", 2, target, target_len);
    if (err)
        goto jfail;
    err = hu_json_buf_append_raw(&jbuf, "},\"message\":{", 13);
    if (err)
        goto jfail;
    err = hu_json_append_key_value(&jbuf, "text", 4, message, message_len);
    if (err)
        goto jfail;
    err = hu_json_buf_append_raw(&jbuf, "}}", 2);
    if (err)
        goto jfail;

    err = hu_meta_graph_send(c->alloc, c->page_access_token, c->page_access_token_len,
                             FACEBOOK_ENDPOINT, FACEBOOK_ENDPOINT_LEN, jbuf.ptr, jbuf.len);
    hu_json_buf_free(&jbuf);
    if (err != HU_OK)
        return HU_ERR_CHANNEL_SEND;
    return HU_OK;
jfail:
    hu_json_buf_free(&jbuf);
    return err;
#endif
}

static void facebook_queue_push(hu_facebook_ctx_t *c, const char *from, size_t from_len,
                                const char *body, size_t body_len) {
    if (!c || !from || !body || c->queue_count >= FACEBOOK_QUEUE_MAX)
        return;
    hu_facebook_queued_msg_t *slot = &c->queue[c->queue_tail];
    size_t sk = from_len < FACEBOOK_SESSION_KEY_MAX ? from_len : FACEBOOK_SESSION_KEY_MAX;
    memcpy(slot->session_key, from, sk);
    slot->session_key[sk] = '\0';
    size_t ct = body_len < FACEBOOK_CONTENT_MAX ? body_len : FACEBOOK_CONTENT_MAX;
    memcpy(slot->content, body, ct);
    slot->content[ct] = '\0';
    c->queue_tail = (c->queue_tail + 1) % FACEBOOK_QUEUE_MAX;
    c->queue_count++;
}

hu_error_t hu_facebook_on_webhook(void *channel_ctx, hu_allocator_t *alloc, const char *body,
                                  size_t body_len) {
    hu_facebook_ctx_t *c = (hu_facebook_ctx_t *)channel_ctx;
    if (!c || !body || body_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    (void)alloc;
    facebook_queue_push(c, "test-sender", 11, body, body_len);
    return HU_OK;
#else
    hu_json_value_t *parsed = NULL;
    hu_error_t err = hu_json_parse(alloc, body, body_len, &parsed);
    if (err != HU_OK || !parsed)
        return HU_OK;

    hu_json_value_t *entry = hu_json_object_get(parsed, "entry");
    if (!entry || entry->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, parsed);
        return HU_OK;
    }

    for (size_t e = 0; e < entry->data.array.len; e++) {
        hu_json_value_t *ent = entry->data.array.items[e];
        if (!ent || ent->type != HU_JSON_OBJECT)
            continue;
        hu_json_value_t *messaging = hu_json_object_get(ent, "messaging");
        if (!messaging || messaging->type != HU_JSON_ARRAY)
            continue;

        for (size_t m = 0; m < messaging->data.array.len; m++) {
            hu_json_value_t *msg = messaging->data.array.items[m];
            if (!msg || msg->type != HU_JSON_OBJECT)
                continue;
            hu_json_value_t *message_obj = hu_json_object_get(msg, "message");
            if (!message_obj || message_obj->type != HU_JSON_OBJECT)
                continue;
            const char *text = hu_json_get_string(message_obj, "text");
            if (!text || strlen(text) == 0)
                continue;
            hu_json_value_t *sender = hu_json_object_get(msg, "sender");
            if (!sender || sender->type != HU_JSON_OBJECT)
                continue;
            const char *sender_id = hu_json_get_string(sender, "id");
            if (!sender_id)
                continue;
            facebook_queue_push(c, sender_id, strlen(sender_id), text, strlen(text));
        }
    }
    hu_json_free(alloc, parsed);
    return HU_OK;
#endif
}

hu_error_t hu_facebook_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                            size_t max_msgs, size_t *out_count) {
    (void)alloc;
    hu_facebook_ctx_t *c = (hu_facebook_ctx_t *)channel_ctx;
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
        hu_facebook_queued_msg_t *slot = &c->queue[c->queue_head];
        memcpy(msgs[cnt].session_key, slot->session_key, sizeof(slot->session_key));
        memcpy(msgs[cnt].content, slot->content, sizeof(slot->content));
        c->queue_head = (c->queue_head + 1) % FACEBOOK_QUEUE_MAX;
        c->queue_count--;
        cnt++;
    }
    *out_count = cnt;
    return HU_OK;
}

static const char *facebook_name(void *ctx) {
    (void)ctx;
    return "facebook";
}

static bool facebook_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static const hu_channel_vtable_t facebook_vtable = {
    .start = facebook_start,
    .stop = facebook_stop,
    .send = facebook_send,
    .name = facebook_name,
    .health_check = facebook_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

hu_error_t hu_facebook_create(hu_allocator_t *alloc, const char *page_id, size_t page_id_len,
                              const char *page_access_token, size_t token_len,
                              const char *app_secret, size_t secret_len, hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_facebook_ctx_t *c = (hu_facebook_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;

    if (page_id && page_id_len > 0) {
        c->page_id = (char *)alloc->alloc(alloc->ctx, page_id_len + 1);
        if (!c->page_id) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
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
            return HU_ERR_OUT_OF_MEMORY;
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
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->app_secret, app_secret, secret_len);
        c->app_secret[secret_len] = '\0';
        c->app_secret_len = secret_len;
    }

    out->ctx = c;
    out->vtable = &facebook_vtable;
    return HU_OK;
}

void hu_facebook_destroy(hu_channel_t *ch) {
    if (ch && ch->ctx) {
        hu_facebook_ctx_t *c = (hu_facebook_ctx_t *)ch->ctx;
        hu_allocator_t *a = c->alloc;
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

#if HU_IS_TEST
hu_error_t hu_facebook_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                        size_t session_key_len, const char *content,
                                        size_t content_len) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_facebook_ctx_t *c = (hu_facebook_ctx_t *)ch->ctx;
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
const char *hu_facebook_test_get_last_message(hu_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    hu_facebook_ctx_t *c = (hu_facebook_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif
