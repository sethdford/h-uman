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
    (void)c;
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    return SC_OK;
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

    sc_facebook_ctx_t *c = (sc_facebook_ctx_t *)calloc(1, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;

    if (page_id && page_id_len > 0) {
        c->page_id = (char *)malloc(page_id_len + 1);
        if (!c->page_id) {
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->page_id, page_id, page_id_len);
        c->page_id[page_id_len] = '\0';
        c->page_id_len = page_id_len;
    }

    if (page_access_token && token_len > 0) {
        c->page_access_token = (char *)malloc(token_len + 1);
        if (!c->page_access_token) {
            if (c->page_id)
                free(c->page_id);
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->page_access_token, page_access_token, token_len);
        c->page_access_token[token_len] = '\0';
        c->page_access_token_len = token_len;
    }

    if (app_secret && secret_len > 0) {
        c->app_secret = (char *)malloc(secret_len + 1);
        if (!c->app_secret) {
            if (c->page_id)
                free(c->page_id);
            if (c->page_access_token)
                free(c->page_access_token);
            free(c);
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
        if (c->page_id)
            free(c->page_id);
        if (c->page_access_token)
            free(c->page_access_token);
        if (c->app_secret)
            free(c->app_secret);
        free(c);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
