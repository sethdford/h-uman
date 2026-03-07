#include "seaclaw/channels/teams.h"
#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEAMS_QUEUE_MAX       32
#define TEAMS_SESSION_KEY_MAX 127
#define TEAMS_CONTENT_MAX     4095

typedef struct sc_teams_queued_msg {
    char session_key[128];
    char content[4096];
} sc_teams_queued_msg_t;

typedef struct sc_teams_ctx {
    sc_allocator_t *alloc;
    char *webhook_url;
    size_t webhook_url_len;
    bool running;
    sc_teams_queued_msg_t queue[TEAMS_QUEUE_MAX];
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
} sc_teams_ctx_t;

static sc_error_t teams_start(void *ctx) {
    sc_teams_ctx_t *c = (sc_teams_ctx_t *)ctx;
    if (!c)
        return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void teams_stop(void *ctx) {
    sc_teams_ctx_t *c = (sc_teams_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static void teams_queue_push(sc_teams_ctx_t *c, const char *from, size_t from_len, const char *body,
                             size_t body_len) {
    if (c->queue_count >= TEAMS_QUEUE_MAX)
        return;
    sc_teams_queued_msg_t *slot = &c->queue[c->queue_tail];
    size_t sk = from_len < TEAMS_SESSION_KEY_MAX ? from_len : TEAMS_SESSION_KEY_MAX;
    memcpy(slot->session_key, from, sk);
    slot->session_key[sk] = '\0';
    size_t ct = body_len < TEAMS_CONTENT_MAX ? body_len : TEAMS_CONTENT_MAX;
    memcpy(slot->content, body, ct);
    slot->content[ct] = '\0';
    c->queue_tail = (c->queue_tail + 1) % TEAMS_QUEUE_MAX;
    c->queue_count++;
}

static sc_error_t teams_send(void *ctx, const char *target, size_t target_len, const char *message,
                             size_t message_len, const char *const *media, size_t media_count) {
    (void)media;
    (void)media_count;
    (void)target;
    (void)target_len;
    sc_teams_ctx_t *c = (sc_teams_ctx_t *)ctx;

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
    if (!c || !c->alloc)
        return SC_ERR_INVALID_ARGUMENT;
    if (!c->webhook_url || c->webhook_url_len == 0)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;
    if (!message)
        return SC_ERR_INVALID_ARGUMENT;
    if (c->webhook_url_len < 9 || strncmp(c->webhook_url, "https://", 8) != 0)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;

    sc_json_buf_t jbuf;
    sc_error_t err = sc_json_buf_init(&jbuf, c->alloc);
    if (err)
        return err;
    err = sc_json_buf_append_raw(&jbuf, "{\"text\":", 8);
    if (err)
        goto jfail;
    err = sc_json_append_string(&jbuf, message, message_len);
    if (err)
        goto jfail;
    err = sc_json_buf_append_raw(&jbuf, "}", 1);
    if (err)
        goto jfail;

    sc_http_response_t resp = {0};
    err = sc_http_post_json(c->alloc, c->webhook_url, NULL, jbuf.ptr, jbuf.len, &resp);
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

static const char *teams_name(void *ctx) {
    (void)ctx;
    return "teams";
}

static bool teams_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static const sc_channel_vtable_t teams_vtable = {
    .start = teams_start,
    .stop = teams_stop,
    .send = teams_send,
    .name = teams_name,
    .health_check = teams_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

sc_error_t sc_teams_on_webhook(void *channel_ctx, sc_allocator_t *alloc, const char *body,
                               size_t body_len) {
    sc_teams_ctx_t *c = (sc_teams_ctx_t *)channel_ctx;
    if (!c || !body || body_len == 0)
        return SC_ERR_INVALID_ARGUMENT;
#if SC_IS_TEST
    (void)alloc;
    teams_queue_push(c, "test-sender", 11, body, body_len);
    return SC_OK;
#else
    sc_json_value_t *parsed = NULL;
    sc_error_t err = sc_json_parse(alloc, body, body_len, &parsed);
    if (err != SC_OK || !parsed)
        return SC_OK;
    const char *activity_type = sc_json_get_string(parsed, "type");
    if (activity_type && strcmp(activity_type, "message") == 0) {
        const char *text = sc_json_get_string(parsed, "text");
        sc_json_value_t *from = sc_json_object_get(parsed, "from");
        const char *from_id = from ? sc_json_get_string(from, "id") : NULL;
        if (text && from_id && strlen(text) > 0)
            teams_queue_push(c, from_id, strlen(from_id), text, strlen(text));
    }
    sc_json_free(alloc, parsed);
    return SC_OK;
#endif
}

sc_error_t sc_teams_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                         size_t max_msgs, size_t *out_count) {
    (void)alloc;
    sc_teams_ctx_t *c = (sc_teams_ctx_t *)channel_ctx;
    if (!c || !msgs || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out_count = 0;
#if SC_IS_TEST
    {
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
        sc_teams_queued_msg_t *slot = &c->queue[c->queue_head];
        memcpy(msgs[cnt].session_key, slot->session_key, sizeof(slot->session_key));
        memcpy(msgs[cnt].content, slot->content, sizeof(slot->content));
        c->queue_head = (c->queue_head + 1) % TEAMS_QUEUE_MAX;
        c->queue_count--;
        cnt++;
    }
    *out_count = cnt;
    return SC_OK;
}

bool sc_teams_is_configured(sc_channel_t *ch) {
    if (!ch || !ch->ctx)
        return false;
    sc_teams_ctx_t *c = (sc_teams_ctx_t *)ch->ctx;
    return c->webhook_url != NULL && c->webhook_url_len > 0;
}

sc_error_t sc_teams_create(sc_allocator_t *alloc, const char *webhook_url, size_t webhook_url_len,
                           sc_channel_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_teams_ctx_t *c = (sc_teams_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    if (webhook_url && webhook_url_len > 0) {
        c->webhook_url = (char *)malloc(webhook_url_len + 1);
        if (!c->webhook_url) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->webhook_url, webhook_url, webhook_url_len);
        c->webhook_url[webhook_url_len] = '\0';
        c->webhook_url_len = webhook_url_len;
    }
    out->ctx = c;
    out->vtable = &teams_vtable;
    return SC_OK;
}

void sc_teams_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_teams_ctx_t *c = (sc_teams_ctx_t *)ch->ctx;
        sc_allocator_t *a = c->alloc;
        if (c->webhook_url)
            free(c->webhook_url);
        a->free(a->ctx, c, sizeof(*c));
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

#if SC_IS_TEST
sc_error_t sc_teams_test_inject_mock(sc_channel_t *ch, const char *session_key,
                                     size_t session_key_len, const char *content,
                                     size_t content_len) {
    if (!ch || !ch->ctx)
        return SC_ERR_INVALID_ARGUMENT;
    sc_teams_ctx_t *c = (sc_teams_ctx_t *)ch->ctx;
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
const char *sc_teams_test_get_last_message(sc_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    sc_teams_ctx_t *c = (sc_teams_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif
