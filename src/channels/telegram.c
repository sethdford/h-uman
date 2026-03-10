/*
 * Telegram channel — Bot API with long-polling (getUpdates).
 * Send/receive, media, commands, smart splitting, typing, policy.
 */
#include "human/channels/telegram.h"
#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define TELEGRAM_API_BASE                "https://api.telegram.org/bot"
#define TELEGRAM_MAX_MSG                 4096
#define TELEGRAM_MARKDOWN_V2_SPECIAL     "_*[]()~>#+-=|{}.!"
#define TELEGRAM_MARKDOWN_V2_SPECIAL_LEN 18

/* Bot command help text — used for /help or agent slash dispatch */
static const char TELEGRAM_COMMANDS_HELP[] = "/start - Start a conversation\n"
                                             "/help - Show available commands\n"
                                             "/status - Show model and stats\n"
                                             "/model - Switch model\n"
                                             "/think - Set thinking level\n"
                                             "/verbose - Set verbose level\n"
                                             "/tts - Set TTS mode\n"
                                             "/memory - Memory tools and diagnostics\n"
                                             "/stop - Stop active background task\n"
                                             "/restart - Restart current session\n"
                                             "/compact - Compact context now";

typedef struct hu_telegram_ctx {
    hu_allocator_t *alloc;
    char *token;
    size_t token_len;
    int64_t last_update_id;
    bool running;
    /* Policy: NULL or count 0 = allow all; otherwise check allowlist */
    const char *const *allow_from;
    size_t allow_from_count;
    /* Streaming: message under edit, accumulated text */
    int64_t stream_message_id;
    char *stream_text;
    size_t stream_text_len;
    size_t stream_text_cap;
#if HU_IS_TEST
    char last_message[4096];
    size_t last_message_len;
    struct {
        char session_key[128];
        char content[4096];
    } mock_msgs[8];
    size_t mock_count;
#endif
} hu_telegram_ctx_t;

/* ─── Helpers ───────────────────────────────────────────────────────────── */

#if !HU_IS_TEST
static int build_api_url(char *buf, size_t cap, const char *token, size_t token_len,
                         const char *method) {
    return snprintf(buf, cap, "%s%.*s/%s", TELEGRAM_API_BASE, (int)token_len, token, method);
}
#endif

/* Escape text for Telegram MarkdownV2: _*[]()~>#+-=|{}.! */
char *hu_telegram_escape_markdown_v2(hu_allocator_t *alloc, const char *text, size_t text_len,
                                     size_t *out_len) {
    if (!alloc || !out_len)
        return NULL;
    if (!text || text_len == 0) {
        *out_len = 0;
        char *z = (char *)alloc->alloc(alloc->ctx, 1);
        if (z)
            z[0] = '\0';
        return z;
    }
    size_t cap = text_len * 2 + 1;
    char *out = (char *)alloc->alloc(alloc->ctx, cap);
    if (!out)
        return NULL;
    size_t w = 0;
    for (size_t i = 0; i < text_len && w + 2 < cap; i++) {
        char c = text[i];
        if (strchr(TELEGRAM_MARKDOWN_V2_SPECIAL, c)) {
            out[w++] = '\\';
        }
        out[w++] = c;
    }
    out[w] = '\0';
    *out_len = w;
    /* Shrink to exact size for correct free() with tracking allocator */
    if (w + 1 < cap && alloc->realloc) {
        char *s = (char *)alloc->realloc(alloc->ctx, out, cap, w + 1);
        if (s)
            out = s;
    }
    return out;
}

#if !HU_IS_TEST
/* Smart split: prefer newline, then space; max TELEGRAM_MAX_MSG per chunk */
static size_t smart_split_next(const char *buf, size_t len, size_t cursor, size_t *out_end) {
    if (cursor >= len) {
        *out_end = cursor;
        return 0;
    }
    size_t max_chunk = len - cursor;
    if (max_chunk > TELEGRAM_MAX_MSG)
        max_chunk = TELEGRAM_MAX_MSG;
    size_t search_end = cursor + max_chunk;
    if (search_end > len)
        search_end = len;
    size_t half = cursor + max_chunk / 2;
    size_t split_at = search_end;
    /* Search backwards for newline in second half */
    for (size_t i = search_end; i > cursor; i--) {
        if (buf[i - 1] == '\n' && (i - 1) >= half) {
            split_at = i;
            goto done;
        }
    }
    /* Else last space */
    for (size_t i = search_end; i > cursor; i--) {
        if (buf[i - 1] == ' ') {
            split_at = i;
            goto done;
        }
    }
done:
    *out_end = split_at;
    return split_at - cursor;
}
#endif

#if !HU_IS_TEST
/* Check if user is allowed: allow_from NULL/empty = allow all; "*" = allow all */
static bool is_user_allowed(hu_telegram_ctx_t *c, const char *username, size_t username_len,
                            const char *user_id, size_t user_id_len) {
    if (!c->allow_from || c->allow_from_count == 0)
        return true;
    for (size_t i = 0; i < c->allow_from_count; i++) {
        const char *a = c->allow_from[i];
        if (!a)
            continue;
        if (strcmp(a, "*") == 0)
            return true;
        size_t alen = strlen(a);
        if (a[0] == '@' && alen > 1) {
            a++;
            alen--;
        }
        if (username_len > 0 && alen == username_len && strncasecmp(a, username, username_len) == 0)
            return true;
        if (user_id_len > 0 && alen == user_id_len && memcmp(a, user_id, user_id_len) == 0)
            return true;
    }
    return false;
}
#endif

#if !HU_IS_TEST
/* ─── JSON body builders ───────────────────────────────────────────────── */

static hu_error_t build_send_body(hu_allocator_t *alloc, const char *chat_id, size_t chat_id_len,
                                  const char *text, size_t text_len, char **out, size_t *out_len) {
    hu_json_buf_t jbuf;
    hu_error_t err = hu_json_buf_init(&jbuf, alloc);
    if (err)
        return err;

    err = hu_json_buf_append_raw(&jbuf, "{", 1);
    if (err)
        goto fail;
    err = hu_json_append_key_value(&jbuf, "chat_id", 8, chat_id, chat_id_len);
    if (err)
        goto fail;
    err = hu_json_buf_append_raw(&jbuf, ",", 1);
    if (err)
        goto fail;
    err = hu_json_append_key_value(&jbuf, "text", 4, text, text_len);
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

static hu_error_t build_get_updates_body(hu_allocator_t *alloc, int64_t offset,
                                         unsigned timeout_secs, char **out, size_t *out_len) {
    hu_json_buf_t jbuf;
    hu_error_t err = hu_json_buf_init(&jbuf, alloc);
    if (err)
        return err;
    char num[128];
    int n = snprintf(num, sizeof(num),
                     "{\"offset\":%lld,\"timeout\":%u,"
                     "\"allowed_updates\":[\"message\"]}",
                     (long long)offset, timeout_secs);
    if (n < 0 || (size_t)n >= sizeof(num)) {
        err = HU_ERR_INTERNAL;
        goto fail;
    }
    err = hu_json_buf_append_raw(&jbuf, num, (size_t)n);
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

#if !HU_IS_TEST
/* ─── Typing indicator ─────────────────────────────────────────────────── */

static void send_typing_action(hu_telegram_ctx_t *c, const char *chat_id, size_t chat_id_len) {
#if HU_IS_TEST
    (void)c;
    (void)chat_id;
    (void)chat_id_len;
    return;
#else
    if (!c->token || chat_id_len == 0)
        return;
    char url_buf[512];
    int n = build_api_url(url_buf, sizeof(url_buf), c->token, c->token_len, "sendChatAction");
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return;
    char body_buf[256];
    n = snprintf(body_buf, sizeof(body_buf), "{\"chat_id\":%.*s,\"action\":\"typing\"}",
                 (int)chat_id_len, chat_id);
    if (n < 0 || (size_t)n >= sizeof(body_buf))
        return;
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_post_json(c->alloc, url_buf, NULL, body_buf, (size_t)n, &resp);
    if (resp.owned && resp.body)
        hu_http_response_free(c->alloc, &resp);
    (void)err;
#endif
}
#endif

/* ─── Media send via curl ───────────────────────────────────────────────── */

typedef enum { TG_MEDIA_PHOTO, TG_MEDIA_DOCUMENT, TG_MEDIA_AUDIO, TG_MEDIA_VIDEO } tg_media_kind_t;

#if !HU_IS_TEST
static const char *media_method(tg_media_kind_t k) {
    switch (k) {
    case TG_MEDIA_PHOTO:
        return "sendPhoto";
    case TG_MEDIA_DOCUMENT:
        return "sendDocument";
    case TG_MEDIA_AUDIO:
        return "sendAudio";
    case TG_MEDIA_VIDEO:
        return "sendVideo";
    }
    return "sendDocument";
}

static const char *media_field(tg_media_kind_t k) {
    switch (k) {
    case TG_MEDIA_PHOTO:
        return "photo";
    case TG_MEDIA_DOCUMENT:
        return "document";
    case TG_MEDIA_AUDIO:
        return "audio";
    case TG_MEDIA_VIDEO:
        return "video";
    }
    return "document";
}

static hu_error_t send_media_curl(hu_telegram_ctx_t *c, const char *chat_id, size_t chat_id_len,
                                  tg_media_kind_t kind, const char *media_path,
                                  size_t media_path_len, const char *caption, size_t caption_len) {
#if HU_IS_TEST
    (void)c;
    (void)chat_id;
    (void)chat_id_len;
    (void)kind;
    (void)media_path;
    (void)media_path_len;
    (void)caption;
    (void)caption_len;
    return HU_OK;
#else
    if (!c->token)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    char url_buf[512];
    int n = build_api_url(url_buf, sizeof(url_buf), c->token, c->token_len, media_method(kind));
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return HU_ERR_INTERNAL;
    const char *field = media_field(kind);
    char chat_arg[160];
    n = snprintf(chat_arg, sizeof(chat_arg), "chat_id=%.*s", (int)chat_id_len, chat_id);
    if (n < 0 || (size_t)n >= sizeof(chat_arg))
        return HU_ERR_INTERNAL;
    char file_arg[1100];
    int is_url = (media_path_len >= 7 && memcmp(media_path, "http://", 7) == 0) ||
                 (media_path_len >= 8 && memcmp(media_path, "https://", 8) == 0);
    if (is_url)
        n = snprintf(file_arg, sizeof(file_arg), "%s=%.*s", field, (int)media_path_len, media_path);
    else
        n = snprintf(file_arg, sizeof(file_arg), "%s=@%.*s", field, (int)media_path_len,
                     media_path);
    if (n < 0 || (size_t)n >= (int)sizeof(file_arg))
        return HU_ERR_INTERNAL;
    const char *argv[16];
    int argc = 0;
    argv[argc++] = "curl";
    argv[argc++] = "-s";
    argv[argc++] = "-m";
    argv[argc++] = "120";
    argv[argc++] = "-F";
    argv[argc++] = chat_arg;
    argv[argc++] = "-F";
    argv[argc++] = file_arg;
    if (caption && caption_len > 0) {
        char cap_arg[1030];
        n = snprintf(cap_arg, sizeof(cap_arg), "caption=%.*s",
                     (int)(caption_len > 1000 ? 1000 : caption_len), caption);
        if (n >= 0 && (size_t)n < sizeof(cap_arg)) {
            argv[argc++] = "-F";
            argv[argc++] = cap_arg;
        }
    }
    argv[argc++] = url_buf;
    argv[argc] = NULL;
    hu_run_result_t run = {0};
    hu_error_t err = hu_process_run(c->alloc, argv, NULL, 1024 * 1024, &run);
    if (err != HU_OK)
        return HU_ERR_CHANNEL_SEND;
    bool ok = run.stdout_buf && (strstr(run.stdout_buf, "\"ok\":true") != NULL);
    hu_run_result_free(c->alloc, &run);
    return ok ? HU_OK : HU_ERR_CHANNEL_SEND;
#endif
}

/* Infer media kind from extension or URL */
static tg_media_kind_t infer_media_kind(const char *path, size_t len) {
    const char *dot = NULL;
    for (size_t i = len; i > 0; i--) {
        if (path[i - 1] == '.') {
            dot = path + i;
            break;
        }
        if (path[i - 1] == '/' || path[i - 1] == '?' || path[i - 1] == '#')
            break;
    }
    if (!dot || dot >= path + len)
        return TG_MEDIA_DOCUMENT;
    size_t ext_len = (path + len) - dot;
    if (ext_len >= 3) {
        char ext[8];
        size_t copy = ext_len > 7 ? 7 : ext_len;
        memcpy(ext, dot, copy);
        ext[copy] = '\0';
        for (size_t j = 0; j < copy; j++)
            if (ext[j] >= 'A' && ext[j] <= 'Z')
                ext[j] += 32;
        if (memcmp(ext, "png", 3) == 0 || memcmp(ext, "jpg", 3) == 0 ||
            memcmp(ext, "jpeg", 4) == 0 || memcmp(ext, "gif", 3) == 0 ||
            memcmp(ext, "webp", 4) == 0)
            return TG_MEDIA_PHOTO;
        if (memcmp(ext, "mp4", 3) == 0 || memcmp(ext, "mov", 3) == 0 || memcmp(ext, "webm", 4) == 0)
            return TG_MEDIA_VIDEO;
        if (memcmp(ext, "mp3", 3) == 0 || memcmp(ext, "m4a", 3) == 0 ||
            memcmp(ext, "wav", 3) == 0 || memcmp(ext, "ogg", 3) == 0)
            return TG_MEDIA_AUDIO;
    }
    return TG_MEDIA_DOCUMENT;
}
#endif

#if !HU_IS_TEST
static hu_error_t build_edit_body(hu_allocator_t *alloc, const char *chat_id, size_t chat_id_len,
                                  int64_t message_id, const char *text, size_t text_len, char **out,
                                  size_t *out_len) {
    hu_json_buf_t jbuf;
    hu_error_t err = hu_json_buf_init(&jbuf, alloc);
    if (err)
        return err;
    err = hu_json_buf_append_raw(&jbuf, "{", 1);
    if (err)
        goto fail;
    err = hu_json_append_key_value(&jbuf, "chat_id", 8, chat_id, chat_id_len);
    if (err)
        goto fail;
    err = hu_json_buf_append_raw(&jbuf, ",", 1);
    if (err)
        goto fail;
    err = hu_json_append_key_int(&jbuf, "message_id", 10, (long long)message_id);
    if (err)
        goto fail;
    err = hu_json_buf_append_raw(&jbuf, ",", 1);
    if (err)
        goto fail;
    err = hu_json_append_key_value(&jbuf, "text", 4, text, text_len);
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

static hu_error_t stream_append(hu_telegram_ctx_t *c, const char *delta, size_t delta_len) {
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

static void stream_clear(hu_telegram_ctx_t *c) {
    if (c->stream_text) {
        c->alloc->free(c->alloc->ctx, c->stream_text, c->stream_text_cap + 1);
        c->stream_text = NULL;
    }
    c->stream_text_len = 0;
    c->stream_text_cap = 0;
    c->stream_message_id = 0;
}
#endif

static hu_error_t telegram_send_event(void *ctx, const char *target, size_t target_len,
                                      const char *message, size_t message_len,
                                      const char *const *media, size_t media_count,
                                      hu_outbound_stage_t stage) {
    (void)media;
    (void)media_count;
    hu_telegram_ctx_t *c = (hu_telegram_ctx_t *)ctx;
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
    if (stage == HU_OUTBOUND_STAGE_CHUNK) {
        if (message_len > 0 && stream_append(c, message, message_len) != HU_OK)
            return HU_ERR_OUT_OF_MEMORY;
        const char *text = c->stream_text ? c->stream_text : "";
        size_t text_len = c->stream_text ? c->stream_text_len : 0;
        if (c->stream_message_id == 0) {
            char url_buf[512];
            int n = build_api_url(url_buf, sizeof(url_buf), c->token, c->token_len, "sendMessage");
            if (n < 0 || (size_t)n >= sizeof(url_buf))
                return HU_ERR_INTERNAL;
            char *body = NULL;
            size_t body_len = 0;
            hu_error_t err =
                build_send_body(c->alloc, target, target_len, text, text_len, &body, &body_len);
            if (err)
                return err;
            hu_http_response_t resp = {0};
            err = hu_http_post_json(c->alloc, url_buf, NULL, body, body_len, &resp);
            if (body)
                c->alloc->free(c->alloc->ctx, body, body_len + 1);
            if (err != HU_OK) {
                if (resp.owned && resp.body)
                    hu_http_response_free(c->alloc, &resp);
                return HU_ERR_CHANNEL_SEND;
            }
            if (resp.status_code == 200 && resp.body) {
                hu_json_value_t *parsed = NULL;
                if (hu_json_parse(c->alloc, resp.body, resp.body_len, &parsed) == HU_OK && parsed) {
                    hu_json_value_t *result = hu_json_object_get(parsed, "result");
                    if (result && result->type == HU_JSON_OBJECT) {
                        double mid = hu_json_get_number(result, "message_id", 0);
                        c->stream_message_id = (int64_t)mid;
                    }
                    hu_json_free(c->alloc, parsed);
                }
            }
            if (resp.owned && resp.body)
                hu_http_response_free(c->alloc, &resp);
        } else {
            char url_buf[512];
            int n =
                build_api_url(url_buf, sizeof(url_buf), c->token, c->token_len, "editMessageText");
            if (n < 0 || (size_t)n >= sizeof(url_buf))
                return HU_ERR_INTERNAL;
            char *body = NULL;
            size_t body_len = 0;
            hu_error_t err = build_edit_body(c->alloc, target, target_len, c->stream_message_id,
                                             text, text_len, &body, &body_len);
            if (err)
                return err;
            hu_http_response_t resp = {0};
            err = hu_http_post_json(c->alloc, url_buf, NULL, body, body_len, &resp);
            if (body)
                c->alloc->free(c->alloc->ctx, body, body_len + 1);
            if (resp.owned && resp.body)
                hu_http_response_free(c->alloc, &resp);
            if (err != HU_OK || (resp.status_code != 200 && resp.status_code != 0))
                return HU_ERR_CHANNEL_SEND;
        }
    } else {
        /* FINAL */
        const char *text = message_len > 0 ? message : (c->stream_text ? c->stream_text : "");
        size_t text_len = message_len > 0 ? message_len : (c->stream_text ? c->stream_text_len : 0);
        if (c->stream_message_id != 0) {
            char url_buf[512];
            int n =
                build_api_url(url_buf, sizeof(url_buf), c->token, c->token_len, "editMessageText");
            if (n < 0 || (size_t)n >= sizeof(url_buf)) {
                stream_clear(c);
                return HU_ERR_INTERNAL;
            }
            char *body = NULL;
            size_t body_len = 0;
            hu_error_t err = build_edit_body(c->alloc, target, target_len, c->stream_message_id,
                                             text, text_len, &body, &body_len);
            if (err) {
                stream_clear(c);
                return err;
            }
            hu_http_response_t resp = {0};
            err = hu_http_post_json(c->alloc, url_buf, NULL, body, body_len, &resp);
            if (body)
                c->alloc->free(c->alloc->ctx, body, body_len + 1);
            stream_clear(c);
            if (resp.owned && resp.body)
                hu_http_response_free(c->alloc, &resp);
            if (err != HU_OK || (resp.status_code != 200 && resp.status_code != 0))
                return HU_ERR_CHANNEL_SEND;
        } else {
            char url_buf[512];
            int n = build_api_url(url_buf, sizeof(url_buf), c->token, c->token_len, "sendMessage");
            if (n < 0 || (size_t)n >= sizeof(url_buf)) {
                stream_clear(c);
                return HU_ERR_INTERNAL;
            }
            char *body = NULL;
            size_t body_len = 0;
            hu_error_t err =
                build_send_body(c->alloc, target, target_len, text, text_len, &body, &body_len);
            if (err) {
                stream_clear(c);
                return err;
            }
            hu_http_response_t resp = {0};
            err = hu_http_post_json(c->alloc, url_buf, NULL, body, body_len, &resp);
            if (body)
                c->alloc->free(c->alloc->ctx, body, body_len + 1);
            stream_clear(c);
            if (resp.owned && resp.body)
                hu_http_response_free(c->alloc, &resp);
            if (err != HU_OK || resp.status_code != 200)
                return HU_ERR_CHANNEL_SEND;
        }
    }
    return HU_OK;
#endif
}

/* ─── Vtable: start, stop, send ─────────────────────────────────────────── */

static hu_error_t telegram_start(void *ctx) {
    hu_telegram_ctx_t *c = (hu_telegram_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
    return HU_OK;
}

static void telegram_stop(void *ctx) {
    hu_telegram_ctx_t *c = (hu_telegram_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static hu_error_t telegram_send(void *ctx, const char *target, size_t target_len,
                                const char *message, size_t message_len, const char *const *media,
                                size_t media_count) {
    hu_telegram_ctx_t *c = (hu_telegram_ctx_t *)ctx;
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
    /* Typing indicator (best-effort) */
    send_typing_action(c, target, target_len);

    char url_buf[512];
    int n = build_api_url(url_buf, sizeof(url_buf), c->token, c->token_len, "sendMessage");
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return HU_ERR_INTERNAL;

    /* Smart split long messages */
    size_t offset = 0;
    while (offset < message_len) {
        size_t chunk_len;
        size_t end;
        chunk_len = smart_split_next(message, message_len, offset, &end);
        if (chunk_len == 0)
            break;

        char *body = NULL;
        size_t body_len = 0;
        hu_error_t err = build_send_body(c->alloc, target, target_len, message + offset, chunk_len,
                                         &body, &body_len);
        if (err)
            return err;

        hu_http_response_t resp = {0};
        err = hu_http_post_json(c->alloc, url_buf, NULL, body, body_len, &resp);
        if (body)
            c->alloc->free(c->alloc->ctx, body, body_len + 1);
        if (err) {
            if (resp.owned && resp.body)
                hu_http_response_free(c->alloc, &resp);
            return HU_ERR_CHANNEL_SEND;
        }
        if (resp.owned && resp.body)
            hu_http_response_free(c->alloc, &resp);
        if (resp.status_code != 200)
            return HU_ERR_CHANNEL_SEND;

        offset = end;
    }

    /* Send media attachments */
    for (size_t i = 0; i < media_count && media; i++) {
        const char *m = media[i];
        if (!m)
            continue;
        size_t mlen = strlen(m);
        tg_media_kind_t kind = infer_media_kind(m, mlen);
        hu_error_t err = send_media_curl(c, target, target_len, kind, m, mlen, NULL, 0);
        (void)err;
    }
    return HU_OK;
#endif
}

static hu_error_t telegram_start_typing(void *ctx, const char *recipient, size_t recipient_len) {
    hu_telegram_ctx_t *c = (hu_telegram_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    (void)recipient;
    (void)recipient_len;
    return HU_OK;
#else
    send_typing_action(c, recipient, recipient_len);
    return HU_OK;
#endif
}

static hu_error_t telegram_stop_typing(void *ctx, const char *recipient, size_t recipient_len) {
    (void)ctx;
    (void)recipient;
    (void)recipient_len;
    /* Telegram has no stop-typing API; typing auto-expires */
    return HU_OK;
}

static const char *telegram_name(void *ctx) {
    (void)ctx;
    return "telegram";
}

static bool telegram_health_check(void *ctx) {
#if HU_IS_TEST
    (void)ctx;
    return true;
#else
    hu_telegram_ctx_t *c = (hu_telegram_ctx_t *)ctx;
    if (!c || !c->token)
        return false;
    char url_buf[512];
    int n = build_api_url(url_buf, sizeof(url_buf), c->token, c->token_len, "getMe");
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return false;
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(c->alloc, url_buf, NULL, &resp);
    bool ok = (err == HU_OK && resp.status_code == 200 && resp.body &&
               strstr(resp.body, "\"ok\":true") != NULL);
    if (resp.owned && resp.body)
        hu_http_response_free(c->alloc, &resp);
    return ok;
#endif
}

static const hu_channel_vtable_t telegram_vtable = {
    .start = telegram_start,
    .stop = telegram_stop,
    .send = telegram_send,
    .name = telegram_name,
    .health_check = telegram_health_check,
    .send_event = telegram_send_event,
    .start_typing = telegram_start_typing,
    .stop_typing = telegram_stop_typing,
};

/* ─── Public API ─────────────────────────────────────────────────────────── */

hu_error_t hu_telegram_create_ex(hu_allocator_t *alloc, const char *token, size_t token_len,
                                 const char *const *allow_from, size_t allow_from_count,
                                 hu_channel_t *out);

hu_error_t hu_telegram_create(hu_allocator_t *alloc, const char *token, size_t token_len,
                              hu_channel_t *out) {
    return hu_telegram_create_ex(alloc, token, token_len, NULL, 0, out);
}

hu_error_t hu_telegram_create_ex(hu_allocator_t *alloc, const char *token, size_t token_len,
                                 const char *const *allow_from, size_t allow_from_count,
                                 hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_telegram_ctx_t *c = (hu_telegram_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    c->last_update_id = 0;
    c->allow_from = allow_from;
    c->allow_from_count = allow_from_count;
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
    out->ctx = c;
    out->vtable = &telegram_vtable;
    return HU_OK;
}

void hu_telegram_destroy(hu_channel_t *ch) {
    if (ch && ch->ctx) {
        hu_telegram_ctx_t *c = (hu_telegram_ctx_t *)ch->ctx;
        hu_allocator_t *a = c->alloc;
        if (c->stream_text && a)
            a->free(a->ctx, c->stream_text, c->stream_text_cap + 1);
        if (c->token)
            a->free(a->ctx, c->token, c->token_len + 1);
        a->free(a->ctx, c, sizeof(*c));
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

void hu_telegram_set_allowlist(hu_channel_t *ch, const char *const *allow_from,
                               size_t allow_from_count) {
    if (!ch || !ch->ctx)
        return;
    hu_telegram_ctx_t *c = (hu_telegram_ctx_t *)ch->ctx;
    c->allow_from = allow_from;
    c->allow_from_count = allow_from_count;
}

const char *hu_telegram_commands_help(void) {
    return TELEGRAM_COMMANDS_HELP;
}

/* ─── Long-polling (getUpdates) for channel_loop ─────────────────────────── */

hu_error_t hu_telegram_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                            size_t max_msgs, size_t *out_count) {
    hu_telegram_ctx_t *c = (hu_telegram_ctx_t *)channel_ctx;
    if (!c || !alloc || !msgs || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;

#if HU_IS_TEST
    if (c->mock_count > 0) {
        (void)alloc;
        size_t n = c->mock_count < max_msgs ? c->mock_count : max_msgs;
        for (size_t i = 0; i < n; i++) {
            memcpy(msgs[i].session_key, c->mock_msgs[i].session_key, 128);
            memcpy(msgs[i].content, c->mock_msgs[i].content, 4096);
        }
        *out_count = n;
        c->mock_count = 0;
        return HU_OK;
    }
    return HU_OK;
#else
    if (!c->token || c->token_len == 0)
        return HU_OK;

    char url_buf[512];
    int n = build_api_url(url_buf, sizeof(url_buf), c->token, c->token_len, "getUpdates");
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return HU_ERR_INTERNAL;

    char *body = NULL;
    size_t body_len = 0;
    hu_error_t err = build_get_updates_body(alloc, c->last_update_id, 25, &body, &body_len);
    if (err != HU_OK)
        return err;

    hu_http_response_t resp = {0};
    err = hu_http_post_json(alloc, url_buf, NULL, body, body_len, &resp);
    alloc->free(alloc->ctx, body, body_len + 1);
    if (err != HU_OK) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return err;
    }
    if (!resp.body || resp.body_len == 0) {
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

    hu_json_value_t *result = hu_json_object_get(parsed, "result");
    if (!result || result->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, parsed);
        return HU_OK;
    }

    size_t cnt = 0;
    for (size_t i = 0; i < result->data.array.len && cnt < max_msgs; i++) {
        hu_json_value_t *up = result->data.array.items[i];
        if (!up || up->type != HU_JSON_OBJECT)
            continue;

        /* Advance offset */
        hu_json_value_t *uid = hu_json_object_get(up, "update_id");
        if (uid && uid->type == HU_JSON_NUMBER) {
            c->last_update_id = (int64_t)uid->data.number + 1;
        }

        hu_json_value_t *msg_obj = hu_json_object_get(up, "message");
        if (!msg_obj || msg_obj->type != HU_JSON_OBJECT)
            continue;

        hu_json_value_t *from = hu_json_object_get(msg_obj, "from");
        if (!from || from->type != HU_JSON_OBJECT)
            continue;

        const char *username = hu_json_get_string(from, "username");
        size_t username_len = username ? strlen(username) : 0;
        char user_id_buf[32];
        double id_num = hu_json_get_number(from, "id", 0);
        int uidn = snprintf(user_id_buf, sizeof(user_id_buf), "%.0f", id_num);
        size_t user_id_len = (uidn > 0 && (size_t)uidn < sizeof(user_id_buf)) ? (size_t)uidn : 0;

        hu_json_value_t *chat = hu_json_object_get(msg_obj, "chat");
        if (!chat || chat->type != HU_JSON_OBJECT)
            continue;
        double chat_id_num = hu_json_get_number(chat, "id", 0);
        char chat_id_buf[32];
        int cn = snprintf(chat_id_buf, sizeof(chat_id_buf), "%.0f", chat_id_num);
        if (cn < 0 || (size_t)cn >= sizeof(chat_id_buf))
            continue;
        size_t chat_id_len = (size_t)cn;

        /* Policy filter */
        if (!is_user_allowed(c, username ? username : "", username_len,
                             user_id_len > 0 ? user_id_buf : "", user_id_len))
            continue;

        /* Extract text or caption; media-only uses caption or "" */
        const char *text = hu_json_get_string(msg_obj, "text");
        if (!text)
            text = hu_json_get_string(msg_obj, "caption");
        if (!text)
            text = "";
        size_t text_len = strlen(text);

        /* Photo/document: getFile to build download URL, then [IMAGE:url] or [FILE:url] */
        const char *content = text;
        size_t content_len = text_len;
        char *built = NULL;
        const char *file_id = NULL;
        bool is_photo = false;
        hu_json_value_t *photo = hu_json_object_get(msg_obj, "photo");
        if (photo && photo->type == HU_JSON_ARRAY && photo->data.array.len > 0) {
            hu_json_value_t *last_photo = photo->data.array.items[photo->data.array.len - 1];
            file_id = hu_json_get_string(last_photo, "file_id");
            is_photo = true;
        }
        if (!file_id) {
            hu_json_value_t *doc = hu_json_object_get(msg_obj, "document");
            if (doc && doc->type == HU_JSON_OBJECT)
                file_id = hu_json_get_string(doc, "file_id");
        }
        if (file_id) {
            char getfile_url[512];
            int gu =
                build_api_url(getfile_url, sizeof(getfile_url), c->token, c->token_len, "getFile");
            if (gu > 0 && (size_t)gu < sizeof(getfile_url)) {
                char body_buf[256];
                size_t fid_len = strlen(file_id);
                hu_json_buf_t gfj;
                int gb = 0;
                if (hu_json_buf_init(&gfj, alloc) == HU_OK) {
                    if (hu_json_buf_append_raw(&gfj, "{", 1) == HU_OK &&
                        hu_json_append_key_value(&gfj, "file_id", 7, file_id, fid_len) == HU_OK &&
                        hu_json_buf_append_raw(&gfj, "}", 1) == HU_OK) {
                        if (gfj.len + 1 <= sizeof(body_buf)) {
                            memcpy(body_buf, gfj.ptr, gfj.len + 1);
                            gb = (int)gfj.len;
                        }
                    }
                    hu_json_buf_free(&gfj);
                }
                if (gb > 0) {
                    hu_http_response_t gresp = {0};
                    hu_error_t ger =
                        hu_http_post_json(alloc, getfile_url, NULL, body_buf, (size_t)gb, &gresp);
                    if (ger == HU_OK && gresp.body && gresp.body_len > 0) {
                        hu_json_value_t *fp_parsed = NULL;
                        if (hu_json_parse(alloc, gresp.body, gresp.body_len, &fp_parsed) == HU_OK &&
                            fp_parsed) {
                            hu_json_value_t *res = hu_json_object_get(fp_parsed, "result");
                            const char *file_path = res && res->type == HU_JSON_OBJECT
                                                        ? hu_json_get_string(res, "file_path")
                                                        : NULL;
                            if (file_path) {
                                char file_url_buf[768];
                                int ub = snprintf(file_url_buf, sizeof(file_url_buf),
                                                  "https://api.telegram.org/file/bot%.*s/%s",
                                                  (int)c->token_len, c->token, file_path);
                                if (ub > 0 && (size_t)ub < sizeof(file_url_buf)) {
                                    size_t prefix_len = is_photo ? 7 : 6;
                                    const char *prefix = is_photo ? "[IMAGE:" : "[FILE:";
                                    size_t need = prefix_len + (size_t)ub + 2 +
                                                  (text_len ? 1 + text_len : 0) + 1;
                                    built = (char *)alloc->alloc(alloc->ctx, need);
                                    if (built) {
                                        size_t pos = 0;
                                        memcpy(built + pos, prefix, prefix_len);
                                        pos += prefix_len;
                                        memcpy(built + pos, file_url_buf, (size_t)ub);
                                        pos += (size_t)ub;
                                        built[pos++] = ']';
                                        if (text_len > 0) {
                                            built[pos++] = ' ';
                                            size_t cp = text_len < 4000 ? text_len : 4000;
                                            memcpy(built + pos, text, cp);
                                            pos += cp;
                                        }
                                        built[pos] = '\0';
                                        content = built;
                                        content_len = pos;
                                    }
                                }
                            }
                            hu_json_free(alloc, fp_parsed);
                        }
                    }
                    if (gresp.owned && gresp.body)
                        hu_http_response_free(alloc, &gresp);
                }
            }
        }

        /* Fill session_key = chat_id, content = message */
        size_t sk_len = chat_id_len < 127 ? chat_id_len : 127;
        memcpy(msgs[cnt].session_key, chat_id_buf, sk_len);
        msgs[cnt].session_key[sk_len] = '\0';
        size_t ct_len = content_len < 4095 ? content_len : 4095;
        memcpy(msgs[cnt].content, content, ct_len);
        msgs[cnt].content[ct_len] = '\0';
        cnt++;
        if (built)
            alloc->free(alloc->ctx, built, strlen(built) + 1);
    }

    hu_json_free(alloc, parsed);
    *out_count = cnt;
    return HU_OK;
#endif
}

#if HU_IS_TEST
hu_error_t hu_telegram_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                        size_t session_key_len, const char *content,
                                        size_t content_len) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_telegram_ctx_t *c = (hu_telegram_ctx_t *)ch->ctx;
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

const char *hu_telegram_test_get_last_message(hu_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    hu_telegram_ctx_t *c = (hu_telegram_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif
