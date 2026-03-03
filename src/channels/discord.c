#include "seaclaw/channels/discord.h"
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

#define DISCORD_API_BASE        "https://discord.com/api/v10/channels"
#define DISCORD_MAX_CHANNELS    16
#define DISCORD_SESSION_KEY_MAX 127
#define DISCORD_CONTENT_MAX     4095

typedef struct sc_discord_ctx {
    sc_allocator_t *alloc;
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
} sc_discord_ctx_t;

#if !SC_IS_TEST
static sc_error_t build_discord_body(sc_allocator_t *alloc, const char *content, size_t content_len,
                                     char **out, size_t *out_len) {
    sc_json_buf_t jbuf;
    sc_error_t err = sc_json_buf_init(&jbuf, alloc);
    if (err)
        return err;
    err = sc_json_buf_append_raw(&jbuf, "{\"content\":", 11);
    if (err)
        goto fail;
    err = sc_json_append_string(&jbuf, content, content_len);
    if (err)
        goto fail;
    err = sc_json_buf_append_raw(&jbuf, "}", 1);
    if (err)
        goto fail;
    *out_len = jbuf.len;
    *out = (char *)alloc->alloc(alloc->ctx, jbuf.len + 1);
    if (!*out) {
        err = SC_ERR_OUT_OF_MEMORY;
        goto fail;
    }
    memcpy(*out, jbuf.ptr, jbuf.len + 1);
    sc_json_buf_free(&jbuf);
    return SC_OK;
fail:
    sc_json_buf_free(&jbuf);
    return err;
}
#endif

static sc_error_t discord_start(void *ctx) {
    sc_discord_ctx_t *c = (sc_discord_ctx_t *)ctx;
    if (!c)
        return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void discord_stop(void *ctx) {
    sc_discord_ctx_t *c = (sc_discord_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static sc_error_t discord_send(void *ctx, const char *target, size_t target_len,
                               const char *message, size_t message_len, const char *const *media,
                               size_t media_count) {
    (void)media;
    (void)media_count;
    sc_discord_ctx_t *c = (sc_discord_ctx_t *)ctx;
    if (!c || !c->alloc)
        return SC_ERR_INVALID_ARGUMENT;
    if (!c->token || c->token_len == 0)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;
    if (!target || target_len == 0 || !message)
        return SC_ERR_INVALID_ARGUMENT;

#if SC_IS_TEST
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    (void)media;
    (void)media_count;
    return SC_OK;
#else
    char url_buf[512];
    int n = snprintf(url_buf, sizeof(url_buf), "%s/%.*s/messages", DISCORD_API_BASE,
                     (int)target_len, target);
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return SC_ERR_INTERNAL;

    char *body = NULL;
    size_t body_len = 0;
    sc_error_t err = build_discord_body(c->alloc, message, message_len, &body, &body_len);
    if (err)
        return err;

    char auth_buf[256];
    n = snprintf(auth_buf, sizeof(auth_buf), "Authorization: Bot %.*s", (int)c->token_len,
                 c->token);
    if (n <= 0 || (size_t)n >= sizeof(auth_buf)) {
        if (body)
            c->alloc->free(c->alloc->ctx, body, body_len + 1);
        return SC_ERR_INTERNAL;
    }

    sc_http_response_t resp = {0};
    err = sc_http_post_json(c->alloc, url_buf, auth_buf, body, body_len, &resp);
    if (body)
        c->alloc->free(c->alloc->ctx, body, body_len + 1);
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

static const sc_channel_vtable_t discord_vtable = {
    .start = discord_start,
    .stop = discord_stop,
    .send = discord_send,
    .name = discord_name,
    .health_check = discord_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

/* ─── REST polling (GET /channels/{id}/messages) ─────────────────────────── */

sc_error_t sc_discord_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                           size_t max_msgs, size_t *out_count) {
    sc_discord_ctx_t *ctx = (sc_discord_ctx_t *)channel_ctx;
    if (!ctx || !msgs || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out_count = 0;

#if SC_IS_TEST
    (void)alloc;
    (void)max_msgs;
    return SC_OK;
#else
    if (!ctx->token || ctx->token_len == 0)
        return SC_OK;
    if (!ctx->channel_ids || ctx->channel_ids_count == 0)
        return SC_OK;
    if (!ctx->running)
        return SC_OK;

    char auth_buf[256];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Authorization: Bot %.*s", (int)ctx->token_len,
                      ctx->token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf))
        return SC_ERR_INTERNAL;

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

        sc_http_response_t resp = {0};
        sc_error_t err = sc_http_get(alloc, url_buf, auth_buf, &resp);
        if (err != SC_OK) {
            if (resp.owned && resp.body)
                sc_http_response_free(alloc, &resp);
            continue;
        }
        if (resp.status_code != 200 || !resp.body || resp.body_len == 0) {
            if (resp.owned && resp.body)
                sc_http_response_free(alloc, &resp);
            continue;
        }

        sc_json_value_t *parsed = NULL;
        err = sc_json_parse(alloc, resp.body, resp.body_len, &parsed);
        if (resp.owned && resp.body)
            sc_http_response_free(alloc, &resp);
        if (err != SC_OK || !parsed || parsed->type != SC_JSON_ARRAY)
            continue;

        /* Discord returns newest first; process oldest first for correct last_message_id */
        size_t arr_len = parsed->data.array.len;
        for (size_t j = arr_len; j > 0 && cnt < max_msgs; j--) {
            size_t i = j - 1;
            sc_json_value_t *msg = parsed->data.array.items[i];
            if (!msg || msg->type != SC_JSON_OBJECT)
                continue;

            sc_json_value_t *author = sc_json_object_get(msg, "author");
            if (!author || author->type != SC_JSON_OBJECT)
                continue;
            bool is_bot = sc_json_get_bool(author, "bot", false);
            /* Skip bot messages (includes own messages when we are a bot) */
            if (is_bot)
                continue;

            const char *content = sc_json_get_string(msg, "content");
            if (!content)
                content = "";
            size_t content_len = strlen(content);
            if (content_len == 0)
                continue;

            const char *msg_id = sc_json_get_string(msg, "id");
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
        sc_json_free(alloc, parsed);
    }
    *out_count = cnt;
    return SC_OK;
#endif
}

sc_error_t sc_discord_create(sc_allocator_t *alloc, const char *token, size_t token_len,
                             sc_channel_t *out) {
    return sc_discord_create_ex(alloc, token, token_len, NULL, 0, NULL, 0, out);
}

sc_error_t sc_discord_create_ex(sc_allocator_t *alloc, const char *token, size_t token_len,
                                const char *const *channel_ids, size_t channel_ids_count,
                                const char *bot_id, size_t bot_id_len, sc_channel_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    if (channel_ids_count > DISCORD_MAX_CHANNELS)
        return SC_ERR_INVALID_ARGUMENT;

    sc_discord_ctx_t *c = (sc_discord_ctx_t *)calloc(1, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;

    if (token && token_len > 0) {
        c->token = (char *)malloc(token_len + 1);
        if (!c->token) {
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->token, token, token_len);
        c->token[token_len] = '\0';
        c->token_len = token_len;
    }

    if (channel_ids && channel_ids_count > 0) {
        c->channel_ids = (char **)calloc(channel_ids_count, sizeof(char *));
        if (!c->channel_ids) {
            if (c->token)
                free(c->token);
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        c->last_message_ids = (char **)calloc(channel_ids_count, sizeof(char *));
        if (!c->last_message_ids) {
            free(c->channel_ids);
            if (c->token)
                free(c->token);
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        for (size_t i = 0; i < channel_ids_count; i++) {
            if (channel_ids[i]) {
                size_t len = strlen(channel_ids[i]);
                c->channel_ids[i] = (char *)malloc(len + 1);
                if (!c->channel_ids[i]) {
                    for (size_t j = 0; j < i; j++)
                        free(c->channel_ids[j]);
                    free(c->channel_ids);
                    free(c->last_message_ids);
                    if (c->token)
                        free(c->token);
                    free(c);
                    return SC_ERR_OUT_OF_MEMORY;
                }
                memcpy(c->channel_ids[i], channel_ids[i], len + 1);
            }
        }
        c->channel_ids_count = channel_ids_count;
    }

    if (bot_id && bot_id_len > 0) {
        c->bot_id = (char *)malloc(bot_id_len + 1);
        if (!c->bot_id) {
            sc_channel_t tmp = {.ctx = c, .vtable = &discord_vtable};
            sc_discord_destroy(&tmp);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->bot_id, bot_id, bot_id_len);
        c->bot_id[bot_id_len] = '\0';
        c->bot_id_len = bot_id_len;
    }

    out->ctx = c;
    out->vtable = &discord_vtable;
    return SC_OK;
}

void sc_discord_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_discord_ctx_t *c = (sc_discord_ctx_t *)ch->ctx;
        if (c->token)
            free(c->token);
        if (c->bot_id)
            free(c->bot_id);
        if (c->channel_ids) {
            for (size_t i = 0; i < c->channel_ids_count; i++) {
                if (c->channel_ids[i])
                    free(c->channel_ids[i]);
            }
            free(c->channel_ids);
        }
        if (c->last_message_ids) {
            if (c->alloc && c->alloc->free) {
                for (size_t i = 0; i < c->channel_ids_count; i++) {
                    if (c->last_message_ids[i])
                        c->alloc->free(c->alloc->ctx, c->last_message_ids[i],
                                       strlen(c->last_message_ids[i]) + 1);
                }
            }
            free(c->last_message_ids);
        }
        free(c);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
