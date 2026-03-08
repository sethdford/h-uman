#include "seaclaw/channels/onebot.h"
#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ONEBOT_QUEUE_MAX       32
#define ONEBOT_SESSION_KEY_MAX 127
#define ONEBOT_CONTENT_MAX     4095

typedef struct sc_onebot_queued_msg {
    char session_key[128];
    char content[4096];
} sc_onebot_queued_msg_t;

typedef struct sc_onebot_ctx {
    sc_allocator_t *alloc;
    char *api_base;
    size_t api_base_len;
    char *access_token;
    size_t access_token_len;
    char *user_id;
    size_t user_id_len;
    bool running;
    sc_onebot_queued_msg_t queue[ONEBOT_QUEUE_MAX];
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
} sc_onebot_ctx_t;

static sc_error_t onebot_start(void *ctx) {
    sc_onebot_ctx_t *c = (sc_onebot_ctx_t *)ctx;
    if (!c)
        return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void onebot_stop(void *ctx) {
    sc_onebot_ctx_t *c = (sc_onebot_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static void onebot_queue_push(sc_onebot_ctx_t *c, const char *from, size_t from_len,
                              const char *body, size_t body_len) {
    if (c->queue_count >= ONEBOT_QUEUE_MAX)
        return;
    sc_onebot_queued_msg_t *slot = &c->queue[c->queue_tail];
    size_t sk = from_len < ONEBOT_SESSION_KEY_MAX ? from_len : ONEBOT_SESSION_KEY_MAX;
    memcpy(slot->session_key, from, sk);
    slot->session_key[sk] = '\0';
    size_t ct = body_len < ONEBOT_CONTENT_MAX ? body_len : ONEBOT_CONTENT_MAX;
    memcpy(slot->content, body, ct);
    slot->content[ct] = '\0';
    c->queue_tail = (c->queue_tail + 1) % ONEBOT_QUEUE_MAX;
    c->queue_count++;
}

static sc_error_t onebot_send(void *ctx, const char *target, size_t target_len, const char *message,
                              size_t message_len, const char *const *media, size_t media_count) {
    (void)media;
    (void)media_count;
    sc_onebot_ctx_t *c = (sc_onebot_ctx_t *)ctx;

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
    const char *uid = (target && target_len > 0) ? target : c->user_id;
    size_t uid_len = (target && target_len > 0) ? target_len : c->user_id_len;

    if (!c || !c->alloc || !c->api_base || c->api_base_len == 0)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;
    if (!uid || uid_len == 0 || !message)
        return SC_ERR_INVALID_ARGUMENT;

    sc_json_buf_t jbuf;
    sc_error_t err = sc_json_buf_init(&jbuf, c->alloc);
    if (err)
        return err;

    /* OneBot 11: private message */
    err = sc_json_buf_append_raw(&jbuf, "{\"message_type\":\"private\",", 26);
    if (err)
        goto fail;
    err = sc_json_append_key_value(&jbuf, "user_id", 7, uid, uid_len);
    if (err)
        goto fail;
    err = sc_json_buf_append_raw(&jbuf, ",\"message\":", 11);
    if (err)
        goto fail;
    err = sc_json_append_string(&jbuf, message, message_len);
    if (err)
        goto fail;
    err = sc_json_buf_append_raw(&jbuf, "}", 1);
    if (err)
        goto fail;

    char url_buf[512];
    size_t base_len = c->api_base_len;
    int n = snprintf(url_buf, sizeof(url_buf), "%.*s%s/send_msg", (int)base_len, c->api_base,
                     (base_len > 0 && c->api_base[base_len - 1] == '/') ? "" : "/");
    if (n < 0 || (size_t)n >= sizeof(url_buf)) {
        err = SC_ERR_INTERNAL;
        goto fail;
    }

    char *auth = NULL;
    if (c->access_token && c->access_token_len > 0) {
        auth = (char *)c->alloc->alloc(c->alloc->ctx, 8 + c->access_token_len + 1);
        if (!auth) {
            err = SC_ERR_OUT_OF_MEMORY;
            goto fail;
        }
        n = snprintf(auth, 8 + c->access_token_len + 1, "Bearer %.*s", (int)c->access_token_len,
                     c->access_token);
        (void)n;
    }

    sc_http_response_t resp = {0};
    err = sc_http_post_json(c->alloc, url_buf, auth, jbuf.ptr, jbuf.len, &resp);
    if (auth)
        c->alloc->free(c->alloc->ctx, auth, 8 + c->access_token_len + 1);
    sc_json_buf_free(&jbuf);
    if (err) {
        if (resp.owned && resp.body)
            sc_http_response_free(c->alloc, &resp);
        return SC_ERR_CHANNEL_SEND;
    }
    if (resp.owned && resp.body)
        sc_http_response_free(c->alloc, &resp);
    if (resp.status_code < 200 || resp.status_code >= 300)
        return SC_ERR_CHANNEL_SEND;
    return SC_OK;
fail:
    sc_json_buf_free(&jbuf);
    return err;
#endif
}

static const char *onebot_name(void *ctx) {
    (void)ctx;
    return "onebot";
}
static bool onebot_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static const sc_channel_vtable_t onebot_vtable = {
    .start = onebot_start,
    .stop = onebot_stop,
    .send = onebot_send,
    .name = onebot_name,
    .health_check = onebot_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

sc_error_t sc_onebot_on_webhook(void *channel_ctx, sc_allocator_t *alloc, const char *body,
                                size_t body_len) {
    sc_onebot_ctx_t *c = (sc_onebot_ctx_t *)channel_ctx;
    if (!c || !body || body_len == 0)
        return SC_ERR_INVALID_ARGUMENT;
#if SC_IS_TEST
    (void)alloc;
    onebot_queue_push(c, "test-sender", 11, body, body_len);
    return SC_OK;
#else
    sc_json_value_t *parsed = NULL;
    sc_error_t err = sc_json_parse(alloc, body, body_len, &parsed);
    if (err != SC_OK || !parsed)
        return SC_OK;
    const char *msg_type = sc_json_get_string(parsed, "post_type");
    if (msg_type && strcmp(msg_type, "message") == 0) {
        const char *message = sc_json_get_string(parsed, "raw_message");
        if (!message)
            message = sc_json_get_string(parsed, "message");
        const char *user_id = sc_json_get_string(parsed, "user_id");
        if (!user_id) {
            sc_json_value_t *sender = sc_json_object_get(parsed, "sender");
            user_id = sender ? sc_json_get_string(sender, "user_id") : NULL;
        }
        if (message && user_id && strlen(message) > 0)
            onebot_queue_push(c, user_id, strlen(user_id), message, strlen(message));
    }
    sc_json_free(alloc, parsed);
    return SC_OK;
#endif
}

sc_error_t sc_onebot_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                          size_t max_msgs, size_t *out_count) {
    (void)alloc;
    sc_onebot_ctx_t *c = (sc_onebot_ctx_t *)channel_ctx;
    if (!c || !msgs || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out_count = 0;
#if SC_IS_TEST
    if (c->mock_count > 0) {
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
        sc_onebot_queued_msg_t *slot = &c->queue[c->queue_head];
        memcpy(msgs[cnt].session_key, slot->session_key, sizeof(slot->session_key));
        memcpy(msgs[cnt].content, slot->content, sizeof(slot->content));
        c->queue_head = (c->queue_head + 1) % ONEBOT_QUEUE_MAX;
        c->queue_count--;
        cnt++;
    }
    *out_count = cnt;
    return SC_OK;
}

sc_error_t sc_onebot_create(sc_allocator_t *alloc, const char *api_base, size_t api_base_len,
                            const char *access_token, size_t access_token_len, sc_channel_t *out) {
    return sc_onebot_create_ex(alloc, api_base, api_base_len, access_token, access_token_len, NULL,
                               0, out);
}

sc_error_t sc_onebot_create_ex(sc_allocator_t *alloc, const char *api_base, size_t api_base_len,
                               const char *access_token, size_t access_token_len,
                               const char *user_id, size_t user_id_len, sc_channel_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_onebot_ctx_t *c = (sc_onebot_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    if (api_base && api_base_len > 0) {
        c->api_base = (char *)malloc(api_base_len + 1);
        if (!c->api_base) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->api_base, api_base, api_base_len);
        c->api_base[api_base_len] = '\0';
        c->api_base_len = api_base_len;
    }
    if (access_token && access_token_len > 0) {
        c->access_token = (char *)malloc(access_token_len + 1);
        if (!c->access_token) {
            if (c->api_base)
                free(c->api_base);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->access_token, access_token, access_token_len);
        c->access_token[access_token_len] = '\0';
        c->access_token_len = access_token_len;
    }
    if (user_id && user_id_len > 0) {
        c->user_id = (char *)malloc(user_id_len + 1);
        if (!c->user_id) {
            if (c->access_token)
                free(c->access_token);
            if (c->api_base)
                free(c->api_base);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->user_id, user_id, user_id_len);
        c->user_id[user_id_len] = '\0';
        c->user_id_len = user_id_len;
    }
    out->ctx = c;
    out->vtable = &onebot_vtable;
    return SC_OK;
}

bool sc_onebot_is_configured(sc_channel_t *ch) {
    if (!ch || !ch->ctx)
        return false;
    sc_onebot_ctx_t *c = (sc_onebot_ctx_t *)ch->ctx;
    return c->api_base != NULL && c->api_base_len > 0 && (c->user_id != NULL && c->user_id_len > 0);
}

void sc_onebot_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_onebot_ctx_t *c = (sc_onebot_ctx_t *)ch->ctx;
        sc_allocator_t *a = c->alloc;
        if (c->api_base)
            free(c->api_base);
        if (c->access_token)
            free(c->access_token);
        if (c->user_id)
            free(c->user_id);
        a->free(a->ctx, c, sizeof(*c));
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

#if SC_IS_TEST
sc_error_t sc_onebot_test_inject_mock(sc_channel_t *ch, const char *session_key,
                                      size_t session_key_len, const char *content,
                                      size_t content_len) {
    if (!ch || !ch->ctx)
        return SC_ERR_INVALID_ARGUMENT;
    sc_onebot_ctx_t *c = (sc_onebot_ctx_t *)ch->ctx;
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
const char *sc_onebot_test_get_last_message(sc_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    sc_onebot_ctx_t *c = (sc_onebot_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif
