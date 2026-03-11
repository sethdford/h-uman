#include "human/channels/instagram.h"
#include "human/channel.h"
#include "human/channels/meta_common.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <stdio.h>
#include <string.h>

#define INSTAGRAM_ENDPOINT        "me/messages"
#define INSTAGRAM_ENDPOINT_LEN    12
#define INSTAGRAM_QUEUE_MAX       32
#define INSTAGRAM_SESSION_KEY_MAX 127
#define INSTAGRAM_CONTENT_MAX     4095

typedef struct hu_instagram_queued_msg {
    char session_key[128];
    char content[4096];
} hu_instagram_queued_msg_t;

typedef struct hu_instagram_ctx {
    hu_allocator_t *alloc;
    char *business_account_id;
    size_t business_account_id_len;
    char *access_token;
    size_t access_token_len;
    char *app_secret;
    size_t app_secret_len;
    bool running;
    hu_instagram_queued_msg_t queue[INSTAGRAM_QUEUE_MAX];
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
} hu_instagram_ctx_t;

static hu_error_t instagram_start(void *ctx) {
    hu_instagram_ctx_t *c = (hu_instagram_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
    return HU_OK;
}

static void instagram_stop(void *ctx) {
    hu_instagram_ctx_t *c = (hu_instagram_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static hu_error_t instagram_send(void *ctx, const char *target, size_t target_len,
                                 const char *message, size_t message_len, const char *const *media,
                                 size_t media_count) {
    (void)target;
    (void)target_len;
    (void)media;
    (void)media_count;
    hu_instagram_ctx_t *c = (hu_instagram_ctx_t *)ctx;

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
    if (!c->access_token || c->access_token_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    if (!target || target_len == 0 || !message)
        return HU_ERR_INVALID_ARGUMENT;

    hu_json_buf_t jbuf;
    hu_error_t err = hu_json_buf_init(&jbuf, c->alloc);
    if (err)
        return err;

    err = hu_json_buf_append_raw(&jbuf, "{\"recipient\":{\"id\":\"", 21);
    if (err)
        goto jfail;
    err = hu_json_append_string(&jbuf, target, target_len);
    if (err)
        goto jfail;
    err = hu_json_buf_append_raw(&jbuf, "\"},\"message\":{\"text\":\"", 24);
    if (err)
        goto jfail;
    err = hu_json_append_string(&jbuf, message, message_len);
    if (err)
        goto jfail;
    err = hu_json_buf_append_raw(&jbuf, "\"}}", 3);
    if (err)
        goto jfail;

    err = hu_meta_graph_send(c->alloc, c->access_token, c->access_token_len, INSTAGRAM_ENDPOINT,
                             INSTAGRAM_ENDPOINT_LEN, jbuf.ptr, jbuf.len);
    hu_json_buf_free(&jbuf);
    if (err != HU_OK)
        return HU_ERR_CHANNEL_SEND;
    return HU_OK;
jfail:
    hu_json_buf_free(&jbuf);
    return err;
#endif
}

static void instagram_queue_push(hu_instagram_ctx_t *c, const char *from, size_t from_len,
                                 const char *body, size_t body_len) {
    if (!c || !from || !body || c->queue_count >= INSTAGRAM_QUEUE_MAX)
        return;
    hu_instagram_queued_msg_t *slot = &c->queue[c->queue_tail];
    size_t sk = from_len < INSTAGRAM_SESSION_KEY_MAX ? from_len : INSTAGRAM_SESSION_KEY_MAX;
    memcpy(slot->session_key, from, sk);
    slot->session_key[sk] = '\0';
    size_t ct = body_len < INSTAGRAM_CONTENT_MAX ? body_len : INSTAGRAM_CONTENT_MAX;
    memcpy(slot->content, body, ct);
    slot->content[ct] = '\0';
    c->queue_tail = (c->queue_tail + 1) % INSTAGRAM_QUEUE_MAX;
    c->queue_count++;
}

hu_error_t hu_instagram_on_webhook(void *channel_ctx, hu_allocator_t *alloc, const char *body,
                                   size_t body_len) {
    hu_instagram_ctx_t *c = (hu_instagram_ctx_t *)channel_ctx;
    if (!c || !body || body_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    (void)alloc;
    instagram_queue_push(c, "test-sender", 11, body, body_len);
    return HU_OK;
#else
    hu_json_value_t *parsed = NULL;
    hu_error_t err = hu_json_parse(alloc, body, body_len, &parsed);
    if (err != HU_OK || !parsed)
        return HU_OK;
    const char *obj_type = hu_json_get_string(parsed, "object");
    if (!obj_type || strcmp(obj_type, "instagram") != 0) {
        hu_json_free(alloc, parsed);
        return HU_OK;
    }
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
            if (hu_json_get_bool(msg, "is_echo", false))
                continue;
            hu_json_value_t *message_obj = hu_json_object_get(msg, "message");
            if (!message_obj || message_obj->type != HU_JSON_OBJECT)
                continue;
            const char *text_body = hu_json_get_string(message_obj, "text");
            if (!text_body || strlen(text_body) == 0)
                continue;
            hu_json_value_t *sender = hu_json_object_get(msg, "sender");
            if (!sender || sender->type != HU_JSON_OBJECT)
                continue;
            const char *from = hu_json_get_string(sender, "id");
            if (!from)
                continue;
            instagram_queue_push(c, from, strlen(from), text_body, strlen(text_body));
        }
    }
    hu_json_free(alloc, parsed);
    return HU_OK;
#endif
}

hu_error_t hu_instagram_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                             size_t max_msgs, size_t *out_count) {
    (void)alloc;
    hu_instagram_ctx_t *c = (hu_instagram_ctx_t *)channel_ctx;
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
        hu_instagram_queued_msg_t *slot = &c->queue[c->queue_head];
        memcpy(msgs[cnt].session_key, slot->session_key, sizeof(slot->session_key));
        memcpy(msgs[cnt].content, slot->content, sizeof(slot->content));
        c->queue_head = (c->queue_head + 1) % INSTAGRAM_QUEUE_MAX;
        c->queue_count--;
        cnt++;
    }
    *out_count = cnt;
    return HU_OK;
}

static const char *instagram_name(void *ctx) {
    (void)ctx;
    return "instagram";
}
static bool instagram_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static const hu_channel_vtable_t instagram_vtable = {
    .start = instagram_start,
    .stop = instagram_stop,
    .send = instagram_send,
    .name = instagram_name,
    .health_check = instagram_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

hu_error_t hu_instagram_create(hu_allocator_t *alloc, const char *business_account_id,
                               size_t business_account_id_len, const char *access_token,
                               size_t access_token_len, const char *app_secret,
                               size_t app_secret_len, hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_instagram_ctx_t *c = (hu_instagram_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    if (business_account_id && business_account_id_len > 0) {
        c->business_account_id = hu_strndup(alloc, business_account_id, business_account_id_len);
        if (!c->business_account_id) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        c->business_account_id_len = business_account_id_len;
    }
    if (access_token && access_token_len > 0) {
        c->access_token = hu_strndup(alloc, access_token, access_token_len);
        if (!c->access_token) {
            if (c->business_account_id)
                hu_str_free(alloc, c->business_account_id);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        c->access_token_len = access_token_len;
    }
    if (app_secret && app_secret_len > 0) {
        c->app_secret = hu_strndup(alloc, app_secret, app_secret_len);
        if (!c->app_secret) {
            if (c->access_token)
                hu_str_free(alloc, c->access_token);
            if (c->business_account_id)
                hu_str_free(alloc, c->business_account_id);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        c->app_secret_len = app_secret_len;
    }
    out->ctx = c;
    out->vtable = &instagram_vtable;
    return HU_OK;
}

void hu_instagram_destroy(hu_channel_t *ch) {
    if (ch && ch->ctx) {
        hu_instagram_ctx_t *c = (hu_instagram_ctx_t *)ch->ctx;
        hu_allocator_t *a = c->alloc;
        if (a) {
            if (c->business_account_id)
                hu_str_free(a, c->business_account_id);
            if (c->access_token)
                hu_str_free(a, c->access_token);
            if (c->app_secret)
                hu_str_free(a, c->app_secret);
            a->free(a->ctx, c, sizeof(*c));
        }
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

#if HU_IS_TEST
hu_error_t hu_instagram_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                         size_t session_key_len, const char *content,
                                         size_t content_len) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_instagram_ctx_t *c = (hu_instagram_ctx_t *)ch->ctx;
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
const char *hu_instagram_test_get_last_message(hu_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    hu_instagram_ctx_t *c = (hu_instagram_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif
