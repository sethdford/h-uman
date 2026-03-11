#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MM_SESSION_KEY_MAX 127
#define MM_CONTENT_MAX     4095

typedef struct hu_mattermost_ctx {
    hu_allocator_t *alloc;
    char *url;
    size_t url_len;
    char *token;
    size_t token_len;
    bool running;
    char *channel_id;
    char *last_post_id;
#if HU_IS_TEST
    char last_message[4096];
    size_t last_message_len;
    struct {
        char session_key[128];
        char content[4096];
    } mock_msgs[8];
    size_t mock_count;
#endif
} hu_mattermost_ctx_t;

static hu_error_t mattermost_start(void *ctx) {
    hu_mattermost_ctx_t *c = (hu_mattermost_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
    return HU_OK;
}

static void mattermost_stop(void *ctx) {
    hu_mattermost_ctx_t *c = (hu_mattermost_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static hu_error_t mattermost_send(void *ctx, const char *target, size_t target_len,
                                  const char *message, size_t message_len, const char *const *media,
                                  size_t media_count) {
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    (void)media;
    (void)media_count;
    hu_mattermost_ctx_t *c = (hu_mattermost_ctx_t *)ctx;

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
    if (!c->url || c->url_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    if (!c->token || c->token_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    if (!target || target_len == 0 || !message)
        return HU_ERR_INVALID_ARGUMENT;

    char url_buf[1024];
    int n = snprintf(url_buf, sizeof(url_buf), "%.*s/api/v4/posts", (int)c->url_len, c->url);
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return HU_ERR_INTERNAL;

    hu_json_buf_t jbuf;
    hu_error_t err = hu_json_buf_init(&jbuf, c->alloc);
    if (err)
        return err;

    err = hu_json_append_key_value(&jbuf, "channel_id", 10, target, target_len);
    if (err)
        goto jfail;
    err = hu_json_buf_append_raw(&jbuf, ",", 1);
    if (err)
        goto jfail;
    err = hu_json_append_key_value(&jbuf, "message", 7, message, message_len);
    if (err)
        goto jfail;
    err = hu_json_buf_append_raw(&jbuf, "}", 1);
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
    return HU_OK;
jfail:
    hu_json_buf_free(&jbuf);
    return err;
#endif
}

static const char *mattermost_name(void *ctx) {
    (void)ctx;
    return "mattermost";
}
static bool mattermost_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static const hu_channel_vtable_t mattermost_vtable = {
    .start = mattermost_start,
    .stop = mattermost_stop,
    .send = mattermost_send,
    .name = mattermost_name,
    .health_check = mattermost_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

hu_error_t hu_mattermost_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                              size_t max_msgs, size_t *out_count) {
    (void)alloc;
    hu_mattermost_ctx_t *ctx = (hu_mattermost_ctx_t *)channel_ctx;
    if (!ctx || !msgs || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;
#if HU_IS_TEST
    if (ctx->mock_count > 0) {
        size_t n = ctx->mock_count < max_msgs ? ctx->mock_count : max_msgs;
        for (size_t i = 0; i < n; i++) {
            memcpy(msgs[i].session_key, ctx->mock_msgs[i].session_key, 128);
            memcpy(msgs[i].content, ctx->mock_msgs[i].content, 4096);
        }
        *out_count = n;
        ctx->mock_count = 0;
        return HU_OK;
    }
    return HU_OK;
#else
    if (!ctx->url || ctx->url_len == 0 || !ctx->token || ctx->token_len == 0)
        return HU_OK;
    if (!ctx->channel_id || !ctx->running)
        return HU_OK;
    char url_buf[1024];
    int nu;
    if (ctx->last_post_id)
        nu =
            snprintf(url_buf, sizeof(url_buf), "%.*s/api/v4/channels/%s/posts?after=%s&per_page=10",
                     (int)ctx->url_len, ctx->url, ctx->channel_id, ctx->last_post_id);
    else
        nu = snprintf(url_buf, sizeof(url_buf), "%.*s/api/v4/channels/%s/posts?per_page=10",
                      (int)ctx->url_len, ctx->url, ctx->channel_id);
    if (nu < 0 || (size_t)nu >= sizeof(url_buf))
        return HU_ERR_INTERNAL;
    char auth_buf[512];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)ctx->token_len, ctx->token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf))
        return HU_ERR_INTERNAL;
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(alloc, url_buf, auth_buf, &resp);
    if (err != HU_OK) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return HU_OK;
    }
    if (resp.status_code != 200 || !resp.body) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return HU_OK;
    }
    hu_json_value_t *parsed = NULL;
    err = hu_json_parse(alloc, resp.body, resp.body_len, &parsed);
    if (resp.owned && resp.body)
        hu_http_response_free(alloc, &resp);
    if (err != HU_OK || !parsed)
        return HU_OK;
    hu_json_value_t *order = hu_json_object_get(parsed, "order");
    hu_json_value_t *posts = hu_json_object_get(parsed, "posts");
    size_t cnt = 0;
    if (order && order->type == HU_JSON_ARRAY && posts && posts->type == HU_JSON_OBJECT) {
        for (size_t i = 0; i < order->data.array.len && cnt < max_msgs; i++) {
            hu_json_value_t *pid_val = order->data.array.items[i];
            if (!pid_val || pid_val->type != HU_JSON_STRING)
                continue;
            const char *pid = pid_val->data.string.ptr;
            hu_json_value_t *post = hu_json_object_get(posts, pid);
            if (!post || post->type != HU_JSON_OBJECT)
                continue;
            const char *msg = hu_json_get_string(post, "message");
            const char *user_id = hu_json_get_string(post, "user_id");
            if (!msg || !user_id || strlen(msg) == 0)
                continue;
            size_t sk = strlen(user_id);
            if (sk > MM_SESSION_KEY_MAX)
                sk = MM_SESSION_KEY_MAX;
            memcpy(msgs[cnt].session_key, user_id, sk);
            msgs[cnt].session_key[sk] = '\0';
            size_t ct = strlen(msg);
            if (ct > MM_CONTENT_MAX)
                ct = MM_CONTENT_MAX;
            memcpy(msgs[cnt].content, msg, ct);
            msgs[cnt].content[ct] = '\0';
            cnt++;
            if (i == 0 && ctx->last_post_id) {
                ctx->alloc->free(ctx->alloc->ctx, ctx->last_post_id, strlen(ctx->last_post_id) + 1);
                ctx->last_post_id = NULL;
            }
            if (i == 0) {
                size_t plen = strlen(pid);
                ctx->last_post_id = (char *)ctx->alloc->alloc(ctx->alloc->ctx, plen + 1);
                if (ctx->last_post_id) {
                    memcpy(ctx->last_post_id, pid, plen);
                    ctx->last_post_id[plen] = '\0';
                }
            }
        }
    }
    hu_json_free(alloc, parsed);
    *out_count = cnt;
    return HU_OK;
#endif
}

hu_error_t hu_mattermost_create(hu_allocator_t *alloc, const char *url, size_t url_len,
                                const char *token, size_t token_len, hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_mattermost_ctx_t *c = (hu_mattermost_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    if (url && url_len > 0) {
        c->url = (char *)alloc->alloc(alloc->ctx, url_len + 1);
        if (!c->url) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->url, url, url_len);
        c->url[url_len] = '\0';
        c->url_len = url_len;
    }
    if (token && token_len > 0) {
        c->token = (char *)alloc->alloc(alloc->ctx, token_len + 1);
        if (!c->token) {
            if (c->url)
                alloc->free(alloc->ctx, c->url, c->url_len + 1);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->token, token, token_len);
        c->token[token_len] = '\0';
        c->token_len = token_len;
    }
    out->ctx = c;
    out->vtable = &mattermost_vtable;
    return HU_OK;
}

void hu_mattermost_destroy(hu_channel_t *ch) {
    if (ch && ch->ctx) {
        hu_mattermost_ctx_t *c = (hu_mattermost_ctx_t *)ch->ctx;
        hu_allocator_t *a = c->alloc;
        if (a) {
            if (c->url)
                a->free(a->ctx, c->url, c->url_len + 1);
            if (c->token)
                a->free(a->ctx, c->token, c->token_len + 1);
            if (c->channel_id)
                a->free(a->ctx, c->channel_id, strlen(c->channel_id) + 1);
            if (c->last_post_id)
                a->free(a->ctx, c->last_post_id, strlen(c->last_post_id) + 1);
            a->free(a->ctx, c, sizeof(*c));
        }
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

#if HU_IS_TEST
hu_error_t hu_mattermost_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                          size_t session_key_len, const char *content,
                                          size_t content_len) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_mattermost_ctx_t *c = (hu_mattermost_ctx_t *)ch->ctx;
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
const char *hu_mattermost_test_get_last_message(hu_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    hu_mattermost_ctx_t *c = (hu_mattermost_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif
