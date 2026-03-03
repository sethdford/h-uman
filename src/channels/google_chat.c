#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GCHAT_API_BASE       "https://chat.googleapis.com/v1/"
#define GCHAT_QUEUE_MAX       32
#define GCHAT_SESSION_KEY_MAX 127
#define GCHAT_CONTENT_MAX     4095

typedef struct sc_google_chat_queued_msg {
    char session_key[128];
    char content[4096];
} sc_google_chat_queued_msg_t;

typedef struct sc_google_chat_ctx {
    sc_allocator_t *alloc;
    char *bearer_token;
    size_t bearer_token_len;
    char *space_id;
    size_t space_id_len;
    bool running;
    sc_google_chat_queued_msg_t queue[GCHAT_QUEUE_MAX];
    size_t queue_head;
    size_t queue_tail;
    size_t queue_count;
} sc_google_chat_ctx_t;

static sc_error_t google_chat_start(void *ctx) {
    sc_google_chat_ctx_t *c = (sc_google_chat_ctx_t *)ctx;
    if (!c)
        return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void google_chat_stop(void *ctx) {
    sc_google_chat_ctx_t *c = (sc_google_chat_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static sc_error_t google_chat_send(void *ctx, const char *target, size_t target_len,
                                   const char *message, size_t message_len,
                                   const char *const *media, size_t media_count) {
    (void)media;
    (void)media_count;
    sc_google_chat_ctx_t *c = (sc_google_chat_ctx_t *)ctx;

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
    if (!c->bearer_token || c->bearer_token_len == 0)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;
    if (!message)
        return SC_ERR_INVALID_ARGUMENT;

    const char *space = (target && target_len > 0) ? target : c->space_id;
    size_t space_len = (target && target_len > 0) ? target_len : c->space_id_len;
    if (!space || space_len == 0)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;

    char url_buf[512];
    int n = snprintf(url_buf, sizeof(url_buf), "%s%.*s/messages", GCHAT_API_BASE, (int)space_len,
                     space);
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return SC_ERR_INTERNAL;

    sc_json_buf_t jbuf;
    sc_error_t err = sc_json_buf_init(&jbuf, c->alloc);
    if (err)
        return err;
    err = sc_json_buf_append_raw(&jbuf, "{", 1);
    if (err)
        goto jfail;
    err = sc_json_append_key_value(&jbuf, "text", 4, message, message_len);
    if (err)
        goto jfail;
    err = sc_json_buf_append_raw(&jbuf, "}", 1);
    if (err)
        goto jfail;

    char auth_buf[512];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)c->bearer_token_len,
                      c->bearer_token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf)) {
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

static const char *google_chat_name(void *ctx) {
    (void)ctx;
    return "google_chat";
}
static bool google_chat_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static const sc_channel_vtable_t google_chat_vtable = {
    .start = google_chat_start,
    .stop = google_chat_stop,
    .send = google_chat_send,
    .name = google_chat_name,
    .health_check = google_chat_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

static void google_chat_queue_push(sc_google_chat_ctx_t *c, const char *from, size_t from_len,
                                   const char *body, size_t body_len) {
    if (c->queue_count >= GCHAT_QUEUE_MAX)
        return;
    sc_google_chat_queued_msg_t *slot = &c->queue[c->queue_tail];
    size_t sk = from_len < GCHAT_SESSION_KEY_MAX ? from_len : GCHAT_SESSION_KEY_MAX;
    memcpy(slot->session_key, from, sk);
    slot->session_key[sk] = '\0';
    size_t ct = body_len < GCHAT_CONTENT_MAX ? body_len : GCHAT_CONTENT_MAX;
    memcpy(slot->content, body, ct);
    slot->content[ct] = '\0';
    c->queue_tail = (c->queue_tail + 1) % GCHAT_QUEUE_MAX;
    c->queue_count++;
}

sc_error_t sc_google_chat_on_webhook(void *channel_ctx, sc_allocator_t *alloc, const char *body,
                                     size_t body_len) {
    sc_google_chat_ctx_t *c = (sc_google_chat_ctx_t *)channel_ctx;
    if (!c || !body || body_len == 0)
        return SC_ERR_INVALID_ARGUMENT;
#if SC_IS_TEST
    (void)alloc;
    google_chat_queue_push(c, "test-sender", 11, body, body_len);
    return SC_OK;
#else
    sc_json_value_t *parsed = NULL;
    sc_error_t err = sc_json_parse(alloc, body, body_len, &parsed);
    if (err != SC_OK || !parsed)
        return SC_OK;
    sc_json_value_t *msg = sc_json_object_get(parsed, "message");
    if (msg && msg->type == SC_JSON_OBJECT) {
        const char *text = sc_json_get_string(msg, "text");
        sc_json_value_t *sender = sc_json_object_get(msg, "sender");
        const char *sender_name = sender ? sc_json_get_string(sender, "name") : NULL;
        if (text && sender_name && strlen(text) > 0)
            google_chat_queue_push(c, sender_name, strlen(sender_name), text, strlen(text));
    }
    sc_json_free(alloc, parsed);
    return SC_OK;
#endif
}

sc_error_t sc_google_chat_poll(void *channel_ctx, sc_allocator_t *alloc,
                               sc_channel_loop_msg_t *msgs, size_t max_msgs, size_t *out_count) {
    (void)alloc;
    sc_google_chat_ctx_t *c = (sc_google_chat_ctx_t *)channel_ctx;
    if (!c || !msgs || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    size_t cnt = 0;
    while (c->queue_count > 0 && cnt < max_msgs) {
        sc_google_chat_queued_msg_t *slot = &c->queue[c->queue_head];
        memcpy(msgs[cnt].session_key, slot->session_key, sizeof(slot->session_key));
        memcpy(msgs[cnt].content, slot->content, sizeof(slot->content));
        c->queue_head = (c->queue_head + 1) % GCHAT_QUEUE_MAX;
        c->queue_count--;
        cnt++;
    }
    *out_count = cnt;
    return SC_OK;
}

sc_error_t sc_google_chat_create(sc_allocator_t *alloc, const char *bearer_token,
                                 size_t bearer_token_len, const char *space_id,
                                 size_t space_id_len, sc_channel_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_google_chat_ctx_t *c = (sc_google_chat_ctx_t *)calloc(1, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    if (bearer_token && bearer_token_len > 0) {
        c->bearer_token = (char *)malloc(bearer_token_len + 1);
        if (!c->bearer_token) {
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->bearer_token, bearer_token, bearer_token_len);
        c->bearer_token[bearer_token_len] = '\0';
        c->bearer_token_len = bearer_token_len;
    }
    if (space_id && space_id_len > 0) {
        c->space_id = (char *)malloc(space_id_len + 1);
        if (!c->space_id) {
            if (c->bearer_token)
                free(c->bearer_token);
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->space_id, space_id, space_id_len);
        c->space_id[space_id_len] = '\0';
        c->space_id_len = space_id_len;
    }
    out->ctx = c;
    out->vtable = &google_chat_vtable;
    return SC_OK;
}

void sc_google_chat_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_google_chat_ctx_t *c = (sc_google_chat_ctx_t *)ch->ctx;
        if (c->bearer_token)
            free(c->bearer_token);
        if (c->space_id)
            free(c->space_id);
        free(c);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
