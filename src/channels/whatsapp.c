#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WHATSAPP_API_BASE "https://graph.facebook.com/v18.0/"

#define WHATSAPP_QUEUE_MAX       32
#define WHATSAPP_SESSION_KEY_MAX 127
#define WHATSAPP_CONTENT_MAX     4095

typedef struct sc_whatsapp_queued_msg {
    char session_key[128];
    char content[4096];
} sc_whatsapp_queued_msg_t;

typedef struct sc_whatsapp_ctx {
    sc_allocator_t *alloc;
    char *phone_number_id;
    size_t phone_number_id_len;
    char *token;
    size_t token_len;
    bool running;
    sc_whatsapp_queued_msg_t queue[WHATSAPP_QUEUE_MAX];
    size_t queue_head;
    size_t queue_tail;
    size_t queue_count;
} sc_whatsapp_ctx_t;

static sc_error_t whatsapp_start(void *ctx) {
    sc_whatsapp_ctx_t *c = (sc_whatsapp_ctx_t *)ctx;
    if (!c)
        return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void whatsapp_stop(void *ctx) {
    sc_whatsapp_ctx_t *c = (sc_whatsapp_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static sc_error_t whatsapp_send(void *ctx, const char *target, size_t target_len,
                                const char *message, size_t message_len, const char *const *media,
                                size_t media_count) {
    sc_whatsapp_ctx_t *c = (sc_whatsapp_ctx_t *)ctx;

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
    if (!c->phone_number_id || c->phone_number_id_len == 0)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;
    if (!c->token || c->token_len == 0)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;
    if (!target || target_len == 0 || !message)
        return SC_ERR_INVALID_ARGUMENT;

    char url_buf[512];
    int n = snprintf(url_buf, sizeof(url_buf), "%s%.*s/messages", WHATSAPP_API_BASE,
                     (int)c->phone_number_id_len, c->phone_number_id);
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return SC_ERR_INTERNAL;

    sc_json_buf_t jbuf;
    sc_error_t err = sc_json_buf_init(&jbuf, c->alloc);
    if (err)
        return err;

    err = sc_json_buf_append_raw(&jbuf, "{\"messaging_product\":\"whatsapp\",", 32);
    if (err)
        goto jfail;
    err = sc_json_append_key_value(&jbuf, "to", 2, target, target_len);
    if (err)
        goto jfail;
    err = sc_json_buf_append_raw(&jbuf, ",\"type\":\"text\",\"text\":{", 23);
    if (err)
        goto jfail;
    err = sc_json_append_key_value(&jbuf, "body", 4, message, message_len);
    if (err)
        goto jfail;
    err = sc_json_buf_append_raw(&jbuf, "}}", 2);
    if (err)
        goto jfail;

    char auth_buf[512];
    n = snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)c->token_len, c->token);
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

    sc_json_buf_t img_buf;
    for (size_t i = 0; i < media_count && media && media[i]; i++) {
        err = sc_json_buf_init(&img_buf, c->alloc);
        if (err)
            return err;
        err = sc_json_buf_append_raw(&img_buf, "{\"messaging_product\":\"whatsapp\",", 32);
        if (err)
            goto img_fail;
        err = sc_json_append_key_value(&img_buf, "to", 2, target, target_len);
        if (err)
            goto img_fail;
        err = sc_json_buf_append_raw(&img_buf, ",\"type\":\"image\",\"image\":{", 26);
        if (err)
            goto img_fail;
        err = sc_json_append_key_value(&img_buf, "link", 4, media[i], strlen(media[i]));
        if (err)
            goto img_fail;
        err = sc_json_buf_append_raw(&img_buf, "}}", 2);
        if (err)
            goto img_fail;

        sc_http_response_t img_resp = {0};
        err = sc_http_post_json(c->alloc, url_buf, auth_buf, img_buf.ptr, img_buf.len, &img_resp);
        sc_json_buf_free(&img_buf);
        if (err != SC_OK) {
            if (img_resp.owned && img_resp.body)
                sc_http_response_free(c->alloc, &img_resp);
            return SC_ERR_CHANNEL_SEND;
        }
        if (img_resp.owned && img_resp.body)
            sc_http_response_free(c->alloc, &img_resp);
        if (img_resp.status_code < 200 || img_resp.status_code >= 300)
            return SC_ERR_CHANNEL_SEND;
    }
    return SC_OK;
img_fail:
    sc_json_buf_free(&img_buf);
    return err;
jfail:
    sc_json_buf_free(&jbuf);
    return err;
#endif
}

static void whatsapp_queue_push(sc_whatsapp_ctx_t *c, const char *from, size_t from_len,
                                const char *body, size_t body_len) {
    if (c->queue_count >= WHATSAPP_QUEUE_MAX)
        return;
    sc_whatsapp_queued_msg_t *slot = &c->queue[c->queue_tail];
    size_t sk = from_len < WHATSAPP_SESSION_KEY_MAX ? from_len : WHATSAPP_SESSION_KEY_MAX;
    memcpy(slot->session_key, from, sk);
    slot->session_key[sk] = '\0';
    size_t ct = body_len < WHATSAPP_CONTENT_MAX ? body_len : WHATSAPP_CONTENT_MAX;
    memcpy(slot->content, body, ct);
    slot->content[ct] = '\0';
    c->queue_tail = (c->queue_tail + 1) % WHATSAPP_QUEUE_MAX;
    c->queue_count++;
}

sc_error_t sc_whatsapp_on_webhook(void *channel_ctx, sc_allocator_t *alloc, const char *body,
                                  size_t body_len) {
    sc_whatsapp_ctx_t *c = (sc_whatsapp_ctx_t *)channel_ctx;
    if (!c || !body || body_len == 0)
        return SC_ERR_INVALID_ARGUMENT;
#if SC_IS_TEST
    (void)alloc;
    whatsapp_queue_push(c, "test-sender", 11, body, body_len);
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
        sc_json_value_t *changes = sc_json_object_get(ent, "changes");
        if (!changes || changes->type != SC_JSON_ARRAY)
            continue;
        for (size_t ch = 0; ch < changes->data.array.len; ch++) {
            sc_json_value_t *change = changes->data.array.items[ch];
            if (!change || change->type != SC_JSON_OBJECT)
                continue;
            sc_json_value_t *value = sc_json_object_get(change, "value");
            if (!value || value->type != SC_JSON_OBJECT)
                continue;
            sc_json_value_t *messages = sc_json_object_get(value, "messages");
            if (!messages || messages->type != SC_JSON_ARRAY)
                continue;
            for (size_t m = 0; m < messages->data.array.len; m++) {
                sc_json_value_t *msg = messages->data.array.items[m];
                if (!msg || msg->type != SC_JSON_OBJECT)
                    continue;
                const char *msg_type = sc_json_get_string(msg, "type");
                if (!msg_type || strcmp(msg_type, "text") != 0)
                    continue;
                const char *from = sc_json_get_string(msg, "from");
                if (!from)
                    continue;
                sc_json_value_t *text_obj = sc_json_object_get(msg, "text");
                if (!text_obj || text_obj->type != SC_JSON_OBJECT)
                    continue;
                const char *text_body = sc_json_get_string(text_obj, "body");
                if (!text_body || strlen(text_body) == 0)
                    continue;
                whatsapp_queue_push(c, from, strlen(from), text_body, strlen(text_body));
            }
        }
    }
    sc_json_free(alloc, parsed);
    return SC_OK;
#endif
}

sc_error_t sc_whatsapp_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                            size_t max_msgs, size_t *out_count) {
    (void)alloc;
    sc_whatsapp_ctx_t *c = (sc_whatsapp_ctx_t *)channel_ctx;
    if (!c || !msgs || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    size_t cnt = 0;
    while (c->queue_count > 0 && cnt < max_msgs) {
        sc_whatsapp_queued_msg_t *slot = &c->queue[c->queue_head];
        memcpy(msgs[cnt].session_key, slot->session_key, sizeof(slot->session_key));
        memcpy(msgs[cnt].content, slot->content, sizeof(slot->content));
        c->queue_head = (c->queue_head + 1) % WHATSAPP_QUEUE_MAX;
        c->queue_count--;
        cnt++;
    }
    *out_count = cnt;
    return SC_OK;
}

static const char *whatsapp_name(void *ctx) {
    (void)ctx;
    return "whatsapp";
}
static bool whatsapp_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static const sc_channel_vtable_t whatsapp_vtable = {
    .start = whatsapp_start,
    .stop = whatsapp_stop,
    .send = whatsapp_send,
    .name = whatsapp_name,
    .health_check = whatsapp_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

sc_error_t sc_whatsapp_create(sc_allocator_t *alloc, const char *phone_number_id,
                              size_t phone_number_id_len, const char *token, size_t token_len,
                              sc_channel_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_whatsapp_ctx_t *c = (sc_whatsapp_ctx_t *)calloc(1, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    if (phone_number_id && phone_number_id_len > 0) {
        c->phone_number_id = (char *)malloc(phone_number_id_len + 1);
        if (!c->phone_number_id) {
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
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
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->token, token, token_len);
        c->token[token_len] = '\0';
        c->token_len = token_len;
    }
    out->ctx = c;
    out->vtable = &whatsapp_vtable;
    return SC_OK;
}

void sc_whatsapp_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_whatsapp_ctx_t *c = (sc_whatsapp_ctx_t *)ch->ctx;
        if (c->phone_number_id)
            free(c->phone_number_id);
        if (c->token)
            free(c->token);
        free(c);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
