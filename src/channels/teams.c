#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
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
    char *app_id;
    size_t app_id_len;
    char *app_secret;
    size_t app_secret_len;
    char *service_url;
    size_t service_url_len;
    bool running;
    sc_teams_queued_msg_t queue[TEAMS_QUEUE_MAX];
    size_t queue_head;
    size_t queue_tail;
    size_t queue_count;
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

static sc_error_t teams_send(void *ctx, const char *target, size_t target_len, const char *message,
                             size_t message_len, const char *const *media, size_t media_count) {
    (void)media;
    (void)media_count;
    sc_teams_ctx_t *c = (sc_teams_ctx_t *)ctx;

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
    if (!c->service_url || c->service_url_len == 0)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;
    if (!target || target_len == 0 || !message)
        return SC_ERR_INVALID_ARGUMENT;

    char url_buf[1024];
    int n = snprintf(url_buf, sizeof(url_buf), "%.*s/v3/conversations/%.*s/activities",
                     (int)c->service_url_len, c->service_url, (int)target_len, target);
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return SC_ERR_INTERNAL;

    sc_json_buf_t jbuf;
    sc_error_t err = sc_json_buf_init(&jbuf, c->alloc);
    if (err)
        return err;
    err = sc_json_buf_append_raw(&jbuf, "{\"type\":\"message\",\"text\":", 24);
    if (err)
        goto jfail;
    err = sc_json_append_string(&jbuf, message, message_len);
    if (err)
        goto jfail;
    err = sc_json_buf_append_raw(&jbuf, "}", 1);
    if (err)
        goto jfail;

    char auth_buf[512];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)c->app_secret_len, c->app_secret);
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

static void teams_queue_push(sc_teams_ctx_t *c, const char *from, size_t from_len,
                             const char *body, size_t body_len) {
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

sc_error_t sc_teams_create(sc_allocator_t *alloc, const char *app_id, size_t app_id_len,
                           const char *app_secret, size_t app_secret_len, const char *service_url,
                           size_t service_url_len, sc_channel_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_teams_ctx_t *c = (sc_teams_ctx_t *)calloc(1, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    if (app_id && app_id_len > 0) {
        c->app_id = (char *)malloc(app_id_len + 1);
        if (!c->app_id) {
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->app_id, app_id, app_id_len);
        c->app_id[app_id_len] = '\0';
        c->app_id_len = app_id_len;
    }
    if (app_secret && app_secret_len > 0) {
        c->app_secret = (char *)malloc(app_secret_len + 1);
        if (!c->app_secret) {
            if (c->app_id)
                free(c->app_id);
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->app_secret, app_secret, app_secret_len);
        c->app_secret[app_secret_len] = '\0';
        c->app_secret_len = app_secret_len;
    }
    if (service_url && service_url_len > 0) {
        c->service_url = (char *)malloc(service_url_len + 1);
        if (!c->service_url) {
            if (c->app_secret)
                free(c->app_secret);
            if (c->app_id)
                free(c->app_id);
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->service_url, service_url, service_url_len);
        c->service_url[service_url_len] = '\0';
        c->service_url_len = service_url_len;
    }
    out->ctx = c;
    out->vtable = &teams_vtable;
    return SC_OK;
}

void sc_teams_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_teams_ctx_t *c = (sc_teams_ctx_t *)ch->ctx;
        if (c->app_id)
            free(c->app_id);
        if (c->app_secret)
            free(c->app_secret);
        if (c->service_url)
            free(c->service_url);
        free(c);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
