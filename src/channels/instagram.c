#include "seaclaw/channels/instagram.h"
#include "seaclaw/channel.h"
#include "seaclaw/channels/meta_common.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INSTAGRAM_ENDPOINT        "me/messages"
#define INSTAGRAM_ENDPOINT_LEN   12
#define INSTAGRAM_QUEUE_MAX      32
#define INSTAGRAM_SESSION_KEY_MAX 127
#define INSTAGRAM_CONTENT_MAX     4095

typedef struct sc_instagram_queued_msg {
    char session_key[128];
    char content[4096];
} sc_instagram_queued_msg_t;

typedef struct sc_instagram_ctx {
    sc_allocator_t *alloc;
    char *business_account_id;
    size_t business_account_id_len;
    char *access_token;
    size_t access_token_len;
    char *app_secret;
    size_t app_secret_len;
    bool running;
    sc_instagram_queued_msg_t queue[INSTAGRAM_QUEUE_MAX];
    size_t queue_head;
    size_t queue_tail;
    size_t queue_count;
} sc_instagram_ctx_t;

static sc_error_t instagram_start(void *ctx) {
    sc_instagram_ctx_t *c = (sc_instagram_ctx_t *)ctx;
    if (!c)
        return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void instagram_stop(void *ctx) {
    sc_instagram_ctx_t *c = (sc_instagram_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static sc_error_t instagram_send(void *ctx, const char *target, size_t target_len,
                                 const char *message, size_t message_len, const char *const *media,
                                 size_t media_count) {
    (void)media;
    (void)media_count;
    sc_instagram_ctx_t *c = (sc_instagram_ctx_t *)ctx;

#if SC_IS_TEST
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    (void)media;
    (void)media_count;
    (void)c;
    return SC_OK;
#else
    if (!c || !c->alloc)
        return SC_ERR_INVALID_ARGUMENT;
    if (!c->access_token || c->access_token_len == 0)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;
    if (!target || target_len == 0 || !message)
        return SC_ERR_INVALID_ARGUMENT;

    sc_json_buf_t jbuf;
    sc_error_t err = sc_json_buf_init(&jbuf, c->alloc);
    if (err)
        return err;

    err = sc_json_buf_append_raw(&jbuf, "{\"recipient\":{\"id\":\"", 21);
    if (err)
        goto jfail;
    err = sc_json_append_string(&jbuf, target, target_len);
    if (err)
        goto jfail;
    err = sc_json_buf_append_raw(&jbuf, "\"},\"message\":{\"text\":\"", 24);
    if (err)
        goto jfail;
    err = sc_json_append_string(&jbuf, message, message_len);
    if (err)
        goto jfail;
    err = sc_json_buf_append_raw(&jbuf, "\"}}", 3);
    if (err)
        goto jfail;

    err = sc_meta_graph_send(c->alloc, c->access_token, c->access_token_len, INSTAGRAM_ENDPOINT,
                             INSTAGRAM_ENDPOINT_LEN, jbuf.ptr, jbuf.len);
    sc_json_buf_free(&jbuf);
    if (err != SC_OK)
        return SC_ERR_CHANNEL_SEND;
    return SC_OK;
jfail:
    sc_json_buf_free(&jbuf);
    return err;
#endif
}

static void instagram_queue_push(sc_instagram_ctx_t *c, const char *from, size_t from_len,
                                 const char *body, size_t body_len) {
    if (c->queue_count >= INSTAGRAM_QUEUE_MAX)
        return;
    sc_instagram_queued_msg_t *slot = &c->queue[c->queue_tail];
    size_t sk = from_len < INSTAGRAM_SESSION_KEY_MAX ? from_len : INSTAGRAM_SESSION_KEY_MAX;
    memcpy(slot->session_key, from, sk);
    slot->session_key[sk] = '\0';
    size_t ct = body_len < INSTAGRAM_CONTENT_MAX ? body_len : INSTAGRAM_CONTENT_MAX;
    memcpy(slot->content, body, ct);
    slot->content[ct] = '\0';
    c->queue_tail = (c->queue_tail + 1) % INSTAGRAM_QUEUE_MAX;
    c->queue_count++;
}

sc_error_t sc_instagram_on_webhook(void *channel_ctx, sc_allocator_t *alloc, const char *body,
                                   size_t body_len) {
    sc_instagram_ctx_t *c = (sc_instagram_ctx_t *)channel_ctx;
    if (!c || !body || body_len == 0)
        return SC_ERR_INVALID_ARGUMENT;
#if SC_IS_TEST
    (void)alloc;
    instagram_queue_push(c, "test-sender", 11, body, body_len);
    return SC_OK;
#else
    sc_json_value_t *parsed = NULL;
    sc_error_t err = sc_json_parse(alloc, body, body_len, &parsed);
    if (err != SC_OK || !parsed)
        return SC_OK;
    const char *obj_type = sc_json_get_string(parsed, "object");
    if (!obj_type || strcmp(obj_type, "instagram") != 0) {
        sc_json_free(alloc, parsed);
        return SC_OK;
    }
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
            if (sc_json_get_bool(msg, "is_echo", false))
                continue;
            sc_json_value_t *message_obj = sc_json_object_get(msg, "message");
            if (!message_obj || message_obj->type != SC_JSON_OBJECT)
                continue;
            const char *text_body = sc_json_get_string(message_obj, "text");
            if (!text_body || strlen(text_body) == 0)
                continue;
            sc_json_value_t *sender = sc_json_object_get(msg, "sender");
            if (!sender || sender->type != SC_JSON_OBJECT)
                continue;
            const char *from = sc_json_get_string(sender, "id");
            if (!from)
                continue;
            instagram_queue_push(c, from, strlen(from), text_body, strlen(text_body));
        }
    }
    sc_json_free(alloc, parsed);
    return SC_OK;
#endif
}

sc_error_t sc_instagram_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                             size_t max_msgs, size_t *out_count) {
    (void)alloc;
    sc_instagram_ctx_t *c = (sc_instagram_ctx_t *)channel_ctx;
    if (!c || !msgs || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    size_t cnt = 0;
    while (c->queue_count > 0 && cnt < max_msgs) {
        sc_instagram_queued_msg_t *slot = &c->queue[c->queue_head];
        memcpy(msgs[cnt].session_key, slot->session_key, sizeof(slot->session_key));
        memcpy(msgs[cnt].content, slot->content, sizeof(slot->content));
        c->queue_head = (c->queue_head + 1) % INSTAGRAM_QUEUE_MAX;
        c->queue_count--;
        cnt++;
    }
    *out_count = cnt;
    return SC_OK;
}

static const char *instagram_name(void *ctx) {
    (void)ctx;
    return "instagram";
}
static bool instagram_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static const sc_channel_vtable_t instagram_vtable = {
    .start = instagram_start,
    .stop = instagram_stop,
    .send = instagram_send,
    .name = instagram_name,
    .health_check = instagram_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

sc_error_t sc_instagram_create(sc_allocator_t *alloc, const char *business_account_id,
                               size_t business_account_id_len, const char *access_token,
                               size_t access_token_len, const char *app_secret,
                               size_t app_secret_len, sc_channel_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_instagram_ctx_t *c = (sc_instagram_ctx_t *)calloc(1, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    if (business_account_id && business_account_id_len > 0) {
        c->business_account_id = sc_strndup(alloc, business_account_id, business_account_id_len);
        if (!c->business_account_id) {
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        c->business_account_id_len = business_account_id_len;
    }
    if (access_token && access_token_len > 0) {
        c->access_token = sc_strndup(alloc, access_token, access_token_len);
        if (!c->access_token) {
            if (c->business_account_id)
                sc_str_free(alloc, c->business_account_id);
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        c->access_token_len = access_token_len;
    }
    if (app_secret && app_secret_len > 0) {
        c->app_secret = sc_strndup(alloc, app_secret, app_secret_len);
        if (!c->app_secret) {
            if (c->access_token)
                sc_str_free(alloc, c->access_token);
            if (c->business_account_id)
                sc_str_free(alloc, c->business_account_id);
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        c->app_secret_len = app_secret_len;
    }
    out->ctx = c;
    out->vtable = &instagram_vtable;
    return SC_OK;
}

void sc_instagram_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_instagram_ctx_t *c = (sc_instagram_ctx_t *)ch->ctx;
        if (c->alloc) {
            if (c->business_account_id)
                sc_str_free(c->alloc, c->business_account_id);
            if (c->access_token)
                sc_str_free(c->alloc, c->access_token);
            if (c->app_secret)
                sc_str_free(c->alloc, c->app_secret);
        }
        free(c);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
