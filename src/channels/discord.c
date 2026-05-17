#include "human/channels/discord.h"
#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/persona.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    /* Persona: borrowed pointer for overlay-aware rendering. */
    const hu_persona_t *persona;
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
        char chat_id[128];
        char guid[96];
        char reply_to_guid[96];
        bool is_group;
        bool has_attachment;
        bool has_video;
        int64_t message_id;
        int64_t timestamp_sec;
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

    /* Persona overlay rendering. See hu_persona_render_for_channel. */
    char *rendered = NULL;
    size_t rendered_len = 0;
    {
        const hu_persona_overlay_t *ov = NULL;
        if (c->persona)
            ov = hu_persona_find_overlay(c->persona, "discord", 7);
        hu_error_t rerr = hu_persona_render_for_channel(ov, message, message_len, c->alloc,
                                                        &rendered, &rendered_len);
        if (rerr != HU_OK)
            return rerr;
        message = rendered;
        message_len = rendered_len;
    }

#if HU_IS_TEST
    {
        size_t len = message_len > 4095 ? 4095 : message_len;
        if (message && len > 0)
            memcpy(c->last_message, message, len);
        c->last_message[len] = '\0';
        c->last_message_len = len;
        if (rendered)
            c->alloc->free(c->alloc->ctx, rendered, rendered_len + 1);
        return HU_OK;
    }
#else
    hu_error_t prod_err = HU_OK;
    char url_buf[512];
    int n = snprintf(url_buf, sizeof(url_buf), "%s/%.*s/messages", DISCORD_API_BASE,
                     (int)target_len, target);
    if (n < 0 || (size_t)n >= sizeof(url_buf)) {
        prod_err = HU_ERR_INTERNAL;
        goto prod_done;
    }

    char *body = NULL;
    size_t body_len = 0;
    hu_error_t err = build_discord_body(c->alloc, message, message_len, &body, &body_len);
    if (err) {
        prod_err = err;
        goto prod_done;
    }

    char auth_buf[256];
    n = snprintf(auth_buf, sizeof(auth_buf), "Authorization: Bot %.*s", (int)c->token_len,
                 c->token);
    if (n <= 0 || (size_t)n >= sizeof(auth_buf)) {
        if (body)
            c->alloc->free(c->alloc->ctx, body, body_len + 1);
        prod_err = HU_ERR_INTERNAL;
        goto prod_done;
    }

    hu_http_response_t resp = {0};
    err = hu_http_post_json(c->alloc, url_buf, auth_buf, body, body_len, &resp);
    if (body)
        c->alloc->free(c->alloc->ctx, body, body_len + 1);
    if (err != HU_OK) {
        if (resp.owned && resp.body)
            hu_http_response_free(c->alloc, &resp);
        prod_err = HU_ERR_CHANNEL_SEND;
        goto prod_done;
    }
    if (resp.owned && resp.body)
        hu_http_response_free(c->alloc, &resp);
    if (resp.status_code < 200 || resp.status_code >= 300) {
        prod_err = HU_ERR_CHANNEL_SEND;
        goto prod_done;
    }
prod_done:
    if (rendered)
        c->alloc->free(c->alloc->ctx, rendered, rendered_len + 1);
    return prod_err;
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
            int patch_status = resp.status_code;
            if (resp.owned && resp.body)
                hu_http_response_free(c->alloc, &resp);
            if (err != HU_OK || patch_status != 200)
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

static hu_error_t discord_stop_typing(void *ctx, const char *recipient, size_t recipient_len) {
    (void)ctx;
    (void)recipient;
    (void)recipient_len;
    return HU_OK;
}

static hu_error_t discord_get_response_constraints(void *ctx,
                                                   hu_channel_response_constraints_t *out) {
    (void)ctx;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    out->max_chars = 400;
    return HU_OK;
}

static const char *discord_reaction_emoji_url(hu_reaction_type_t reaction) {
    switch (reaction) {
    case HU_REACTION_HEART:
        return "%E2%9D%A4%EF%B8%8F";
    case HU_REACTION_THUMBS_UP:
        return "%F0%9F%91%8D";
    case HU_REACTION_THUMBS_DOWN:
        return "%F0%9F%91%8E";
    case HU_REACTION_HAHA:
        return "%F0%9F%98%82";
    case HU_REACTION_EMPHASIS:
        return "%E2%9D%97";
    case HU_REACTION_QUESTION:
        return "%E2%9D%93";
    default:
        return NULL;
    }
}

static hu_error_t discord_start_typing(void *ctx, const char *recipient, size_t recipient_len) {
    hu_discord_ctx_t *c = (hu_discord_ctx_t *)ctx;
    if (!c || !c->alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (!c->token || c->token_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    if (!recipient || recipient_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

#if HU_IS_TEST
    return HU_OK;
#else
    char url_buf[512];
    int n = snprintf(url_buf, sizeof(url_buf), "%s/%.*s/typing", DISCORD_API_BASE,
                     (int)recipient_len, recipient);
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return HU_ERR_INTERNAL;

    char auth_buf[256];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Authorization: Bot %.*s", (int)c->token_len,
                      c->token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf))
        return HU_ERR_INTERNAL;

    char headers_buf[320];
    int nh = snprintf(headers_buf, sizeof(headers_buf), "Content-Length: 0\r\n%s", auth_buf);
    if (nh <= 0 || (size_t)nh >= sizeof(headers_buf))
        return HU_ERR_INTERNAL;

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_request(c->alloc, url_buf, "POST", headers_buf, NULL, 0, &resp);
    if (resp.owned && resp.body)
        hu_http_response_free(c->alloc, &resp);
    if (err != HU_OK)
        return HU_ERR_CHANNEL_SEND;
    if (resp.status_code < 200 || resp.status_code >= 300)
        return HU_ERR_CHANNEL_SEND;
    return HU_OK;
#endif
}

static hu_error_t discord_react(void *ctx, const char *target, size_t target_len,
                                int64_t message_id, hu_reaction_type_t reaction) {
    hu_discord_ctx_t *c = (hu_discord_ctx_t *)ctx;
    if (!c || !c->alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (!c->token || c->token_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    if (!target || target_len == 0 || message_id <= 0)
        return HU_ERR_INVALID_ARGUMENT;
    const char *emoji_enc = discord_reaction_emoji_url(reaction);
    if (!emoji_enc)
        return HU_ERR_INVALID_ARGUMENT;

#if HU_IS_TEST
    return HU_OK;
#else
    char auth_buf[256];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Authorization: Bot %.*s", (int)c->token_len,
                      c->token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf))
        return HU_ERR_INTERNAL;

    char url_buf[640];
    int nu = snprintf(url_buf, sizeof(url_buf), "%s/%.*s/messages/%" PRId64 "/reactions/%s/@me",
                      DISCORD_API_BASE, (int)target_len, target, message_id, emoji_enc);
    if (nu < 0 || (size_t)nu >= sizeof(url_buf))
        return HU_ERR_INTERNAL;

    char headers_buf[320];
    int nh = snprintf(headers_buf, sizeof(headers_buf), "Content-Length: 0\r\n%s", auth_buf);
    if (nh <= 0 || (size_t)nh >= sizeof(headers_buf))
        return HU_ERR_INTERNAL;

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_request(c->alloc, url_buf, "PUT", headers_buf, NULL, 0, &resp);
    if (resp.owned && resp.body)
        hu_http_response_free(c->alloc, &resp);
    if (err != HU_OK)
        return HU_ERR_CHANNEL_SEND;
    if (resp.status_code < 200 || resp.status_code >= 300)
        return HU_ERR_CHANNEL_SEND;
    return HU_OK;
#endif
}

static hu_error_t discord_load_conversation_history(void *ctx, hu_allocator_t *alloc,
                                                    const char *contact_id, size_t contact_id_len,
                                                    size_t limit, hu_channel_history_entry_t **out,
                                                    size_t *out_count) {
    hu_discord_ctx_t *c = (hu_discord_ctx_t *)ctx;
    if (!c || !alloc || !contact_id || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

#if HU_IS_TEST
    (void)contact_id_len;
    (void)limit;
    return HU_OK;
#else
    if (!c->token || c->token_len == 0)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    if (limit == 0)
        return HU_OK;

    size_t lim = limit > 100 ? 100 : limit;

    char url_buf[512];
    int nu = snprintf(url_buf, sizeof(url_buf), "%s/%.*s/messages?limit=%zu", DISCORD_API_BASE,
                      (int)contact_id_len, contact_id, lim);
    if (nu < 0 || (size_t)nu >= sizeof(url_buf))
        return HU_ERR_INTERNAL;

    char auth_buf[256];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Authorization: Bot %.*s", (int)c->token_len,
                      c->token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf))
        return HU_ERR_INTERNAL;

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(alloc, url_buf, auth_buf, &resp);
    if (err != HU_OK || resp.status_code != 200 || !resp.body || resp.body_len == 0) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return err != HU_OK ? err : HU_ERR_CHANNEL_SEND;
    }

    hu_json_value_t *parsed = NULL;
    err = hu_json_parse(alloc, resp.body, resp.body_len, &parsed);
    if (resp.owned && resp.body)
        hu_http_response_free(alloc, &resp);
    if (err != HU_OK || !parsed || parsed->type != HU_JSON_ARRAY) {
        if (parsed)
            hu_json_free(alloc, parsed);
        return err != HU_OK ? err : HU_ERR_INTERNAL;
    }

    size_t arr_len = parsed->data.array.len;
    if (arr_len == 0) {
        hu_json_free(alloc, parsed);
        return HU_OK;
    }

    hu_channel_history_entry_t *entries =
        (hu_channel_history_entry_t *)alloc->alloc(alloc->ctx, arr_len * sizeof(*entries));
    if (!entries) {
        hu_json_free(alloc, parsed);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(entries, 0, arr_len * sizeof(*entries));
    size_t count = 0;

    /* Discord returns newest first; emit chronological (oldest first). */
    for (size_t j = arr_len; j > 0; j--) {
        size_t i = j - 1;
        hu_json_value_t *msg = parsed->data.array.items[i];
        if (!msg || msg->type != HU_JSON_OBJECT)
            continue;

        hu_json_value_t *author = hu_json_object_get(msg, "author");
        if (!author || author->type != HU_JSON_OBJECT)
            continue;

        const char *content = hu_json_get_string(msg, "content");
        if (!content)
            content = "";
        size_t content_len = strlen(content);
        if (content_len == 0)
            continue;

        const char *author_id = hu_json_get_string(author, "id");
        bool from_me = false;
        if (c->bot_id && c->bot_id_len > 0 && author_id)
            from_me = strcmp(author_id, c->bot_id) == 0;
        else
            from_me = hu_json_get_bool(author, "bot", false);

        size_t tlen = content_len;
        if (tlen > sizeof(entries[0].text) - 1)
            tlen = sizeof(entries[0].text) - 1;
        memcpy(entries[count].text, content, tlen);
        entries[count].text[tlen] = '\0';
        entries[count].from_me = from_me;

        const char *ts = hu_json_get_string(msg, "timestamp");
        if (ts) {
            size_t tslen = strlen(ts);
            if (tslen > sizeof(entries[0].timestamp) - 1)
                tslen = sizeof(entries[0].timestamp) - 1;
            memcpy(entries[count].timestamp, ts, tslen);
            entries[count].timestamp[tslen] = '\0';
        }

        count++;
    }

    hu_json_free(alloc, parsed);

    if (count == 0) {
        alloc->free(alloc->ctx, entries, arr_len * sizeof(*entries));
        return HU_OK;
    }

    /* Callers free with entry_count * sizeof — allocation must match exactly. */
    if (count < arr_len) {
        hu_channel_history_entry_t *exact =
            (hu_channel_history_entry_t *)alloc->alloc(alloc->ctx, count * sizeof(*entries));
        if (!exact) {
            alloc->free(alloc->ctx, entries, arr_len * sizeof(*entries));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(exact, entries, count * sizeof(*entries));
        alloc->free(alloc->ctx, entries, arr_len * sizeof(*entries));
        entries = exact;
    }

    *out = entries;
    *out_count = count;
    return HU_OK;
#endif
}

static char *discord_get_attachment_path(void *ctx, hu_allocator_t *alloc, int64_t message_id) {
#if HU_IS_TEST
    (void)ctx;
    (void)alloc;
    (void)message_id;
    return NULL;
#else
    hu_discord_ctx_t *c = (hu_discord_ctx_t *)ctx;
    if (!c || !alloc || !c->token || c->token_len == 0 || message_id <= 0)
        return NULL;
    if (!c->channel_ids || c->channel_ids_count == 0)
        return NULL;

    char auth_buf[256];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Authorization: Bot %.*s", (int)c->token_len,
                      c->token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf))
        return NULL;

    char mid[32];
    int nm = snprintf(mid, sizeof(mid), "%" PRId64, message_id);
    if (nm <= 0 || (size_t)nm >= sizeof(mid))
        return NULL;

    for (size_t ch_idx = 0; ch_idx < c->channel_ids_count; ch_idx++) {
        const char *ch_id = c->channel_ids[ch_idx];
        if (!ch_id)
            continue;

        char url_buf[512];
        int nu =
            snprintf(url_buf, sizeof(url_buf), "%s/%s/messages/%s", DISCORD_API_BASE, ch_id, mid);
        if (nu < 0 || (size_t)nu >= sizeof(url_buf))
            continue;

        hu_http_response_t resp = {0};
        hu_error_t err = hu_http_get(alloc, url_buf, auth_buf, &resp);
        if (err != HU_OK || resp.status_code != 200 || !resp.body) {
            if (resp.owned && resp.body)
                hu_http_response_free(alloc, &resp);
            continue;
        }

        hu_json_value_t *parsed = NULL;
        err = hu_json_parse(alloc, resp.body, resp.body_len, &parsed);
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        if (err != HU_OK || !parsed || parsed->type != HU_JSON_OBJECT) {
            if (parsed)
                hu_json_free(alloc, parsed);
            continue;
        }

        hu_json_value_t *atts = hu_json_object_get(parsed, "attachments");
        if (!atts || atts->type != HU_JSON_ARRAY || atts->data.array.len == 0) {
            hu_json_free(alloc, parsed);
            continue;
        }

        hu_json_value_t *a0 = atts->data.array.items[0];
        const char *url = (a0 && a0->type == HU_JSON_OBJECT) ? hu_json_get_string(a0, "url") : NULL;
        if (!url) {
            hu_json_free(alloc, parsed);
            continue;
        }

        size_t ulen = strlen(url);
        char *copy = (char *)alloc->alloc(alloc->ctx, ulen + 1);
        if (!copy) {
            hu_json_free(alloc, parsed);
            return NULL;
        }
        memcpy(copy, url, ulen + 1);
        hu_json_free(alloc, parsed);
        return copy;
    }

    return NULL;
#endif
}

static bool discord_human_active_recently(void *ctx, const char *contact, size_t contact_len,
                                          int window_sec) {
    (void)ctx;
    (void)contact;
    (void)contact_len;
    (void)window_sec;
    return false;
}

static const hu_channel_vtable_t discord_vtable = {
    .start = discord_start,
    .stop = discord_stop,
    .send = discord_send,
    .name = discord_name,
    .health_check = discord_health_check,
    .send_event = discord_send_event,
    .start_typing = discord_start_typing,
    .stop_typing = discord_stop_typing,
    .load_conversation_history = discord_load_conversation_history,
    .get_response_constraints = discord_get_response_constraints,
    .react = discord_react,
    .get_attachment_path = discord_get_attachment_path,
    .human_active_recently = discord_human_active_recently,
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
            memcpy(msgs[i].chat_id, ctx->mock_msgs[i].chat_id, sizeof(msgs[i].chat_id));
            memcpy(msgs[i].guid, ctx->mock_msgs[i].guid, sizeof(msgs[i].guid));
            memcpy(msgs[i].reply_to_guid, ctx->mock_msgs[i].reply_to_guid,
                   sizeof(msgs[i].reply_to_guid));
            msgs[i].is_group = ctx->mock_msgs[i].is_group;
            msgs[i].has_attachment = ctx->mock_msgs[i].has_attachment;
            msgs[i].has_video = ctx->mock_msgs[i].has_video;
            msgs[i].message_id = ctx->mock_msgs[i].message_id;
            msgs[i].timestamp_sec = ctx->mock_msgs[i].timestamp_sec;
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

            /* Message ID (Discord snowflake → int64) */
            if (msg_id)
                msgs[cnt].message_id = strtoll(msg_id, NULL, 10);

            /* Timestamp from Discord ISO 8601 (extract Unix epoch) */
            const char *ts = hu_json_get_string(msg, "timestamp");
            if (ts) {
                struct tm tm_val;
                memset(&tm_val, 0, sizeof(tm_val));
                if (sscanf(ts, "%d-%d-%dT%d:%d:%d", &tm_val.tm_year, &tm_val.tm_mon,
                           &tm_val.tm_mday, &tm_val.tm_hour, &tm_val.tm_min, &tm_val.tm_sec) >= 6) {
                    tm_val.tm_year -= 1900;
                    tm_val.tm_mon -= 1;
                    msgs[cnt].timestamp_sec = (int64_t)timegm(&tm_val);
                }
            }

            /* Guild channels are always group contexts */
            msgs[cnt].is_group = true;
            memcpy(msgs[cnt].chat_id, ch_id, sk_len);
            msgs[cnt].chat_id[sk_len] = '\0';

            /* Attachments */
            hu_json_value_t *attachments = hu_json_object_get(msg, "attachments");
            if (attachments && attachments->type == HU_JSON_ARRAY &&
                attachments->data.array.len > 0) {
                msgs[cnt].has_attachment = true;
                for (size_t ai = 0; ai < attachments->data.array.len; ai++) {
                    hu_json_value_t *att = attachments->data.array.items[ai];
                    if (!att || att->type != HU_JSON_OBJECT)
                        continue;
                    const char *ct_type = hu_json_get_string(att, "content_type");
                    if (ct_type && (strstr(ct_type, "video/") == ct_type)) {
                        msgs[cnt].has_video = true;
                        break;
                    }
                }
            }

            /* Inline reply context */
            hu_json_value_t *ref = hu_json_object_get(msg, "message_reference");
            if (ref && ref->type == HU_JSON_OBJECT) {
                const char *ref_id = hu_json_get_string(ref, "message_id");
                if (ref_id) {
                    size_t rl = strlen(ref_id);
                    if (rl > 95)
                        rl = 95;
                    memcpy(msgs[cnt].reply_to_guid, ref_id, rl);
                    msgs[cnt].reply_to_guid[rl] = '\0';
                }
            }

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

hu_error_t hu_discord_test_inject_mock_full(hu_channel_t *ch, const char *session_key,
                                            size_t session_key_len, const char *content,
                                            size_t content_len,
                                            const hu_discord_test_msg_opts_t *opts) {
    if (!ch || !ch->ctx || !opts)
        return HU_ERR_INVALID_ARGUMENT;
    hu_discord_ctx_t *c = (hu_discord_ctx_t *)ch->ctx;
    if (c->mock_count >= 8)
        return HU_ERR_OUT_OF_MEMORY;
    size_t i = c->mock_count++;
    memset(&c->mock_msgs[i], 0, sizeof(c->mock_msgs[i]));

    size_t sk = session_key_len > 127 ? 127 : session_key_len;
    if (session_key && sk > 0)
        memcpy(c->mock_msgs[i].session_key, session_key, sk);
    c->mock_msgs[i].session_key[sk] = '\0';

    size_t ct = content_len > 4095 ? 4095 : content_len;
    if (content && ct > 0)
        memcpy(c->mock_msgs[i].content, content, ct);
    c->mock_msgs[i].content[ct] = '\0';

    c->mock_msgs[i].is_group = opts->is_group;
    c->mock_msgs[i].has_attachment = opts->has_attachment;
    c->mock_msgs[i].has_video = opts->has_video;
    c->mock_msgs[i].message_id = opts->message_id;
    c->mock_msgs[i].timestamp_sec = opts->timestamp_sec;

    if (opts->chat_id) {
        size_t cl = strlen(opts->chat_id);
        if (cl > 127)
            cl = 127;
        memcpy(c->mock_msgs[i].chat_id, opts->chat_id, cl);
        c->mock_msgs[i].chat_id[cl] = '\0';
    }
    if (opts->guid) {
        size_t gl = strlen(opts->guid);
        if (gl > 95)
            gl = 95;
        memcpy(c->mock_msgs[i].guid, opts->guid, gl);
        c->mock_msgs[i].guid[gl] = '\0';
    }
    if (opts->reply_to_guid) {
        size_t rl = strlen(opts->reply_to_guid);
        if (rl > 95)
            rl = 95;
        memcpy(c->mock_msgs[i].reply_to_guid, opts->reply_to_guid, rl);
        c->mock_msgs[i].reply_to_guid[rl] = '\0';
    }
    return HU_OK;
}
#endif

void hu_discord_set_persona(hu_channel_t *ch, const struct hu_persona *persona) {
    if (!ch || !ch->ctx)
        return;
    hu_discord_ctx_t *c = (hu_discord_ctx_t *)ch->ctx;
    c->persona = persona;
}

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
