#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WHATSAPP_API_BASE "https://graph.facebook.com/v18.0/"

#define WHATSAPP_QUEUE_MAX       32
#define WHATSAPP_SESSION_KEY_MAX 127
#define WHATSAPP_CONTENT_MAX     4095

typedef struct hu_whatsapp_queued_msg {
    char session_key[128];
    char content[4096];
} hu_whatsapp_queued_msg_t;

typedef struct hu_whatsapp_ctx {
    hu_allocator_t *alloc;
    char *phone_number_id;
    size_t phone_number_id_len;
    char *token;
    size_t token_len;
    bool running;
    hu_whatsapp_queued_msg_t queue[WHATSAPP_QUEUE_MAX];
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
} hu_whatsapp_ctx_t;

static hu_error_t whatsapp_start(void *ctx) {
    hu_whatsapp_ctx_t *c = (hu_whatsapp_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
    return HU_OK;
}

static void whatsapp_stop(void *ctx) {
    hu_whatsapp_ctx_t *c = (hu_whatsapp_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static hu_error_t whatsapp_send(void *ctx, const char *target, size_t target_len,
                                const char *message, size_t message_len, const char *const *media,
                                size_t media_count) {
    hu_whatsapp_ctx_t *c = (hu_whatsapp_ctx_t *)ctx;

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
    if (!c->phone_number_id || c->phone_number_id_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    if (!c->token || c->token_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    if (!target || target_len == 0 || !message)
        return HU_ERR_INVALID_ARGUMENT;

    char url_buf[512];
    int n = snprintf(url_buf, sizeof(url_buf), "%s%.*s/messages", WHATSAPP_API_BASE,
                     (int)c->phone_number_id_len, c->phone_number_id);
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return HU_ERR_INTERNAL;

    hu_json_buf_t jbuf;
    hu_error_t err = hu_json_buf_init(&jbuf, c->alloc);
    if (err)
        return err;

    err = hu_json_buf_append_raw(&jbuf, "{\"messaging_product\":\"whatsapp\",", 32);
    if (err)
        goto jfail;
    err = hu_json_append_key_value(&jbuf, "to", 2, target, target_len);
    if (err)
        goto jfail;
    err = hu_json_buf_append_raw(&jbuf, ",\"type\":\"text\",\"text\":{", 23);
    if (err)
        goto jfail;
    err = hu_json_append_key_value(&jbuf, "body", 4, message, message_len);
    if (err)
        goto jfail;
    err = hu_json_buf_append_raw(&jbuf, "}}", 2);
    if (err)
        goto jfail;

    char auth_buf[512];
    n = snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)c->token_len, c->token);
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

    hu_json_buf_t img_buf;
    for (size_t i = 0; i < media_count && media && media[i]; i++) {
        err = hu_json_buf_init(&img_buf, c->alloc);
        if (err)
            return err;
        err = hu_json_buf_append_raw(&img_buf, "{\"messaging_product\":\"whatsapp\",", 32);
        if (err)
            goto img_fail;
        err = hu_json_append_key_value(&img_buf, "to", 2, target, target_len);
        if (err)
            goto img_fail;
        err = hu_json_buf_append_raw(&img_buf, ",\"type\":\"image\",\"image\":{", 26);
        if (err)
            goto img_fail;
        err = hu_json_append_key_value(&img_buf, "link", 4, media[i], strlen(media[i]));
        if (err)
            goto img_fail;
        err = hu_json_buf_append_raw(&img_buf, "}}", 2);
        if (err)
            goto img_fail;

        hu_http_response_t img_resp = {0};
        err = hu_http_post_json(c->alloc, url_buf, auth_buf, img_buf.ptr, img_buf.len, &img_resp);
        hu_json_buf_free(&img_buf);
        if (err != HU_OK) {
            if (img_resp.owned && img_resp.body)
                hu_http_response_free(c->alloc, &img_resp);
            return HU_ERR_CHANNEL_SEND;
        }
        if (img_resp.owned && img_resp.body)
            hu_http_response_free(c->alloc, &img_resp);
        if (img_resp.status_code < 200 || img_resp.status_code >= 300)
            return HU_ERR_CHANNEL_SEND;
    }
    return HU_OK;
img_fail:
    hu_json_buf_free(&img_buf);
    return err;
jfail:
    hu_json_buf_free(&jbuf);
    return err;
#endif
}

static void whatsapp_queue_push(hu_whatsapp_ctx_t *c, const char *from, size_t from_len,
                                const char *body, size_t body_len) {
    if (c->queue_count >= WHATSAPP_QUEUE_MAX)
        return;
    hu_whatsapp_queued_msg_t *slot = &c->queue[c->queue_tail];
    size_t sk = from_len < WHATSAPP_SESSION_KEY_MAX ? from_len : WHATSAPP_SESSION_KEY_MAX;
    memcpy(slot->session_key, from, sk);
    slot->session_key[sk] = '\0';
    size_t ct = body_len < WHATSAPP_CONTENT_MAX ? body_len : WHATSAPP_CONTENT_MAX;
    memcpy(slot->content, body, ct);
    slot->content[ct] = '\0';
    c->queue_tail = (c->queue_tail + 1) % WHATSAPP_QUEUE_MAX;
    c->queue_count++;
}

hu_error_t hu_whatsapp_on_webhook(void *channel_ctx, hu_allocator_t *alloc, const char *body,
                                  size_t body_len) {
    hu_whatsapp_ctx_t *c = (hu_whatsapp_ctx_t *)channel_ctx;
    if (!c || !body || body_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    (void)alloc;
    whatsapp_queue_push(c, "test-sender", 11, body, body_len);
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
        hu_json_value_t *changes = hu_json_object_get(ent, "changes");
        if (!changes || changes->type != HU_JSON_ARRAY)
            continue;
        for (size_t ch = 0; ch < changes->data.array.len; ch++) {
            hu_json_value_t *change = changes->data.array.items[ch];
            if (!change || change->type != HU_JSON_OBJECT)
                continue;
            hu_json_value_t *value = hu_json_object_get(change, "value");
            if (!value || value->type != HU_JSON_OBJECT)
                continue;
            hu_json_value_t *messages = hu_json_object_get(value, "messages");
            if (!messages || messages->type != HU_JSON_ARRAY)
                continue;
            for (size_t m = 0; m < messages->data.array.len; m++) {
                hu_json_value_t *msg = messages->data.array.items[m];
                if (!msg || msg->type != HU_JSON_OBJECT)
                    continue;
                const char *msg_type = hu_json_get_string(msg, "type");
                if (!msg_type || strcmp(msg_type, "text") != 0)
                    continue;
                const char *from = hu_json_get_string(msg, "from");
                if (!from)
                    continue;
                hu_json_value_t *text_obj = hu_json_object_get(msg, "text");
                if (!text_obj || text_obj->type != HU_JSON_OBJECT)
                    continue;
                const char *text_body = hu_json_get_string(text_obj, "body");
                if (!text_body || strlen(text_body) == 0)
                    continue;
                whatsapp_queue_push(c, from, strlen(from), text_body, strlen(text_body));
            }
        }
    }
    hu_json_free(alloc, parsed);
    return HU_OK;
#endif
}

hu_error_t hu_whatsapp_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                            size_t max_msgs, size_t *out_count) {
    (void)alloc;
    hu_whatsapp_ctx_t *c = (hu_whatsapp_ctx_t *)channel_ctx;
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
        hu_whatsapp_queued_msg_t *slot = &c->queue[c->queue_head];
        memcpy(msgs[cnt].session_key, slot->session_key, sizeof(slot->session_key));
        memcpy(msgs[cnt].content, slot->content, sizeof(slot->content));
        c->queue_head = (c->queue_head + 1) % WHATSAPP_QUEUE_MAX;
        c->queue_count--;
        cnt++;
    }
    *out_count = cnt;
    return HU_OK;
}

static const char *whatsapp_name(void *ctx) {
    (void)ctx;
    return "whatsapp";
}
static bool whatsapp_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static const hu_channel_vtable_t whatsapp_vtable = {
    .start = whatsapp_start,
    .stop = whatsapp_stop,
    .send = whatsapp_send,
    .name = whatsapp_name,
    .health_check = whatsapp_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

hu_error_t hu_whatsapp_create(hu_allocator_t *alloc, const char *phone_number_id,
                              size_t phone_number_id_len, const char *token, size_t token_len,
                              hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_whatsapp_ctx_t *c = (hu_whatsapp_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    if (phone_number_id && phone_number_id_len > 0) {
        c->phone_number_id = (char *)malloc(phone_number_id_len + 1);
        if (!c->phone_number_id) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->phone_number_id, phone_number_id, phone_number_id_len);
        c->phone_number_id[phone_number_id_len] = '\0';
        c->phone_number_id_len = phone_number_id_len;
    }
    if (token && token_len > 0) {
        c->token = (char *)malloc(token_len + 1);
        if (!c->token) {
            if (c->phone_number_id)
                free(c->phone_number_id);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->token, token, token_len);
        c->token[token_len] = '\0';
        c->token_len = token_len;
    }
    out->ctx = c;
    out->vtable = &whatsapp_vtable;
    return HU_OK;
}

void hu_whatsapp_destroy(hu_channel_t *ch) {
    if (ch && ch->ctx) {
        hu_whatsapp_ctx_t *c = (hu_whatsapp_ctx_t *)ch->ctx;
        hu_allocator_t *a = c->alloc;
        if (c->phone_number_id)
            free(c->phone_number_id);
        if (c->token)
            free(c->token);
        a->free(a->ctx, c, sizeof(*c));
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

#if HU_IS_TEST
hu_error_t hu_whatsapp_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                        size_t session_key_len, const char *content,
                                        size_t content_len) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_whatsapp_ctx_t *c = (hu_whatsapp_ctx_t *)ch->ctx;
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
const char *hu_whatsapp_test_get_last_message(hu_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    hu_whatsapp_ctx_t *c = (hu_whatsapp_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif
