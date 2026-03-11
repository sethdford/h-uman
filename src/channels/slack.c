/*
 * Slack channel — chat.postMessage for outbound, auth.test for bot identity.
 * Supports thread replies (channel_id:thread_ts), typing, markdown→mrkdwn.
 */
#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLACK_API_BASE         "https://slack.com/api"
#define SLACK_AUTH_TEST        "/auth.test"
#define SLACK_CHAT_POST        "/chat.postMessage"
#define SLACK_CHAT_UPDATE      "/chat.update"
#define SLACK_ASSISTANT_STATUS "/assistant.threads.setStatus"

#define SLACK_CONVERSATIONS_HISTORY "/conversations.history"
#define SLACK_MAX_CHANNELS          16
#define SLACK_SESSION_KEY_MAX       127
#define SLACK_CONTENT_MAX           4095
#define SLACK_QUEUE_MAX             32

typedef struct hu_slack_queued_msg {
    char session_key[128];
    char content[4096];
} hu_slack_queued_msg_t;

typedef struct hu_slack_ctx {
    hu_allocator_t *alloc;
    char *token;
    size_t token_len;
    bool running;
    char *bot_user_id;
    char **channel_ids;
    size_t channel_ids_count;
    char **last_ts;
    /* Streaming: message under edit */
    char *stream_ts;
    char *stream_text;
    size_t stream_text_len;
    size_t stream_text_cap;
    /* Webhook inbound queue */
    hu_slack_queued_msg_t queue[SLACK_QUEUE_MAX];
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
} hu_slack_ctx_t;

/* Parse target: "channel_id:thread_ts" → channel_id and optional thread_ts. */
static void parse_target(const char *target, size_t target_len, const char **channel,
                         size_t *channel_len, const char **thread_ts_out,
                         size_t *thread_ts_len_out) {
    const char *colon = memchr(target, ':', target_len);
    if (colon && (size_t)(colon - target) < target_len) {
        *channel = target;
        *channel_len = (size_t)(colon - target);
        *thread_ts_out = colon + 1;
        *thread_ts_len_out = target_len - *channel_len - 1;
    } else {
        *channel = target;
        *channel_len = target_len;
        *thread_ts_out = NULL;
        *thread_ts_len_out = 0;
    }
}

/* Find needle in haystack. */
static const char *slack_memmem(const char *haystack, size_t hlen, const char *needle,
                                size_t nlen) {
    if (nlen == 0)
        return haystack;
    if (hlen < nlen)
        return NULL;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (memcmp(haystack + i, needle, nlen) == 0)
            return haystack + i;
    }
    return NULL;
}

/* Convert Markdown to Slack mrkdwn. */
char *hu_slack_markdown_to_mrkdwn(hu_allocator_t *alloc, const char *input, size_t input_len,
                                  size_t *out_len) {
    if (!alloc || !out_len)
        return NULL;
    if (!input || input_len == 0) {
        char *empty = (char *)alloc->alloc(alloc->ctx, 1);
        if (empty)
            empty[0] = '\0';
        *out_len = 0;
        return empty;
    }

    hu_json_buf_t buf;
    if (hu_json_buf_init(&buf, alloc) != HU_OK)
        return NULL;

    size_t i = 0;
    int line_start = 1;

    while (i < input_len) {
        /* Fenced code blocks ``` — preserve as-is */
        if (i + 3 <= input_len && memcmp(input + i, "```", 3) == 0) {
            if (hu_json_buf_append_raw(&buf, "```", 3) != HU_OK)
                goto fail;
            i += 3;
            while (i < input_len) {
                if (i + 3 <= input_len && memcmp(input + i, "```", 3) == 0) {
                    if (hu_json_buf_append_raw(&buf, "```", 3) != HU_OK)
                        goto fail;
                    i += 3;
                    break;
                }
                if (hu_json_buf_append_raw(&buf, input + i, 1) != HU_OK)
                    goto fail;
                i++;
            }
            line_start = 0;
            continue;
        }

        /* Headers at line start: # -> bold */
        if (line_start && i < input_len && input[i] == '#') {
            size_t hi = i;
            while (hi < input_len && input[hi] == '#')
                hi++;
            if (hi < input_len && input[hi] == ' ') {
                size_t end = hi + 1;
                while (end < input_len && input[end] != '\n')
                    end++;
                if (hu_json_buf_append_raw(&buf, "*", 1) != HU_OK)
                    goto fail;
                if (hu_json_buf_append_raw(&buf, input + hi + 1, end - hi - 1) != HU_OK)
                    goto fail;
                if (hu_json_buf_append_raw(&buf, "*", 1) != HU_OK)
                    goto fail;
                i = end;
                line_start = 0;
                continue;
            }
        }

        /* Bullets - -> bullet char */
        if (line_start && i + 1 < input_len && input[i] == '-' && input[i + 1] == ' ') {
            if (hu_json_buf_append_raw(&buf, "\xe2\x80\xa2 ", 4) != HU_OK)
                goto fail;
            i += 2;
            line_start = 0;
            continue;
        }

        /* Bold **text** -> *text* */
        if (i + 2 <= input_len && memcmp(input + i, "**", 2) == 0) {
            const char *close = slack_memmem(input + i + 2, input_len - i - 2, "**", 2);
            if (close) {
                size_t mid = (size_t)(close - (input + i + 2));
                if (hu_json_buf_append_raw(&buf, "*", 1) != HU_OK)
                    goto fail;
                if (hu_json_buf_append_raw(&buf, input + i + 2, mid) != HU_OK)
                    goto fail;
                if (hu_json_buf_append_raw(&buf, "*", 1) != HU_OK)
                    goto fail;
                i += 4 + mid;
                line_start = 0;
                continue;
            }
        }

        /* Strikethrough ~~text~~ -> ~text~ */
        if (i + 2 <= input_len && memcmp(input + i, "~~", 2) == 0) {
            const char *close = slack_memmem(input + i + 2, input_len - i - 2, "~~", 2);
            if (close) {
                size_t mid = (size_t)(close - (input + i + 2));
                if (hu_json_buf_append_raw(&buf, "~", 1) != HU_OK)
                    goto fail;
                if (hu_json_buf_append_raw(&buf, input + i + 2, mid) != HU_OK)
                    goto fail;
                if (hu_json_buf_append_raw(&buf, "~", 1) != HU_OK)
                    goto fail;
                i += 4 + mid;
                line_start = 0;
                continue;
            }
        }

        /* Inline code ` -> preserve */
        if (input[i] == '`') {
            if (hu_json_buf_append_raw(&buf, "`", 1) != HU_OK)
                goto fail;
            i++;
            while (i < input_len && input[i] != '`') {
                if (hu_json_buf_append_raw(&buf, input + i, 1) != HU_OK)
                    goto fail;
                i++;
            }
            if (i < input_len) {
                if (hu_json_buf_append_raw(&buf, "`", 1) != HU_OK)
                    goto fail;
                i++;
            }
            line_start = 0;
            continue;
        }

        /* Links [text](url) -> <url|text> */
        if (input[i] == '[') {
            const char *bracket_end = memchr(input + i + 1, ']', input_len - i - 1);
            if (bracket_end && (size_t)(bracket_end - input) + 1 < input_len &&
                input[(size_t)(bracket_end - input) + 1] == '(') {
                const char *paren_start = bracket_end + 2;
                const char *paren_end =
                    memchr(paren_start, ')', input_len - (size_t)(paren_start - input));
                if (paren_end) {
                    if (hu_json_buf_append_raw(&buf, "<", 1) != HU_OK)
                        goto fail;
                    if (hu_json_buf_append_raw(&buf, paren_start,
                                               (size_t)(paren_end - paren_start)) != HU_OK)
                        goto fail;
                    if (hu_json_buf_append_raw(&buf, "|", 1) != HU_OK)
                        goto fail;
                    if (hu_json_buf_append_raw(&buf, input + i + 1,
                                               (size_t)(bracket_end - input - 1)) != HU_OK)
                        goto fail;
                    if (hu_json_buf_append_raw(&buf, ">", 1) != HU_OK)
                        goto fail;
                    i = (size_t)(paren_end - input) + 1;
                    line_start = 0;
                    continue;
                }
            }
        }

        if (input[i] == '\n') {
            if (hu_json_buf_append_raw(&buf, "\n", 1) != HU_OK)
                goto fail;
            i++;
            line_start = 1;
            continue;
        }

        if (hu_json_buf_append_raw(&buf, input + i, 1) != HU_OK)
            goto fail;
        i++;
        line_start = 0;
    }

    *out_len = buf.len;
    char *result = (char *)alloc->alloc(alloc->ctx, buf.len + 1);
    if (!result)
        goto fail;
    memcpy(result, buf.ptr, buf.len + 1);
    hu_json_buf_free(&buf);
    return result;
fail:
    hu_json_buf_free(&buf);
    return NULL;
}

#if !HU_IS_TEST
/* Build chat.postMessage body with optional thread_ts. */
static hu_error_t build_chat_body(hu_allocator_t *alloc, const char *channel, size_t channel_len,
                                  const char *thread_ts, size_t thread_ts_len, const char *text,
                                  size_t text_len, char **out, size_t *out_len) {
    hu_json_buf_t jbuf;
    hu_error_t err = hu_json_buf_init(&jbuf, alloc);
    if (err)
        return err;

    err = hu_json_buf_append_raw(&jbuf, "{\"channel\":", 11);
    if (err)
        goto fail;
    err = hu_json_append_string(&jbuf, channel, channel_len);
    if (err)
        goto fail;
    err = hu_json_buf_append_raw(&jbuf, ",\"mrkdwn\":true,\"text\":", 22);
    if (err)
        goto fail;
    err = hu_json_append_string(&jbuf, text, text_len);
    if (err)
        goto fail;
    if (thread_ts && thread_ts_len > 0) {
        err = hu_json_buf_append_raw(&jbuf, ",\"thread_ts\":", 13);
        if (err)
            goto fail;
        err = hu_json_append_string(&jbuf, thread_ts, thread_ts_len);
        if (err)
            goto fail;
    }
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

static hu_error_t build_chat_update_body(hu_allocator_t *alloc, const char *channel,
                                         size_t channel_len, const char *ts, size_t ts_len,
                                         const char *text, size_t text_len, char **out,
                                         size_t *out_len) {
    hu_json_buf_t jbuf;
    hu_error_t err = hu_json_buf_init(&jbuf, alloc);
    if (err)
        return err;
    err = hu_json_buf_append_raw(&jbuf, "{\"channel\":", 11);
    if (err)
        goto fail;
    err = hu_json_append_string(&jbuf, channel, channel_len);
    if (err)
        goto fail;
    err = hu_json_buf_append_raw(&jbuf, ",\"ts\":", 6);
    if (err)
        goto fail;
    err = hu_json_append_string(&jbuf, ts, ts_len);
    if (err)
        goto fail;
    err = hu_json_buf_append_raw(&jbuf, ",\"text\":", 8);
    if (err)
        goto fail;
    err = hu_json_append_string(&jbuf, text, text_len);
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
#endif /* !HU_IS_TEST */

/* Call auth.test to fetch bot_user_id. */
static hu_error_t fetch_auth_test(hu_slack_ctx_t *c) {
#if HU_IS_TEST
    (void)c;
    return HU_OK;
#else
    char url_buf[256];
    int n = snprintf(url_buf, sizeof(url_buf), "%s%s", SLACK_API_BASE, SLACK_AUTH_TEST);
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return HU_ERR_INTERNAL;

    char auth_buf[512];
    n = snprintf(auth_buf, sizeof(auth_buf), "Authorization: Bearer %.*s", (int)c->token_len,
                 c->token);
    if (n <= 0 || (size_t)n >= sizeof(auth_buf))
        return HU_ERR_INTERNAL;

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(c->alloc, url_buf, auth_buf, &resp);
    if (err || !resp.body) {
        if (resp.owned && resp.body)
            hu_http_response_free(c->alloc, &resp);
        return err;
    }

    hu_json_value_t *parsed = NULL;
    err = hu_json_parse(c->alloc, resp.body, resp.body_len, &parsed);
    if (resp.owned && resp.body)
        hu_http_response_free(c->alloc, &resp);
    if (err || !parsed)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;

    bool ok = hu_json_get_bool(parsed, "ok", false);
    if (!ok) {
        hu_json_free(c->alloc, parsed);
        return HU_ERR_CHANNEL_NOT_CONFIGURED;
    }

    const char *uid = hu_json_get_string(parsed, "user_id");
    if (uid) {
        size_t uid_len = strlen(uid);
        if (c->bot_user_id)
            c->alloc->free(c->alloc->ctx, c->bot_user_id, strlen(c->bot_user_id) + 1);
        c->bot_user_id = hu_strndup(c->alloc, uid, uid_len);
    }
    hu_json_free(c->alloc, parsed);
    return HU_OK;
#endif
}

static hu_error_t slack_start(void *ctx) {
    hu_slack_ctx_t *c = (hu_slack_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
    return fetch_auth_test(c);
}

static void slack_stop(void *ctx) {
    hu_slack_ctx_t *c = (hu_slack_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static hu_error_t slack_send(void *ctx, const char *target, size_t target_len, const char *message,
                             size_t message_len, const char *const *media, size_t media_count) {
    (void)media;
    (void)media_count;
    hu_slack_ctx_t *c = (hu_slack_ctx_t *)ctx;
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
    const char *channel = NULL;
    size_t channel_len = 0;
    const char *thread_ts = NULL;
    size_t thread_ts_len = 0;
    parse_target(target, target_len, &channel, &channel_len, &thread_ts, &thread_ts_len);

    size_t conv_len = 0;
    char *mrkdwn = hu_slack_markdown_to_mrkdwn(c->alloc, message, message_len, &conv_len);
    const char *text = mrkdwn ? mrkdwn : message;
    size_t text_len = mrkdwn ? conv_len : message_len;

    char *body = NULL;
    size_t body_len = 0;
    hu_error_t err = build_chat_body(c->alloc, channel, channel_len, thread_ts, thread_ts_len, text,
                                     text_len, &body, &body_len);
    if (mrkdwn)
        c->alloc->free(c->alloc->ctx, mrkdwn, conv_len + 1);
    if (err)
        return err;

    char auth_buf[512];
    int n = snprintf(auth_buf, sizeof(auth_buf), "Authorization: Bearer %.*s", (int)c->token_len,
                     c->token);
    if (n <= 0 || (size_t)n >= sizeof(auth_buf)) {
        c->alloc->free(c->alloc->ctx, body, body_len + 1);
        return HU_ERR_INTERNAL;
    }

    char url_buf[256];
    snprintf(url_buf, sizeof(url_buf), "%s%s", SLACK_API_BASE, SLACK_CHAT_POST);

    hu_http_response_t resp = {0};
    err = hu_http_post_json(c->alloc, url_buf, auth_buf, body, body_len, &resp);
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

static hu_error_t slack_set_thread_status(hu_slack_ctx_t *c, const char *channel_id,
                                          size_t channel_id_len, const char *thread_ts,
                                          size_t thread_ts_len, const char *status,
                                          size_t status_len) {
#if HU_IS_TEST
    (void)c;
    (void)channel_id;
    (void)channel_id_len;
    (void)thread_ts;
    (void)thread_ts_len;
    (void)status;
    (void)status_len;
    return HU_OK;
#else
    if (!c->token || channel_id_len == 0 || thread_ts_len == 0)
        return HU_OK;

    hu_error_t post_err = HU_OK;
    hu_json_buf_t jbuf;
    if (hu_json_buf_init(&jbuf, c->alloc) != HU_OK)
        return HU_OK;
    if (hu_json_buf_append_raw(&jbuf, "{\"channel_id\":", 14) != HU_OK)
        goto out;
    if (hu_json_append_string(&jbuf, channel_id, channel_id_len) != HU_OK)
        goto out;
    if (hu_json_buf_append_raw(&jbuf, ",\"thread_ts\":", 13) != HU_OK)
        goto out;
    if (hu_json_append_string(&jbuf, thread_ts, thread_ts_len) != HU_OK)
        goto out;
    if (hu_json_buf_append_raw(&jbuf, ",\"status\":", 10) != HU_OK)
        goto out;
    if (hu_json_append_string(&jbuf, status, status_len) != HU_OK)
        goto out;
    if (hu_json_buf_append_raw(&jbuf, "}", 1) != HU_OK)
        goto out;

    char url_buf[256];
    snprintf(url_buf, sizeof(url_buf), "%s%s", SLACK_API_BASE, SLACK_ASSISTANT_STATUS);
    char auth_buf[512];
    snprintf(auth_buf, sizeof(auth_buf), "Authorization: Bearer %.*s", (int)c->token_len, c->token);

    hu_http_response_t resp = {0};
    post_err = hu_http_post_json(c->alloc, url_buf, auth_buf, jbuf.ptr, jbuf.len, &resp);
    if (resp.owned && resp.body)
        hu_http_response_free(c->alloc, &resp);
out:
    hu_json_buf_free(&jbuf);
    return (post_err != HU_OK) ? post_err : HU_OK;
#endif
}

static hu_error_t slack_start_typing(void *ctx, const char *recipient, size_t recipient_len) {
    hu_slack_ctx_t *c = (hu_slack_ctx_t *)ctx;
    if (!c || recipient_len == 0)
        return HU_OK;

    const char *channel = NULL;
    size_t channel_len = 0;
    const char *thread_ts = NULL;
    size_t thread_ts_len = 0;
    parse_target(recipient, recipient_len, &channel, &channel_len, &thread_ts, &thread_ts_len);
    if (!thread_ts || thread_ts_len == 0)
        return HU_OK;

    return slack_set_thread_status(c, channel, channel_len, thread_ts, thread_ts_len,
                                   "is typing...", 12);
}

static hu_error_t slack_stop_typing(void *ctx, const char *recipient, size_t recipient_len) {
    hu_slack_ctx_t *c = (hu_slack_ctx_t *)ctx;
    if (!c || recipient_len == 0)
        return HU_OK;

    const char *channel = NULL;
    size_t channel_len = 0;
    const char *thread_ts = NULL;
    size_t thread_ts_len = 0;
    parse_target(recipient, recipient_len, &channel, &channel_len, &thread_ts, &thread_ts_len);
    if (!thread_ts || thread_ts_len == 0)
        return HU_OK;

    return slack_set_thread_status(c, channel, channel_len, thread_ts, thread_ts_len, "", 0);
}

static const char *slack_name(void *ctx) {
    (void)ctx;
    return "slack";
}

static bool slack_health_check(void *ctx) {
    hu_slack_ctx_t *c = (hu_slack_ctx_t *)ctx;
#if HU_IS_TEST
    return c != NULL;
#else
    return c && c->running;
#endif
}

static void slack_stream_clear(hu_slack_ctx_t *c) {
    if (c->stream_ts) {
        c->alloc->free(c->alloc->ctx, c->stream_ts, strlen(c->stream_ts) + 1);
        c->stream_ts = NULL;
    }
    if (c->stream_text) {
        c->alloc->free(c->alloc->ctx, c->stream_text, c->stream_text_cap + 1);
        c->stream_text = NULL;
    }
    c->stream_text_len = 0;
    c->stream_text_cap = 0;
}

#if !HU_IS_TEST
static hu_error_t slack_stream_append(hu_slack_ctx_t *c, const char *delta, size_t delta_len) {
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
#endif

static hu_error_t slack_send_event(void *ctx, const char *target, size_t target_len,
                                   const char *message, size_t message_len,
                                   const char *const *media, size_t media_count,
                                   hu_outbound_stage_t stage) {
    (void)media;
    (void)media_count;
    hu_slack_ctx_t *c = (hu_slack_ctx_t *)ctx;
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
    const char *channel = NULL;
    size_t channel_len = 0;
    const char *thread_ts = NULL;
    size_t thread_ts_len = 0;
    parse_target(target, target_len, &channel, &channel_len, &thread_ts, &thread_ts_len);

    char auth_buf[512];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Authorization: Bearer %.*s", (int)c->token_len,
                      c->token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf))
        return HU_ERR_INTERNAL;

    if (stage == HU_OUTBOUND_STAGE_CHUNK) {
        if (message_len > 0 && slack_stream_append(c, message, message_len) != HU_OK)
            return HU_ERR_OUT_OF_MEMORY;
        const char *text = c->stream_text ? c->stream_text : "";
        size_t text_len = c->stream_text ? c->stream_text_len : 0;
        if (!c->stream_ts) {
            char *body = NULL;
            size_t body_len = 0;
            hu_error_t err = build_chat_body(c->alloc, channel, channel_len, thread_ts,
                                             thread_ts_len, text, text_len, &body, &body_len);
            if (err)
                return err;
            char url_buf[256];
            snprintf(url_buf, sizeof(url_buf), "%s%s", SLACK_API_BASE, SLACK_CHAT_POST);
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
                    const char *ts = hu_json_get_string(parsed, "ts");
                    if (ts) {
                        size_t ts_len = strlen(ts);
                        c->stream_ts = (char *)c->alloc->alloc(c->alloc->ctx, ts_len + 1);
                        if (c->stream_ts) {
                            memcpy(c->stream_ts, ts, ts_len + 1);
                        }
                    }
                    hu_json_free(c->alloc, parsed);
                }
            }
            if (resp.owned && resp.body)
                hu_http_response_free(c->alloc, &resp);
        } else {
            char *body = NULL;
            size_t body_len = 0;
            hu_error_t err =
                build_chat_update_body(c->alloc, channel, channel_len, c->stream_ts,
                                       strlen(c->stream_ts), text, text_len, &body, &body_len);
            if (err)
                return err;
            char url_buf[256];
            snprintf(url_buf, sizeof(url_buf), "%s%s", SLACK_API_BASE, SLACK_CHAT_UPDATE);
            hu_http_response_t resp = {0};
            err = hu_http_post_json(c->alloc, url_buf, auth_buf, body, body_len, &resp);
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
        if (message_len > 0) {
            size_t conv_len = 0;
            char *mrkdwn = hu_slack_markdown_to_mrkdwn(c->alloc, text, text_len, &conv_len);
            if (mrkdwn) {
                text = mrkdwn;
                text_len = conv_len;
            }
        }
        if (c->stream_ts) {
            char *body = NULL;
            size_t body_len = 0;
            hu_error_t err =
                build_chat_update_body(c->alloc, channel, channel_len, c->stream_ts,
                                       strlen(c->stream_ts), text, text_len, &body, &body_len);
            if (err) {
                slack_stream_clear(c);
                return err;
            }
            char url_buf[256];
            snprintf(url_buf, sizeof(url_buf), "%s%s", SLACK_API_BASE, SLACK_CHAT_UPDATE);
            hu_http_response_t resp = {0};
            err = hu_http_post_json(c->alloc, url_buf, auth_buf, body, body_len, &resp);
            if (body)
                c->alloc->free(c->alloc->ctx, body, body_len + 1);
            slack_stream_clear(c);
            if (resp.owned && resp.body)
                hu_http_response_free(c->alloc, &resp);
            if (err != HU_OK || resp.status_code < 200 || resp.status_code >= 300)
                return HU_ERR_CHANNEL_SEND;
        } else {
            char *body = NULL;
            size_t body_len = 0;
            hu_error_t err = build_chat_body(c->alloc, channel, channel_len, thread_ts,
                                             thread_ts_len, text, text_len, &body, &body_len);
            if (err) {
                slack_stream_clear(c);
                return err;
            }
            char url_buf[256];
            snprintf(url_buf, sizeof(url_buf), "%s%s", SLACK_API_BASE, SLACK_CHAT_POST);
            hu_http_response_t resp = {0};
            err = hu_http_post_json(c->alloc, url_buf, auth_buf, body, body_len, &resp);
            if (body)
                c->alloc->free(c->alloc->ctx, body, body_len + 1);
            slack_stream_clear(c);
            if (resp.owned && resp.body)
                hu_http_response_free(c->alloc, &resp);
            if (err != HU_OK || resp.status_code < 200 || resp.status_code >= 300)
                return HU_ERR_CHANNEL_SEND;
        }
    }
    return HU_OK;
#endif
}

static const hu_channel_vtable_t slack_vtable = {
    .start = slack_start,
    .stop = slack_stop,
    .send = slack_send,
    .name = slack_name,
    .health_check = slack_health_check,
    .send_event = slack_send_event,
    .start_typing = slack_start_typing,
    .stop_typing = slack_stop_typing,
};

/* ─── Webhook inbound queue ────────────────────────────────────────────── */

static void slack_queue_push(hu_slack_ctx_t *c, const char *from, size_t from_len, const char *body,
                             size_t body_len) {
    if (c->queue_count >= SLACK_QUEUE_MAX)
        return;
    hu_slack_queued_msg_t *slot = &c->queue[c->queue_tail];
    size_t sk = from_len < SLACK_SESSION_KEY_MAX ? from_len : SLACK_SESSION_KEY_MAX;
    memcpy(slot->session_key, from, sk);
    slot->session_key[sk] = '\0';
    size_t ct = body_len < SLACK_CONTENT_MAX ? body_len : SLACK_CONTENT_MAX;
    memcpy(slot->content, body, ct);
    slot->content[ct] = '\0';
    c->queue_tail = (c->queue_tail + 1) % SLACK_QUEUE_MAX;
    c->queue_count++;
}

hu_error_t hu_slack_on_webhook(void *channel_ctx, hu_allocator_t *alloc, const char *body,
                               size_t body_len) {
    hu_slack_ctx_t *c = (hu_slack_ctx_t *)channel_ctx;
    if (!c || !body || body_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    (void)alloc;
    slack_queue_push(c, "test-sender", 11, body, body_len);
    return HU_OK;
#else
    hu_json_value_t *parsed = NULL;
    hu_error_t err = hu_json_parse(alloc, body, body_len, &parsed);
    if (err != HU_OK || !parsed)
        return HU_OK;

    /*
     * Slack Events API payload:
     *   { "type": "event_callback", "event": { "type": "message",
     *     "channel": "C...", "user": "U...", "text": "...", "ts": "..." } }
     * URL verification challenge:
     *   { "type": "url_verification", "challenge": "..." }
     */
    const char *type = hu_json_get_string(parsed, "type");
    if (type && strcmp(type, "url_verification") == 0) {
        hu_json_free(alloc, parsed);
        return HU_OK;
    }

    hu_json_value_t *event = hu_json_object_get(parsed, "event");
    if (!event || event->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, parsed);
        return HU_OK;
    }

    const char *event_type = hu_json_get_string(event, "type");
    if (!event_type || strcmp(event_type, "message") != 0) {
        hu_json_free(alloc, parsed);
        return HU_OK;
    }

    const char *subtype = hu_json_get_string(event, "subtype");
    if (subtype) {
        hu_json_free(alloc, parsed);
        return HU_OK;
    }

    const char *user = hu_json_get_string(event, "user");
    if (!user) {
        hu_json_free(alloc, parsed);
        return HU_OK;
    }

    if (c->bot_user_id && strcmp(user, c->bot_user_id) == 0) {
        hu_json_free(alloc, parsed);
        return HU_OK;
    }

    const char *text = hu_json_get_string(event, "text");
    if (!text || strlen(text) == 0) {
        hu_json_free(alloc, parsed);
        return HU_OK;
    }

    const char *channel_id = hu_json_get_string(event, "channel");
    const char *session = channel_id ? channel_id : "unknown";

    slack_queue_push(c, session, strlen(session), text, strlen(text));
    hu_json_free(alloc, parsed);
    return HU_OK;
#endif
}

/* ─── REST polling (conversations.history) ────────────────────────────── */

hu_error_t hu_slack_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                         size_t max_msgs, size_t *out_count) {
    hu_slack_ctx_t *ctx = (hu_slack_ctx_t *)channel_ctx;
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
    {
        size_t cnt = 0;
        while (ctx->queue_count > 0 && cnt < max_msgs) {
            hu_slack_queued_msg_t *slot = &ctx->queue[ctx->queue_head];
            memcpy(msgs[cnt].session_key, slot->session_key, sizeof(slot->session_key));
            memcpy(msgs[cnt].content, slot->content, sizeof(slot->content));
            ctx->queue_head = (ctx->queue_head + 1) % SLACK_QUEUE_MAX;
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
            hu_slack_queued_msg_t *slot = &ctx->queue[ctx->queue_head];
            memcpy(msgs[cnt].session_key, slot->session_key, sizeof(slot->session_key));
            memcpy(msgs[cnt].content, slot->content, sizeof(slot->content));
            ctx->queue_head = (ctx->queue_head + 1) % SLACK_QUEUE_MAX;
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
    char auth_buf[512];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Authorization: Bearer %.*s", (int)ctx->token_len,
                      ctx->token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf))
        return HU_ERR_INTERNAL;
    size_t cnt = 0;
    for (size_t ch_idx = 0; ch_idx < ctx->channel_ids_count && cnt < max_msgs; ch_idx++) {
        const char *ch_id = ctx->channel_ids[ch_idx];
        if (!ch_id)
            continue;
        size_t ch_id_len = strlen(ch_id);
        char url_buf[512];
        const char *last_ts = (ctx->last_ts && ctx->last_ts[ch_idx]) ? ctx->last_ts[ch_idx] : NULL;
        int nu;
        if (last_ts && *last_ts)
            nu = snprintf(url_buf, sizeof(url_buf), "%s%s?channel=%.*s&oldest=%s&limit=10",
                          SLACK_API_BASE, SLACK_CONVERSATIONS_HISTORY, (int)ch_id_len, ch_id,
                          last_ts);
        else
            nu = snprintf(url_buf, sizeof(url_buf), "%s%s?channel=%.*s&limit=10", SLACK_API_BASE,
                          SLACK_CONVERSATIONS_HISTORY, (int)ch_id_len, ch_id);
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
        if (err != HU_OK || !parsed)
            continue;
        bool ok = hu_json_get_bool(parsed, "ok", false);
        if (!ok) {
            hu_json_free(alloc, parsed);
            continue;
        }
        hu_json_value_t *messages = hu_json_object_get(parsed, "messages");
        if (!messages || messages->type != HU_JSON_ARRAY) {
            hu_json_free(alloc, parsed);
            continue;
        }
        size_t arr_len = messages->data.array.len;
        for (size_t j = arr_len; j > 0 && cnt < max_msgs; j--) {
            size_t i = j - 1;
            hu_json_value_t *msg = messages->data.array.items[i];
            if (!msg || msg->type != HU_JSON_OBJECT)
                continue;
            const char *subtype = hu_json_get_string(msg, "subtype");
            if (subtype)
                continue;
            const char *user = hu_json_get_string(msg, "user");
            if (!user)
                continue;
            if (ctx->bot_user_id && strcmp(user, ctx->bot_user_id) == 0)
                continue;
            const char *text = hu_json_get_string(msg, "text");
            if (!text || strlen(text) == 0)
                continue;
            const char *ts = hu_json_get_string(msg, "ts");
            if (ts && ctx->alloc) {
                if (ctx->last_ts && ctx->last_ts[ch_idx])
                    ctx->alloc->free(ctx->alloc->ctx, ctx->last_ts[ch_idx],
                                     strlen(ctx->last_ts[ch_idx]) + 1);
                size_t ts_len = strlen(ts);
                ctx->last_ts[ch_idx] = (char *)ctx->alloc->alloc(ctx->alloc->ctx, ts_len + 1);
                if (ctx->last_ts[ch_idx])
                    memcpy(ctx->last_ts[ch_idx], ts, ts_len + 1);
            }
            size_t sk_len = ch_id_len < SLACK_SESSION_KEY_MAX ? ch_id_len : SLACK_SESSION_KEY_MAX;
            memcpy(msgs[cnt].session_key, ch_id, sk_len);
            msgs[cnt].session_key[sk_len] = '\0';
            size_t ct_len = strlen(text);
            if (ct_len > SLACK_CONTENT_MAX)
                ct_len = SLACK_CONTENT_MAX;
            memcpy(msgs[cnt].content, text, ct_len);
            msgs[cnt].content[ct_len] = '\0';
            cnt++;
        }
        hu_json_free(alloc, parsed);
    }
    *out_count = cnt;
    return HU_OK;
#endif
}

#if HU_IS_TEST
hu_error_t hu_slack_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                     size_t session_key_len, const char *content,
                                     size_t content_len) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_slack_ctx_t *c = (hu_slack_ctx_t *)ch->ctx;
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

const char *hu_slack_test_get_last_message(hu_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    hu_slack_ctx_t *c = (hu_slack_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif

hu_error_t hu_slack_create_ex(hu_allocator_t *alloc, const char *token, size_t token_len,
                              const char *const *channel_ids, size_t channel_ids_count,
                              hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (channel_ids_count > SLACK_MAX_CHANNELS)
        return HU_ERR_INVALID_ARGUMENT;
    hu_slack_ctx_t *c = (hu_slack_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
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
        c->last_ts = (char **)alloc->alloc(alloc->ctx, channel_ids_count * sizeof(char *));
        if (!c->last_ts) {
            alloc->free(alloc->ctx, c->channel_ids, channel_ids_count * sizeof(char *));
            if (c->token)
                alloc->free(alloc->ctx, c->token, c->token_len + 1);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memset(c->last_ts, 0, channel_ids_count * sizeof(char *));
        for (size_t i = 0; i < channel_ids_count; i++) {
            if (channel_ids[i]) {
                size_t len = strlen(channel_ids[i]);
                c->channel_ids[i] = (char *)alloc->alloc(alloc->ctx, len + 1);
                if (!c->channel_ids[i]) {
                    for (size_t j = 0; j < i; j++)
                        alloc->free(alloc->ctx, c->channel_ids[j], strlen(c->channel_ids[j]) + 1);
                    alloc->free(alloc->ctx, c->channel_ids, channel_ids_count * sizeof(char *));
                    alloc->free(alloc->ctx, c->last_ts, channel_ids_count * sizeof(char *));
                    if (c->token)
                        alloc->free(alloc->ctx, c->token, c->token_len + 1);
                    alloc->free(alloc->ctx, c, sizeof(*c));
                    return HU_ERR_OUT_OF_MEMORY;
                }
                memcpy(c->channel_ids[i], channel_ids[i], len + 1);
            }
        }
        c->channel_ids_count = channel_ids_count;
    }
    out->ctx = c;
    out->vtable = &slack_vtable;
    return HU_OK;
}

hu_error_t hu_slack_create(hu_allocator_t *alloc, const char *token, size_t token_len,
                           hu_channel_t *out) {
    return hu_slack_create_ex(alloc, token, token_len, NULL, 0, out);
}

void hu_slack_destroy(hu_channel_t *ch) {
    if (ch && ch->ctx) {
        hu_slack_ctx_t *c = (hu_slack_ctx_t *)ch->ctx;
        hu_allocator_t *a = c->alloc;
        slack_stream_clear(c);
        if (c->token)
            a->free(a->ctx, c->token, c->token_len + 1);
        if (c->bot_user_id && a)
            a->free(a->ctx, c->bot_user_id, strlen(c->bot_user_id) + 1);
        if (c->channel_ids) {
            for (size_t i = 0; i < c->channel_ids_count; i++)
                if (c->channel_ids[i])
                    a->free(a->ctx, c->channel_ids[i], strlen(c->channel_ids[i]) + 1);
            if (a)
                a->free(a->ctx, c->channel_ids, c->channel_ids_count * sizeof(char *));
        }
        if (c->last_ts) {
            if (a && a->free)
                for (size_t i = 0; i < c->channel_ids_count; i++)
                    if (c->last_ts[i])
                        a->free(a->ctx, c->last_ts[i], strlen(c->last_ts[i]) + 1);
            if (a)
                a->free(a->ctx, c->last_ts, c->channel_ids_count * sizeof(char *));
        }
        a->free(a->ctx, c, sizeof(*c));
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
