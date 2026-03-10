#include "human/channels/discord.h"
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

#define DISCORD_API_BASE        "https://discord.com/api/v10/channels"
#define DISCORD_MAX_CHANNELS    16
#define DISCORD_SESSION_KEY_MAX 127
#define DISCORD_CONTENT_MAX     4095
#define DISCORD_QUEUE_MAX       32

typedef struct hu_discord_queued_msg {
    char session_key[128];
    char content[4096];
} hu_discord_queued_msg_t;

typedef struct hu_discord_ctx {
    hu_allocator_t *alloc;
    char *token;
    size_t token_len;
    bool running;
    /* Polling: channel IDs to monitor */
    char **channel_ids;
    size_t channel_ids_count;
    char *bot_id;
    size_t bot_id_len;
    /* last_message_id per channel (parallel to channel_ids) */
    char **last_message_ids;
    /* Streaming: message under edit */
    char *stream_message_id;
    char *stream_text;
    size_t stream_text_len;
    size_t stream_text_cap;
    /* Webhook inbound queue */
    hu_discord_queued_msg_t queue[DISCORD_QUEUE_MAX];
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
} hu_discord_ctx_t;

#if !HU_IS_TEST
static hu_error_t build_discord_body(hu_allocator_t *alloc, const char *content, size_t content_len,
                                     char **out, size_t *out_len) {
    hu_json_buf_t jbuf;
    hu_error_t err = hu_json_buf_init(&jbuf, alloc);
    if (err)
        return err;
    err = hu_json_buf_append_raw(&jbuf, "{\"content\":", 11);
    if (err)
        goto fail;
    err = hu_json_append_string(&jbuf, content, content_len);
    if (err)
        goto fail;
    err = hu_json_buf_append_raw(&jbuf, "}", 1);
    if (err)
        goto fail;
    *out_len = jbuf.len;
    *out = (char *)alloc->alloc(alloc->ctx, jbuf.len + 1);
    if (!*out) {
        err = HU_ERR_OUT_OF_MEMORY;
        goto fail;
    }
    memcpy(*out, jbuf.ptr, jbuf.len + 1);
    hu_json_buf_free(&jbuf);
    return HU_OK;
fail:
    hu_json_buf_free(&jbuf);
    return err;
}
#endif

static hu_error_t discord_start(void *ctx) {
    hu_discord_ctx_t *c = (hu_discord_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
    return HU_OK;
}

static void discord_stop(void *ctx) {
    hu_discord_ctx_t *c = (hu_discord_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static hu_error_t discord_send(void *ctx, const char *target, size_t target_len,
                               const char *message, size_t message_len, const char *const *media,
                               size_t media_count) {
    (void)media;
    (void)media_count;
    hu_discord_ctx_t *c = (hu_discord_ctx_t *)ctx;
    if (!c || !c->alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (!c->token || c->token_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    if (!target || target_len == 0 || !message)
        return HU_ERR_INVALID_ARGUMENT;

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
    char url_buf[512];
    int n = snprintf(url_buf, sizeof(url_buf), "%s/%.*s/messages", DISCORD_API_BASE,
                     (int)target_len, target);
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return HU_ERR_INTERNAL;

    char *body = NULL;
    size_t body_len = 0;
    hu_error_t err = build_discord_body(c->alloc, message, message_len, &body, &body_len);
    if (err)
        return err;

    char auth_buf[256];
    n = snprintf(auth_buf, sizeof(auth_buf), "Authorization: Bot %.*s", (int)c->token_len,
                 c->token);
    if (n <= 0 || (size_t)n >= sizeof(auth_buf)) {
        if (body)
            c->alloc->free(c->alloc->ctx, body, body_len + 1);
        return HU_ERR_INTERNAL;
    }

    hu_http_response_t resp = {0};
    err = hu_http_post_json(c->alloc, url_buf, auth_buf, body, body_len, &resp);
    if (body)
        c->alloc->free(c->alloc->ctx, body, body_len + 1);
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
#endif
}

static const char *discord_name(void *ctx) {
    (void)ctx;
    return "discord";
}
static bool discord_health_check(void *ctx) {
    (void)ctx;
    return true;
}

#if !HU_IS_TEST
static hu_error_t discord_stream_append(hu_discord_ctx_t *c, const char *delta, size_t delta_len) {
    size_t need = c->stream_text_len + delta_len + 1;
    if (need > c->stream_text_cap) {
        size_t new_cap = c->stream_text_cap ? c->stream_text_cap * 2 : 256;
        while (new_cap < need)
            new_cap *= 2;
        char *p =
            (char *)c->alloc->realloc(c->alloc->ctx, c->stream_text,
                                      c->stream_text_cap ? c->stream_text_cap + 1 : 0, new_cap + 1);
        if (!p)
            return HU_ERR_OUT_OF_MEMORY;
        c->stream_text = p;
        c->stream_text_cap = new_cap;
    }
    memcpy(c->stream_text + c->stream_text_len, delta, delta_len);
    c->stream_text_len += delta_len;
    c->stream_text[c->stream_text_len] = '\0';
    return HU_OK;
}

static void discord_stream_clear(hu_discord_ctx_t *c) {
    if (c->stream_message_id) {
        c->alloc->free(c->alloc->ctx, c->stream_message_id, strlen(c->stream_message_id) + 1);
        c->stream_message_id = NULL;
    }
    if (c->stream_text) {
        c->alloc->free(c->alloc->ctx, c->stream_text, c->stream_text_cap + 1);
        c->stream_text = NULL;
    }
    c->stream_text_len = 0;
    c->stream_text_cap = 0;
}
#endif

static hu_error_t discord_send_event(void *ctx, const char *target, size_t target_len,
                                     const char *message, size_t message_len,
                                     const char *const *media, size_t media_count,
                                     hu_outbound_stage_t stage) {
    (void)media;
    (void)media_count;
    hu_discord_ctx_t *c = (hu_discord_ctx_t *)ctx;
    if (!c || !c->alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (!c->token || c->token_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    if (!target || target_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

#if HU_IS_TEST
    (void)message;
    (void)message_len;
    (void)stage;
    return HU_OK;
#else
    char auth_buf[256];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Authorization: Bot %.*s", (int)c->token_len,
                      c->token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf))
        return HU_ERR_INTERNAL;

    if (stage == HU_OUTBOUND_STAGE_CHUNK) {
        if (message_len > 0 && discord_stream_append(c, message, message_len) != HU_OK)
            return HU_ERR_OUT_OF_MEMORY;
        const char *text = c->stream_text ? c->stream_text : "";
        size_t text_len = c->stream_text ? c->stream_text_len : 0;
        if (!c->stream_message_id) {
            char url_buf[512];
            int n = snprintf(url_buf, sizeof(url_buf), "%s/%.*s/messages", DISCORD_API_BASE,
                             (int)target_len, target);
            if (n < 0 || (size_t)n >= sizeof(url_buf))
                return HU_ERR_INTERNAL;
            char *body = NULL;
            size_t body_len = 0;
            hu_error_t err = build_discord_body(c->alloc, text, text_len, &body, &body_len);
            if (err)
                return err;
            hu_http_response_t resp = {0};
            err = hu_http_post_json(c->alloc, url_buf, auth_buf, body, body_len, &resp);
            if (body)
                c->alloc->free(c->alloc->ctx, body, body_len + 1);
            if (err != HU_OK) {
                if (resp.owned && resp.body)
                    hu_http_response_free(c->alloc, &resp);
                return HU_ERR_CHANNEL_SEND;
            }
            if (resp.status_code >= 200 && resp.status_code < 300 && resp.body) {
                hu_json_value_t *parsed = NULL;
                if (hu_json_parse(c->alloc, resp.body, resp.body_len, &parsed) == HU_OK && parsed) {
                    const char *msg_id = hu_json_get_string(parsed, "id");
                    if (msg_id) {
                        size_t id_len = strlen(msg_id);
                        c->stream_message_id = (char *)c->alloc->alloc(c->alloc->ctx, id_len + 1);
                        if (c->stream_message_id) {
                            memcpy(c->stream_message_id, msg_id, id_len + 1);
                        }
                    }
                    hu_json_free(c->alloc, parsed);
                }
            }
            if (resp.owned && resp.body)
                hu_http_response_free(c->alloc, &resp);
        } else {
            char url_buf[512];
            int n = snprintf(url_buf, sizeof(url_buf), "%s/%.*s/messages/%s", DISCORD_API_BASE,
                             (int)target_len, target, c->stream_message_id);
            if (n < 0 || (size_t)n >= sizeof(url_buf))
                return HU_ERR_INTERNAL;
            char *body = NULL;
            size_t body_len = 0;
            hu_error_t err = build_discord_body(c->alloc, text, text_len, &body, &body_len);
            if (err)
                return err;
            hu_http_response_t resp = {0};
            err = hu_http_patch_json(c->alloc, url_buf, auth_buf, body, body_len, &resp);
            if (body)
                c->alloc->free(c->alloc->ctx, body, body_len + 1);
            if (resp.owned && resp.body)
                hu_http_response_free(c->alloc, &resp);
            if (err != HU_OK)
                return HU_ERR_CHANNEL_SEND;
        }
    } else {
        /* FINAL */
        const char *text = message_len > 0 ? message : (c->stream_text ? c->stream_text : "");
        size_t text_len = message_len > 0 ? message_len : (c->stream_text ? c->stream_text_len : 0);
        if (c->stream_message_id) {
            char url_buf[512];
            int n = snprintf(url_buf, sizeof(url_buf), "%s/%.*s/messages/%s", DISCORD_API_BASE,
                             (int)target_len, target, c->stream_message_id);
            if (n < 0 || (size_t)n >= sizeof(url_buf)) {
                discord_stream_clear(c);
                return HU_ERR_INTERNAL;
            }
            char *body = NULL;
            size_t body_len = 0;
            hu_error_t err = build_discord_body(c->alloc, text, text_len, &body, &body_len);
            if (err) {
                discord_stream_clear(c);
                return err;
            }
            char headers_buf[320];
            int nh = snprintf(headers_buf, sizeof(headers_buf),
                              "Content-Type: application/json\r\n%s", auth_buf);
            if (nh <= 0 || (size_t)nh >= sizeof(headers_buf)) {
                if (body)
                    c->alloc->free(c->alloc->ctx, body, body_len + 1);
                discord_stream_clear(c);
                return HU_ERR_INTERNAL;
            }
            hu_http_response_t resp = {0};
            err = hu_http_request(c->alloc, url_buf, "PATCH", headers_buf, body, body_len, &resp);
            if (body)
                c->alloc->free(c->alloc->ctx, body, body_len + 1);
            discord_stream_clear(c);
            if (resp.owned && resp.body)
                hu_http_response_free(c->alloc, &resp);
            if (err != HU_OK)
                return HU_ERR_CHANNEL_SEND;
        } else {
            char url_buf[512];
            int n = snprintf(url_buf, sizeof(url_buf), "%s/%.*s/messages", DISCORD_API_BASE,
                             (int)target_len, target);
            if (n < 0 || (size_t)n >= sizeof(url_buf)) {
                discord_stream_clear(c);
                return HU_ERR_INTERNAL;
            }
            char *body = NULL;
            size_t body_len = 0;
            hu_error_t err = build_discord_body(c->alloc, text, text_len, &body, &body_len);
            if (err) {
                discord_stream_clear(c);
                return err;
            }
            hu_http_response_t resp = {0};
            err = hu_http_post_json(c->alloc, url_buf, auth_buf, body, body_len, &resp);
            if (body)
                c->alloc->free(c->alloc->ctx, body, body_len + 1);
            discord_stream_clear(c);
            if (resp.owned && resp.body)
                hu_http_response_free(c->alloc, &resp);
            if (err != HU_OK || resp.status_code < 200 || resp.status_code >= 300)
                return HU_ERR_CHANNEL_SEND;
        }
    }
    return HU_OK;
#endif
}

static const hu_channel_vtable_t discord_vtable = {
    .start = discord_start,
    .stop = discord_stop,
    .send = discord_send,
    .name = discord_name,
    .health_check = discord_health_check,
    .send_event = discord_send_event,
    .start_typing = NULL,
    .stop_typing = NULL,
};

/* ─── Webhook inbound queue ────────────────────────────────────────────── */

static void discord_queue_push(hu_discord_ctx_t *c, const char *from, size_t from_len,
                               const char *body, size_t body_len) {
    if (c->queue_count >= DISCORD_QUEUE_MAX)
        return;
    hu_discord_queued_msg_t *slot = &c->queue[c->queue_tail];
    size_t sk = from_len < DISCORD_SESSION_KEY_MAX ? from_len : DISCORD_SESSION_KEY_MAX;
    memcpy(slot->session_key, from, sk);
    slot->session_key[sk] = '\0';
    size_t ct = body_len < DISCORD_CONTENT_MAX ? body_len : DISCORD_CONTENT_MAX;
    memcpy(slot->content, body, ct);
    slot->content[ct] = '\0';
    c->queue_tail = (c->queue_tail + 1) % DISCORD_QUEUE_MAX;
    c->queue_count++;
}

hu_error_t hu_discord_on_webhook(void *channel_ctx, hu_allocator_t *alloc, const char *body,
                                 size_t body_len) {
    hu_discord_ctx_t *c = (hu_discord_ctx_t *)channel_ctx;
    if (!c || !body || body_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    (void)alloc;
    discord_queue_push(c, "test-sender", 11, body, body_len);
    return HU_OK;
#else
    hu_json_value_t *parsed = NULL;
    hu_error_t err = hu_json_parse(alloc, body, body_len, &parsed);
    if (err != HU_OK || !parsed)
        return HU_OK;

    /*
     * Discord Interactions webhook payload:
     *   { "type": 0, "d": { "channel_id": "...", "author": { "id": "...", "bot": false },
     *     "content": "..." } }
     * Discord also sends via the gateway events forwarded as webhooks:
     *   { "t": "MESSAGE_CREATE", "d": { ... } }
     * Handle both: look for "d" envelope or flat structure.
     */
    hu_json_value_t *d_obj = hu_json_object_get(parsed, "d");
    hu_json_value_t *msg = d_obj ? d_obj : parsed;

    hu_json_value_t *author = hu_json_object_get(msg, "author");
    if (author && author->type == HU_JSON_OBJECT) {
        bool is_bot = hu_json_get_bool(author, "bot", false);
        if (is_bot) {
            hu_json_free(alloc, parsed);
            return HU_OK;
        }
    }

    const char *content = hu_json_get_string(msg, "content");
    if (!content || strlen(content) == 0) {
        hu_json_free(alloc, parsed);
        return HU_OK;
    }

    const char *channel_id = hu_json_get_string(msg, "channel_id");
    const char *session = channel_id ? channel_id : "unknown";

    discord_queue_push(c, session, strlen(session), content, strlen(content));
    hu_json_free(alloc, parsed);
    return HU_OK;
#endif
}

/* ─── REST polling (GET /channels/{id}/messages) ─────────────────────────── */

hu_error_t hu_discord_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                           size_t max_msgs, size_t *out_count) {
    hu_discord_ctx_t *ctx = (hu_discord_ctx_t *)channel_ctx;
    if (!ctx || !msgs || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;

#if HU_IS_TEST
    if (ctx->mock_count > 0) {
        (void)alloc;
        size_t n = ctx->mock_count < max_msgs ? ctx->mock_count : max_msgs;
        for (size_t i = 0; i < n; i++) {
            memcpy(msgs[i].session_key, ctx->mock_msgs[i].session_key, 128);
            memcpy(msgs[i].content, ctx->mock_msgs[i].content, 4096);
        }
        *out_count = n;
        ctx->mock_count = 0;
        return HU_OK;
    }
    /* Drain webhook queue in test mode too */
    {
        size_t cnt = 0;
        while (ctx->queue_count > 0 && cnt < max_msgs) {
            hu_discord_queued_msg_t *slot = &ctx->queue[ctx->queue_head];
            memcpy(msgs[cnt].session_key, slot->session_key, sizeof(slot->session_key));
            memcpy(msgs[cnt].content, slot->content, sizeof(slot->content));
            ctx->queue_head = (ctx->queue_head + 1) % DISCORD_QUEUE_MAX;
            ctx->queue_count--;
            cnt++;
        }
        *out_count = cnt;
    }
    return HU_OK;
#else
    /* Drain webhook queue first */
    {
        size_t cnt = 0;
        while (ctx->queue_count > 0 && cnt < max_msgs) {
            hu_discord_queued_msg_t *slot = &ctx->queue[ctx->queue_head];
            memcpy(msgs[cnt].session_key, slot->session_key, sizeof(slot->session_key));
            memcpy(msgs[cnt].content, slot->content, sizeof(slot->content));
            ctx->queue_head = (ctx->queue_head + 1) % DISCORD_QUEUE_MAX;
            ctx->queue_count--;
            cnt++;
        }
        if (cnt > 0) {
            *out_count = cnt;
            return HU_OK;
        }
    }

    if (!ctx->token || ctx->token_len == 0)
        return HU_OK;
    if (!ctx->channel_ids || ctx->channel_ids_count == 0)
        return HU_OK;
    if (!ctx->running)
        return HU_OK;

    char auth_buf[256];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Authorization: Bot %.*s", (int)ctx->token_len,
                      ctx->token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf))
        return HU_ERR_INTERNAL;

    size_t cnt = 0;
    for (size_t ch_idx = 0; ch_idx < ctx->channel_ids_count && cnt < max_msgs; ch_idx++) {
        const char *ch_id = ctx->channel_ids[ch_idx];
        if (!ch_id)
            continue;
        size_t ch_id_len = strlen(ch_id);

        char url_buf[384];
        const char *last_id = (ctx->last_message_ids && ctx->last_message_ids[ch_idx])
                                  ? ctx->last_message_ids[ch_idx]
                                  : NULL;
        int nu;
        if (last_id && *last_id) {
            nu = snprintf(url_buf, sizeof(url_buf), "%s/%.*s/messages?after=%s&limit=10",
                          DISCORD_API_BASE, (int)ch_id_len, ch_id, last_id);
        } else {
            nu = snprintf(url_buf, sizeof(url_buf), "%s/%.*s/messages?limit=10", DISCORD_API_BASE,
                          (int)ch_id_len, ch_id);
        }
        if (nu < 0 || (size_t)nu >= sizeof(url_buf))
            continue;

        hu_http_response_t resp = {0};
        hu_error_t err = hu_http_get(alloc, url_buf, auth_buf, &resp);
        if (err != HU_OK) {
            if (resp.owned && resp.body)
                hu_http_response_free(alloc, &resp);
            continue;
        }
        if (resp.status_code != 200 || !resp.body || resp.body_len == 0) {
            if (resp.owned && resp.body)
                hu_http_response_free(alloc, &resp);
            continue;
        }

        hu_json_value_t *parsed = NULL;
        err = hu_json_parse(alloc, resp.body, resp.body_len, &parsed);
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        if (err != HU_OK || !parsed || parsed->type != HU_JSON_ARRAY) {
            if (parsed)
                hu_json_free(alloc, parsed);
            continue;
        }

        /* Discord returns newest first; process oldest first for correct last_message_id */
        size_t arr_len = parsed->data.array.len;
        for (size_t j = arr_len; j > 0 && cnt < max_msgs; j--) {
            size_t i = j - 1;
            hu_json_value_t *msg = parsed->data.array.items[i];
            if (!msg || msg->type != HU_JSON_OBJECT)
                continue;

            hu_json_value_t *author = hu_json_object_get(msg, "author");
            if (!author || author->type != HU_JSON_OBJECT)
                continue;
            bool is_bot = hu_json_get_bool(author, "bot", false);
            /* Skip bot messages (includes own messages when we are a bot) */
            if (is_bot)
                continue;

            const char *content = hu_json_get_string(msg, "content");
            if (!content)
                content = "";
            size_t content_len = strlen(content);
            if (content_len == 0)
                continue;

            const char *msg_id = hu_json_get_string(msg, "id");
            if (msg_id && ctx->alloc) {
                if (ctx->last_message_ids && ctx->last_message_ids[ch_idx]) {
                    ctx->alloc->free(ctx->alloc->ctx, ctx->last_message_ids[ch_idx],
                                     strlen(ctx->last_message_ids[ch_idx]) + 1);
                }
                size_t id_len = strlen(msg_id);
                ctx->last_message_ids[ch_idx] =
                    (char *)ctx->alloc->alloc(ctx->alloc->ctx, id_len + 1);
                if (ctx->last_message_ids[ch_idx]) {
                    memcpy(ctx->last_message_ids[ch_idx], msg_id, id_len + 1);
                }
            }

            size_t sk_len =
                ch_id_len < DISCORD_SESSION_KEY_MAX ? ch_id_len : DISCORD_SESSION_KEY_MAX;
            memcpy(msgs[cnt].session_key, ch_id, sk_len);
            msgs[cnt].session_key[sk_len] = '\0';
            size_t ct_len = content_len < DISCORD_CONTENT_MAX ? content_len : DISCORD_CONTENT_MAX;
            memcpy(msgs[cnt].content, content, ct_len);
            msgs[cnt].content[ct_len] = '\0';
            cnt++;
        }
        hu_json_free(alloc, parsed);
    }
    *out_count = cnt;
    return HU_OK;
#endif
}

hu_error_t hu_discord_create(hu_allocator_t *alloc, const char *token, size_t token_len,
                             hu_channel_t *out) {
    return hu_discord_create_ex(alloc, token, token_len, NULL, 0, NULL, 0, out);
}

hu_error_t hu_discord_create_ex(hu_allocator_t *alloc, const char *token, size_t token_len,
                                const char *const *channel_ids, size_t channel_ids_count,
                                const char *bot_id, size_t bot_id_len, hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (channel_ids_count > DISCORD_MAX_CHANNELS)
        return HU_ERR_INVALID_ARGUMENT;

    hu_discord_ctx_t *c = (hu_discord_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;

    if (token && token_len > 0) {
        c->token = (char *)alloc->alloc(alloc->ctx, token_len + 1);
        if (!c->token) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->token, token, token_len);
        c->token[token_len] = '\0';
        c->token_len = token_len;
    }

    if (channel_ids && channel_ids_count > 0) {
        c->channel_ids = (char **)alloc->alloc(alloc->ctx, channel_ids_count * sizeof(char *));
        if (!c->channel_ids) {
            if (c->token)
                alloc->free(alloc->ctx, c->token, token_len + 1);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memset(c->channel_ids, 0, channel_ids_count * sizeof(char *));
        c->last_message_ids = (char **)alloc->alloc(alloc->ctx, channel_ids_count * sizeof(char *));
        if (!c->last_message_ids) {
            alloc->free(alloc->ctx, c->channel_ids, channel_ids_count * sizeof(char *));
            if (c->token)
                alloc->free(alloc->ctx, c->token, token_len + 1);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memset(c->last_message_ids, 0, channel_ids_count * sizeof(char *));
        for (size_t i = 0; i < channel_ids_count; i++) {
            if (channel_ids[i]) {
                size_t len = strlen(channel_ids[i]);
                c->channel_ids[i] = (char *)alloc->alloc(alloc->ctx, len + 1);
                if (!c->channel_ids[i]) {
                    for (size_t j = 0; j < i; j++)
                        alloc->free(alloc->ctx, c->channel_ids[j], strlen(c->channel_ids[j]) + 1);
                    alloc->free(alloc->ctx, c->channel_ids, channel_ids_count * sizeof(char *));
                    alloc->free(alloc->ctx, c->last_message_ids,
                                channel_ids_count * sizeof(char *));
                    if (c->token)
                        alloc->free(alloc->ctx, c->token, token_len + 1);
                    alloc->free(alloc->ctx, c, sizeof(*c));
                    return HU_ERR_OUT_OF_MEMORY;
                }
                memcpy(c->channel_ids[i], channel_ids[i], len + 1);
            }
        }
        c->channel_ids_count = channel_ids_count;
    }

    if (bot_id && bot_id_len > 0) {
        c->bot_id = (char *)alloc->alloc(alloc->ctx, bot_id_len + 1);
        if (!c->bot_id) {
            hu_channel_t tmp = {.ctx = c, .vtable = &discord_vtable};
            hu_discord_destroy(&tmp);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->bot_id, bot_id, bot_id_len);
        c->bot_id[bot_id_len] = '\0';
        c->bot_id_len = bot_id_len;
    }

    out->ctx = c;
    out->vtable = &discord_vtable;
    return HU_OK;
}

#if HU_IS_TEST
hu_error_t hu_discord_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                       size_t session_key_len, const char *content,
                                       size_t content_len) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_discord_ctx_t *c = (hu_discord_ctx_t *)ch->ctx;
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

const char *hu_discord_test_get_last_message(hu_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    hu_discord_ctx_t *c = (hu_discord_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif

void hu_discord_destroy(hu_channel_t *ch) {
    if (ch && ch->ctx) {
        hu_discord_ctx_t *c = (hu_discord_ctx_t *)ch->ctx;
#if !HU_IS_TEST
        discord_stream_clear(c);
#endif
        if (c->alloc) {
            if (c->token)
                c->alloc->free(c->alloc->ctx, c->token, c->token_len + 1);
            if (c->bot_id)
                c->alloc->free(c->alloc->ctx, c->bot_id, c->bot_id_len + 1);
            if (c->channel_ids) {
                for (size_t i = 0; i < c->channel_ids_count; i++) {
                    if (c->channel_ids[i])
                        c->alloc->free(c->alloc->ctx, c->channel_ids[i],
                                       strlen(c->channel_ids[i]) + 1);
                }
                c->alloc->free(c->alloc->ctx, c->channel_ids,
                               c->channel_ids_count * sizeof(char *));
            }
            if (c->last_message_ids) {
                for (size_t i = 0; i < c->channel_ids_count; i++) {
                    if (c->last_message_ids[i])
                        c->alloc->free(c->alloc->ctx, c->last_message_ids[i],
                                       strlen(c->last_message_ids[i]) + 1);
                }
                c->alloc->free(c->alloc->ctx, c->last_message_ids,
                               c->channel_ids_count * sizeof(char *));
            }
            c->alloc->free(c->alloc->ctx, c, sizeof(*c));
        }
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
