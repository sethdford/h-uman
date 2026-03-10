#include "human/channels/qq.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QQ_API_BASE     "https://api.sgroup.qq.com"
#define QQ_SANDBOX_BASE "https://sandbox.api.sgroup.qq.com"
#define QQ_MAX_MSG      4096

#define QQ_QUEUE_MAX       32
#define QQ_SESSION_KEY_MAX 127
#define QQ_CONTENT_MAX     4095

typedef struct hu_qq_queued_msg {
    char session_key[128];
    char content[4096];
} hu_qq_queued_msg_t;

typedef struct hu_qq_ctx {
    hu_allocator_t *alloc;
    char *app_id;
    char *bot_token;
    char *channel_id;
    size_t channel_id_len;
    bool sandbox;
    bool running;
    hu_qq_queued_msg_t queue[QQ_QUEUE_MAX];
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
} hu_qq_ctx_t;

static hu_error_t qq_start(void *ctx) {
    hu_qq_ctx_t *c = (hu_qq_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
    return HU_OK;
}

static void qq_stop(void *ctx) {
    hu_qq_ctx_t *c = (hu_qq_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static void qq_queue_push(hu_qq_ctx_t *c, const char *from, size_t from_len, const char *body,
                          size_t body_len) {
    if (c->queue_count >= QQ_QUEUE_MAX)
        return;
    hu_qq_queued_msg_t *slot = &c->queue[c->queue_tail];
    size_t sk = from_len < QQ_SESSION_KEY_MAX ? from_len : QQ_SESSION_KEY_MAX;
    memcpy(slot->session_key, from, sk);
    slot->session_key[sk] = '\0';
    size_t ct = body_len < QQ_CONTENT_MAX ? body_len : QQ_CONTENT_MAX;
    memcpy(slot->content, body, ct);
    slot->content[ct] = '\0';
    c->queue_tail = (c->queue_tail + 1) % QQ_QUEUE_MAX;
    c->queue_count++;
}

static hu_error_t qq_send(void *ctx, const char *target, size_t target_len, const char *message,
                          size_t message_len, const char *const *media, size_t media_count) {
    (void)media;
    (void)media_count;
    hu_qq_ctx_t *c = (hu_qq_ctx_t *)ctx;

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
    const char *ch_id = (target && target_len > 0) ? target : c->channel_id;
    size_t ch_id_len = (target && target_len > 0) ? target_len : c->channel_id_len;

    if (!c || !c->alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (!c->bot_token || !ch_id || ch_id_len == 0 || !message)
        return HU_ERR_INVALID_ARGUMENT;

    const char *base = c->sandbox ? QQ_SANDBOX_BASE : QQ_API_BASE;
    char url_buf[512];
    int n = snprintf(url_buf, sizeof(url_buf), "%s/channels/%.*s/messages", base, (int)ch_id_len,
                     ch_id);
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return HU_ERR_INTERNAL;

    hu_json_buf_t jbuf;
    hu_error_t err = hu_json_buf_init(&jbuf, c->alloc);
    if (err)
        return err;
    err = hu_json_buf_append_raw(&jbuf, "{\"content\":", 11);
    if (err)
        goto jfail;
    err = hu_json_append_string(&jbuf, message, message_len);
    if (err)
        goto jfail;
    err = hu_json_buf_append_raw(&jbuf, "}", 1);
    if (err)
        goto jfail;

    char *body = (char *)c->alloc->alloc(c->alloc->ctx, jbuf.len + 1);
    if (!body) {
        err = HU_ERR_OUT_OF_MEMORY;
        goto jfail;
    }
    memcpy(body, jbuf.ptr, jbuf.len + 1);
    size_t body_len = jbuf.len;
    hu_json_buf_free(&jbuf);

    char auth_buf[256];
    int ab = snprintf(auth_buf, sizeof(auth_buf), "Bot %s.%s", c->app_id ? c->app_id : "",
                      c->bot_token ? c->bot_token : "");
    if (ab <= 0 || (size_t)ab >= sizeof(auth_buf)) {
        c->alloc->free(c->alloc->ctx, body, body_len + 1);
        return HU_ERR_INTERNAL;
    }

    hu_http_response_t resp = {0};
    err = hu_http_post_json(c->alloc, url_buf, auth_buf, body, body_len, &resp);
    c->alloc->free(c->alloc->ctx, body, body_len + 1);
    if (err) {
        if (resp.owned && resp.body)
            hu_http_response_free(c->alloc, &resp);
        return HU_ERR_CHANNEL_SEND;
    }
    if (resp.owned && resp.body)
        hu_http_response_free(c->alloc, &resp);
    return HU_OK;
jfail:
    hu_json_buf_free(&jbuf);
    return err;
#endif
}

static const char *qq_name(void *ctx) {
    (void)ctx;
    return "qq";
}
static bool qq_health_check(void *ctx) {
    hu_qq_ctx_t *c = (hu_qq_ctx_t *)ctx;
    return c && c->running;
}

static const hu_channel_vtable_t qq_vtable = {
    .start = qq_start,
    .stop = qq_stop,
    .send = qq_send,
    .name = qq_name,
    .health_check = qq_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

hu_error_t hu_qq_on_webhook(void *channel_ctx, hu_allocator_t *alloc, const char *body,
                            size_t body_len) {
    hu_qq_ctx_t *c = (hu_qq_ctx_t *)channel_ctx;
    if (!c || !body || body_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    (void)alloc;
    qq_queue_push(c, "test-sender", 11, body, body_len);
    return HU_OK;
#else
    hu_json_value_t *parsed = NULL;
    hu_error_t err = hu_json_parse(alloc, body, body_len, &parsed);
    if (err != HU_OK || !parsed)
        return HU_OK;
    hu_json_value_t *d = hu_json_object_get(parsed, "d");
    if (d && d->type == HU_JSON_OBJECT) {
        const char *content = hu_json_get_string(d, "content");
        hu_json_value_t *author = hu_json_object_get(d, "author");
        const char *author_id = author ? hu_json_get_string(author, "id") : NULL;
        if (content && author_id && strlen(content) > 0)
            qq_queue_push(c, author_id, strlen(author_id), content, strlen(content));
    }
    hu_json_free(alloc, parsed);
    return HU_OK;
#endif
}

hu_error_t hu_qq_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                      size_t max_msgs, size_t *out_count) {
    (void)alloc;
    hu_qq_ctx_t *c = (hu_qq_ctx_t *)channel_ctx;
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
        hu_qq_queued_msg_t *slot = &c->queue[c->queue_head];
        memcpy(msgs[cnt].session_key, slot->session_key, sizeof(slot->session_key));
        memcpy(msgs[cnt].content, slot->content, sizeof(slot->content));
        c->queue_head = (c->queue_head + 1) % QQ_QUEUE_MAX;
        c->queue_count--;
        cnt++;
    }
    *out_count = cnt;
    return HU_OK;
}

hu_error_t hu_qq_create(hu_allocator_t *alloc, const char *app_id, size_t app_id_len,
                        const char *bot_token, size_t bot_token_len, bool sandbox,
                        hu_channel_t *out) {
    return hu_qq_create_ex(alloc, app_id, app_id_len, bot_token, bot_token_len, NULL, 0, sandbox,
                           out);
}

hu_error_t hu_qq_create_ex(hu_allocator_t *alloc, const char *app_id, size_t app_id_len,
                           const char *bot_token, size_t bot_token_len, const char *channel_id,
                           size_t channel_id_len, bool sandbox, hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_qq_ctx_t *c = (hu_qq_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    c->sandbox = sandbox;
    if (app_id && app_id_len > 0) {
        c->app_id = (char *)malloc(app_id_len + 1);
        if (!c->app_id) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->app_id, app_id, app_id_len);
        c->app_id[app_id_len] = '\0';
    }
    if (bot_token && bot_token_len > 0) {
        c->bot_token = (char *)malloc(bot_token_len + 1);
        if (!c->bot_token) {
            if (c->app_id)
                free(c->app_id);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->bot_token, bot_token, bot_token_len);
        c->bot_token[bot_token_len] = '\0';
    }
    if (channel_id && channel_id_len > 0) {
        c->channel_id = (char *)malloc(channel_id_len + 1);
        if (!c->channel_id) {
            if (c->bot_token)
                free(c->bot_token);
            if (c->app_id)
                free(c->app_id);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->channel_id, channel_id, channel_id_len);
        c->channel_id[channel_id_len] = '\0';
        c->channel_id_len = channel_id_len;
    }
    out->ctx = c;
    out->vtable = &qq_vtable;
    return HU_OK;
}

bool hu_qq_is_configured(hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return false;
    hu_qq_ctx_t *c = (hu_qq_ctx_t *)ch->ctx;
    return c->bot_token != NULL && (c->channel_id != NULL && c->channel_id_len > 0);
}

void hu_qq_destroy(hu_channel_t *ch) {
    if (ch && ch->ctx) {
        hu_qq_ctx_t *c = (hu_qq_ctx_t *)ch->ctx;
        hu_allocator_t *a = c->alloc;
        if (c->app_id)
            free(c->app_id);
        if (c->bot_token)
            free(c->bot_token);
        if (c->channel_id)
            free(c->channel_id);
        if (a)
            a->free(a->ctx, c, sizeof(*c));
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

#if HU_IS_TEST
hu_error_t hu_qq_test_inject_mock(hu_channel_t *ch, const char *session_key, size_t session_key_len,
                                  const char *content, size_t content_len) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_qq_ctx_t *c = (hu_qq_ctx_t *)ch->ctx;
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
const char *hu_qq_test_get_last_message(hu_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    hu_qq_ctx_t *c = (hu_qq_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif
