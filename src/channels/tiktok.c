/*
 * TikTok channel — webhook-based inbound (comment events), Content Posting API outbound.
 * TikTok does not expose a DM API; interactions are comment/mention based.
 */
#include "human/channels/tiktok.h"
#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TIKTOK_API_BASE        "https://open.tiktokapis.com/v2"
#define TIKTOK_QUEUE_MAX       32
#define TIKTOK_SESSION_KEY_MAX 127
#define TIKTOK_CONTENT_MAX     4095

typedef struct hu_tiktok_queued_msg {
    char session_key[128];
    char content[4096];
} hu_tiktok_queued_msg_t;

typedef struct hu_tiktok_ctx {
    hu_allocator_t *alloc;
    char *client_key;
    size_t client_key_len;
    char *client_secret;
    size_t client_secret_len;
    char *access_token;
    size_t access_token_len;
    bool running;
    hu_tiktok_queued_msg_t queue[TIKTOK_QUEUE_MAX];
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
} hu_tiktok_ctx_t;

static hu_error_t tiktok_start(void *ctx) {
    hu_tiktok_ctx_t *c = (hu_tiktok_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
    return HU_OK;
}

static void tiktok_stop(void *ctx) {
    hu_tiktok_ctx_t *c = (hu_tiktok_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static hu_error_t tiktok_send(void *ctx, const char *target, size_t target_len, const char *message,
                              size_t message_len, const char *const *media, size_t media_count) {
    (void)media;
    (void)media_count;
    hu_tiktok_ctx_t *c = (hu_tiktok_ctx_t *)ctx;

#if HU_IS_TEST
    {
        (void)target;
        (void)target_len;
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

    char url_buf[256];
    int n = snprintf(url_buf, sizeof(url_buf), "%s/post/publish/content/init/", TIKTOK_API_BASE);
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return HU_ERR_INTERNAL;

    hu_json_buf_t jbuf;
    hu_error_t err = hu_json_buf_init(&jbuf, c->alloc);
    if (err)
        return err;

    err = hu_json_buf_append_raw(&jbuf, "{\"post_info\":{\"title\":", 21);
    if (err)
        goto jfail;
    err = hu_json_append_string(&jbuf, message, message_len);
    if (err)
        goto jfail;
    err = hu_json_buf_append_raw(
        &jbuf,
        ",\"privacy_level\":\"SELF_ONLY\",\"disable_comment\":false},\"source_info\":{\"source\":"
        "\"PULL_FROM_URL\",\"video_url\":",
        120);
    if (err)
        goto jfail;
    err = hu_json_append_string(&jbuf, target, target_len);
    if (err)
        goto jfail;
    err = hu_json_buf_append_raw(&jbuf, "}}", 2);
    if (err)
        goto jfail;

    char auth_buf[512];
    n = snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)c->access_token_len,
                 c->access_token);
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

static const char *tiktok_name(void *ctx) {
    (void)ctx;
    return "tiktok";
}

static bool tiktok_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static const hu_channel_vtable_t tiktok_vtable = {
    .start = tiktok_start,
    .stop = tiktok_stop,
    .send = tiktok_send,
    .name = tiktok_name,
    .health_check = tiktok_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

/* ─── Webhook inbound queue ────────────────────────────────────────────── */

static void tiktok_queue_push(hu_tiktok_ctx_t *c, const char *from, size_t from_len,
                              const char *body, size_t body_len) {
    if (c->queue_count >= TIKTOK_QUEUE_MAX)
        return;
    hu_tiktok_queued_msg_t *slot = &c->queue[c->queue_tail];
    size_t sk = from_len < TIKTOK_SESSION_KEY_MAX ? from_len : TIKTOK_SESSION_KEY_MAX;
    memcpy(slot->session_key, from, sk);
    slot->session_key[sk] = '\0';
    size_t ct = body_len < TIKTOK_CONTENT_MAX ? body_len : TIKTOK_CONTENT_MAX;
    memcpy(slot->content, body, ct);
    slot->content[ct] = '\0';
    c->queue_tail = (c->queue_tail + 1) % TIKTOK_QUEUE_MAX;
    c->queue_count++;
}

hu_error_t hu_tiktok_on_webhook(void *channel_ctx, hu_allocator_t *alloc, const char *body,
                                size_t body_len) {
    hu_tiktok_ctx_t *c = (hu_tiktok_ctx_t *)channel_ctx;
    if (!c || !body || body_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    (void)alloc;
    tiktok_queue_push(c, "test-sender", 11, body, body_len);
    return HU_OK;
#else
    hu_json_value_t *parsed = NULL;
    hu_error_t err = hu_json_parse(alloc, body, body_len, &parsed);
    if (err != HU_OK || !parsed)
        return HU_OK;

    /*
     * TikTok webhook event payload:
     *   { "event": "comment.create", "user": { "open_id": "..." },
     *     "comment": { "text": "...", "video_id": "..." } }
     */
    const char *event = hu_json_get_string(parsed, "event");
    if (!event) {
        hu_json_free(alloc, parsed);
        return HU_OK;
    }

    hu_json_value_t *user_obj = hu_json_object_get(parsed, "user");
    const char *open_id = NULL;
    if (user_obj && user_obj->type == HU_JSON_OBJECT)
        open_id = hu_json_get_string(user_obj, "open_id");
    const char *session = open_id ? open_id : "unknown";

    hu_json_value_t *comment_obj = hu_json_object_get(parsed, "comment");
    if (comment_obj && comment_obj->type == HU_JSON_OBJECT) {
        const char *text = hu_json_get_string(comment_obj, "text");
        if (text && strlen(text) > 0)
            tiktok_queue_push(c, session, strlen(session), text, strlen(text));
    }

    hu_json_free(alloc, parsed);
    return HU_OK;
#endif
}

hu_error_t hu_tiktok_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                          size_t max_msgs, size_t *out_count) {
    (void)alloc;
    hu_tiktok_ctx_t *c = (hu_tiktok_ctx_t *)channel_ctx;
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
        hu_tiktok_queued_msg_t *slot = &c->queue[c->queue_head];
        memcpy(msgs[cnt].session_key, slot->session_key, sizeof(slot->session_key));
        memcpy(msgs[cnt].content, slot->content, sizeof(slot->content));
        c->queue_head = (c->queue_head + 1) % TIKTOK_QUEUE_MAX;
        c->queue_count--;
        cnt++;
    }
    *out_count = cnt;
    return HU_OK;
}

/* ─── Create / Destroy ─────────────────────────────────────────────────── */

hu_error_t hu_tiktok_create(hu_allocator_t *alloc, const char *client_key, size_t client_key_len,
                            const char *client_secret, size_t client_secret_len,
                            const char *access_token, size_t access_token_len, hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_tiktok_ctx_t *c = (hu_tiktok_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;

    if (client_key && client_key_len > 0) {
        c->client_key = (char *)alloc->alloc(alloc->ctx, client_key_len + 1);
        if (!c->client_key) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->client_key, client_key, client_key_len);
        c->client_key[client_key_len] = '\0';
        c->client_key_len = client_key_len;
    }

    if (client_secret && client_secret_len > 0) {
        c->client_secret = (char *)alloc->alloc(alloc->ctx, client_secret_len + 1);
        if (!c->client_secret) {
            if (c->client_key)
                alloc->free(alloc->ctx, c->client_key, c->client_key_len + 1);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->client_secret, client_secret, client_secret_len);
        c->client_secret[client_secret_len] = '\0';
        c->client_secret_len = client_secret_len;
    }

    if (access_token && access_token_len > 0) {
        c->access_token = (char *)alloc->alloc(alloc->ctx, access_token_len + 1);
        if (!c->access_token) {
            if (c->client_key)
                alloc->free(alloc->ctx, c->client_key, c->client_key_len + 1);
            if (c->client_secret)
                alloc->free(alloc->ctx, c->client_secret, c->client_secret_len + 1);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->access_token, access_token, access_token_len);
        c->access_token[access_token_len] = '\0';
        c->access_token_len = access_token_len;
    }

    out->ctx = c;
    out->vtable = &tiktok_vtable;
    return HU_OK;
}

void hu_tiktok_destroy(hu_channel_t *ch) {
    if (ch && ch->ctx) {
        hu_tiktok_ctx_t *c = (hu_tiktok_ctx_t *)ch->ctx;
        hu_allocator_t *a = c->alloc;
        if (a) {
            if (c->client_key)
                a->free(a->ctx, c->client_key, c->client_key_len + 1);
            if (c->client_secret)
                a->free(a->ctx, c->client_secret, c->client_secret_len + 1);
            if (c->access_token)
                a->free(a->ctx, c->access_token, c->access_token_len + 1);
            a->free(a->ctx, c, sizeof(*c));
        }
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

#if HU_IS_TEST
hu_error_t hu_tiktok_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                      size_t session_key_len, const char *content,
                                      size_t content_len) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_tiktok_ctx_t *c = (hu_tiktok_ctx_t *)ch->ctx;
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

const char *hu_tiktok_test_get_last_message(hu_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    hu_tiktok_ctx_t *c = (hu_tiktok_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif
