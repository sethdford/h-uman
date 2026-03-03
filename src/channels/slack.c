/*
 * Slack channel — chat.postMessage for outbound, auth.test for bot identity.
 * Supports thread replies (channel_id:thread_ts), typing, markdown→mrkdwn.
 */
#include "seaclaw/channel.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLACK_API_BASE         "https://slack.com/api"
#define SLACK_AUTH_TEST        "/auth.test"
#define SLACK_CHAT_POST        "/chat.postMessage"
#define SLACK_ASSISTANT_STATUS "/assistant.threads.setStatus"

typedef struct sc_slack_ctx {
    sc_allocator_t *alloc;
    char *token;
    size_t token_len;
    bool running;
    char *bot_user_id;
} sc_slack_ctx_t;

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
char *sc_slack_markdown_to_mrkdwn(sc_allocator_t *alloc, const char *input, size_t input_len,
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

    sc_json_buf_t buf;
    if (sc_json_buf_init(&buf, alloc) != SC_OK)
        return NULL;

    size_t i = 0;
    int line_start = 1;

    while (i < input_len) {
        /* Fenced code blocks ``` — preserve as-is */
        if (i + 3 <= input_len && memcmp(input + i, "```", 3) == 0) {
            if (sc_json_buf_append_raw(&buf, "```", 3) != SC_OK)
                goto fail;
            i += 3;
            while (i < input_len) {
                if (i + 3 <= input_len && memcmp(input + i, "```", 3) == 0) {
                    if (sc_json_buf_append_raw(&buf, "```", 3) != SC_OK)
                        goto fail;
                    i += 3;
                    break;
                }
                if (sc_json_buf_append_raw(&buf, input + i, 1) != SC_OK)
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
                if (sc_json_buf_append_raw(&buf, "*", 1) != SC_OK)
                    goto fail;
                if (sc_json_buf_append_raw(&buf, input + hi + 1, end - hi - 1) != SC_OK)
                    goto fail;
                if (sc_json_buf_append_raw(&buf, "*", 1) != SC_OK)
                    goto fail;
                i = end;
                line_start = 0;
                continue;
            }
        }

        /* Bullets - -> bullet char */
        if (line_start && i + 1 < input_len && input[i] == '-' && input[i + 1] == ' ') {
            if (sc_json_buf_append_raw(&buf, "\xe2\x80\xa2 ", 3) != SC_OK)
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
                if (sc_json_buf_append_raw(&buf, "*", 1) != SC_OK)
                    goto fail;
                if (sc_json_buf_append_raw(&buf, input + i + 2, mid) != SC_OK)
                    goto fail;
                if (sc_json_buf_append_raw(&buf, "*", 1) != SC_OK)
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
                if (sc_json_buf_append_raw(&buf, "~", 1) != SC_OK)
                    goto fail;
                if (sc_json_buf_append_raw(&buf, input + i + 2, mid) != SC_OK)
                    goto fail;
                if (sc_json_buf_append_raw(&buf, "~", 1) != SC_OK)
                    goto fail;
                i += 4 + mid;
                line_start = 0;
                continue;
            }
        }

        /* Inline code ` -> preserve */
        if (input[i] == '`') {
            if (sc_json_buf_append_raw(&buf, "`", 1) != SC_OK)
                goto fail;
            i++;
            while (i < input_len && input[i] != '`') {
                if (sc_json_buf_append_raw(&buf, input + i, 1) != SC_OK)
                    goto fail;
                i++;
            }
            if (i < input_len) {
                if (sc_json_buf_append_raw(&buf, "`", 1) != SC_OK)
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
                    if (sc_json_buf_append_raw(&buf, "<", 1) != SC_OK)
                        goto fail;
                    if (sc_json_buf_append_raw(&buf, paren_start,
                                               (size_t)(paren_end - paren_start)) != SC_OK)
                        goto fail;
                    if (sc_json_buf_append_raw(&buf, "|", 1) != SC_OK)
                        goto fail;
                    if (sc_json_buf_append_raw(&buf, input + i + 1,
                                               (size_t)(bracket_end - input - 1)) != SC_OK)
                        goto fail;
                    if (sc_json_buf_append_raw(&buf, ">", 1) != SC_OK)
                        goto fail;
                    i = (size_t)(paren_end - input) + 1;
                    line_start = 0;
                    continue;
                }
            }
        }

        if (input[i] == '\n') {
            if (sc_json_buf_append_raw(&buf, "\n", 1) != SC_OK)
                goto fail;
            i++;
            line_start = 1;
            continue;
        }

        if (sc_json_buf_append_raw(&buf, input + i, 1) != SC_OK)
            goto fail;
        i++;
        line_start = 0;
    }

    *out_len = buf.len;
    char *result = (char *)alloc->alloc(alloc->ctx, buf.len + 1);
    if (!result)
        goto fail;
    memcpy(result, buf.ptr, buf.len + 1);
    sc_json_buf_free(&buf);
    return result;
fail:
    sc_json_buf_free(&buf);
    return NULL;
}

#if !SC_IS_TEST
/* Build chat.postMessage body with optional thread_ts. */
static sc_error_t build_chat_body(sc_allocator_t *alloc, const char *channel, size_t channel_len,
                                  const char *thread_ts, size_t thread_ts_len, const char *text,
                                  size_t text_len, char **out, size_t *out_len) {
    sc_json_buf_t jbuf;
    sc_error_t err = sc_json_buf_init(&jbuf, alloc);
    if (err)
        return err;

    err = sc_json_buf_append_raw(&jbuf, "{\"channel\":", 11);
    if (err)
        goto fail;
    err = sc_json_append_string(&jbuf, channel, channel_len);
    if (err)
        goto fail;
    err = sc_json_buf_append_raw(&jbuf, ",\"mrkdwn\":true,\"text\":", 21);
    if (err)
        goto fail;
    err = sc_json_append_string(&jbuf, text, text_len);
    if (err)
        goto fail;
    if (thread_ts && thread_ts_len > 0) {
        err = sc_json_buf_append_raw(&jbuf, ",\"thread_ts\":", 13);
        if (err)
            goto fail;
        err = sc_json_append_string(&jbuf, thread_ts, thread_ts_len);
        if (err)
            goto fail;
    }
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
#endif /* !SC_IS_TEST */

/* Call auth.test to fetch bot_user_id. */
static sc_error_t fetch_auth_test(sc_slack_ctx_t *c) {
#if SC_IS_TEST
    (void)c;
    return SC_OK;
#else
    char url_buf[256];
    int n = snprintf(url_buf, sizeof(url_buf), "%s%s", SLACK_API_BASE, SLACK_AUTH_TEST);
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return SC_ERR_INTERNAL;

    char auth_buf[512];
    n = snprintf(auth_buf, sizeof(auth_buf), "Authorization: Bearer %.*s", (int)c->token_len,
                 c->token);
    if (n <= 0 || (size_t)n >= sizeof(auth_buf))
        return SC_ERR_INTERNAL;

    sc_http_response_t resp = {0};
    sc_error_t err = sc_http_get(c->alloc, url_buf, auth_buf, &resp);
    if (err || !resp.body) {
        if (resp.owned && resp.body)
            sc_http_response_free(c->alloc, &resp);
        return err;
    }

    sc_json_value_t *parsed = NULL;
    err = sc_json_parse(c->alloc, resp.body, resp.body_len, &parsed);
    if (resp.owned && resp.body)
        sc_http_response_free(c->alloc, &resp);
    if (err || !parsed)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;

    bool ok = sc_json_get_bool(parsed, "ok", false);
    if (!ok) {
        sc_json_free(c->alloc, parsed);
        return SC_ERR_CHANNEL_NOT_CONFIGURED;
    }

    const char *uid = sc_json_get_string(parsed, "user_id");
    if (uid) {
        size_t uid_len = strlen(uid);
        if (c->bot_user_id)
            c->alloc->free(c->alloc->ctx, c->bot_user_id, strlen(c->bot_user_id) + 1);
        c->bot_user_id = sc_strndup(c->alloc, uid, uid_len);
    }
    sc_json_free(c->alloc, parsed);
    return SC_OK;
#endif
}

static sc_error_t slack_start(void *ctx) {
    sc_slack_ctx_t *c = (sc_slack_ctx_t *)ctx;
    if (!c)
        return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return fetch_auth_test(c);
}

static void slack_stop(void *ctx) {
    sc_slack_ctx_t *c = (sc_slack_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static sc_error_t slack_send(void *ctx, const char *target, size_t target_len, const char *message,
                             size_t message_len, const char *const *media, size_t media_count) {
    (void)media;
    (void)media_count;
    sc_slack_ctx_t *c = (sc_slack_ctx_t *)ctx;
    if (!c || !c->alloc)
        return SC_ERR_INVALID_ARGUMENT;
    if (!c->token || c->token_len == 0)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;
    if (!target || target_len == 0 || !message)
        return SC_ERR_INVALID_ARGUMENT;

#if SC_IS_TEST
    (void)message_len;
    (void)media;
    (void)media_count;
    return SC_OK;
#else
    const char *channel = NULL;
    size_t channel_len = 0;
    const char *thread_ts = NULL;
    size_t thread_ts_len = 0;
    parse_target(target, target_len, &channel, &channel_len, &thread_ts, &thread_ts_len);

    size_t conv_len = 0;
    char *mrkdwn = sc_slack_markdown_to_mrkdwn(c->alloc, message, message_len, &conv_len);
    const char *text = mrkdwn ? mrkdwn : message;
    size_t text_len = mrkdwn ? conv_len : message_len;

    char *body = NULL;
    size_t body_len = 0;
    sc_error_t err = build_chat_body(c->alloc, channel, channel_len, thread_ts, thread_ts_len, text,
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
        return SC_ERR_INTERNAL;
    }

    char url_buf[256];
    snprintf(url_buf, sizeof(url_buf), "%s%s", SLACK_API_BASE, SLACK_CHAT_POST);

    sc_http_response_t resp = {0};
    err = sc_http_post_json(c->alloc, url_buf, auth_buf, body, body_len, &resp);
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

static sc_error_t slack_set_thread_status(sc_slack_ctx_t *c, const char *channel_id,
                                          size_t channel_id_len, const char *thread_ts,
                                          size_t thread_ts_len, const char *status,
                                          size_t status_len) {
#if SC_IS_TEST
    (void)c;
    (void)channel_id;
    (void)channel_id_len;
    (void)thread_ts;
    (void)thread_ts_len;
    (void)status;
    (void)status_len;
    return SC_OK;
#else
    if (!c->token || channel_id_len == 0 || thread_ts_len == 0)
        return SC_OK;

    sc_json_buf_t jbuf;
    if (sc_json_buf_init(&jbuf, c->alloc) != SC_OK)
        return SC_OK;
    if (sc_json_buf_append_raw(&jbuf, "{\"channel_id\":", 14) != SC_OK)
        goto out;
    if (sc_json_append_string(&jbuf, channel_id, channel_id_len) != SC_OK)
        goto out;
    if (sc_json_buf_append_raw(&jbuf, ",\"thread_ts\":", 13) != SC_OK)
        goto out;
    if (sc_json_append_string(&jbuf, thread_ts, thread_ts_len) != SC_OK)
        goto out;
    if (sc_json_buf_append_raw(&jbuf, ",\"status\":", 10) != SC_OK)
        goto out;
    if (sc_json_append_string(&jbuf, status, status_len) != SC_OK)
        goto out;
    if (sc_json_buf_append_raw(&jbuf, "}", 1) != SC_OK)
        goto out;

    char url_buf[256];
    snprintf(url_buf, sizeof(url_buf), "%s%s", SLACK_API_BASE, SLACK_ASSISTANT_STATUS);
    char auth_buf[512];
    snprintf(auth_buf, sizeof(auth_buf), "Authorization: Bearer %.*s", (int)c->token_len, c->token);

    sc_http_response_t resp = {0};
    sc_http_post_json(c->alloc, url_buf, auth_buf, jbuf.ptr, jbuf.len, &resp);
    if (resp.owned && resp.body)
        sc_http_response_free(c->alloc, &resp);
out:
    sc_json_buf_free(&jbuf);
    return SC_OK;
#endif
}

static sc_error_t slack_start_typing(void *ctx, const char *recipient, size_t recipient_len) {
    sc_slack_ctx_t *c = (sc_slack_ctx_t *)ctx;
    if (!c || recipient_len == 0)
        return SC_OK;

    const char *channel = NULL;
    size_t channel_len = 0;
    const char *thread_ts = NULL;
    size_t thread_ts_len = 0;
    parse_target(recipient, recipient_len, &channel, &channel_len, &thread_ts, &thread_ts_len);
    if (!thread_ts || thread_ts_len == 0)
        return SC_OK;

    return slack_set_thread_status(c, channel, channel_len, thread_ts, thread_ts_len,
                                   "is typing...", 12);
}

static sc_error_t slack_stop_typing(void *ctx, const char *recipient, size_t recipient_len) {
    sc_slack_ctx_t *c = (sc_slack_ctx_t *)ctx;
    if (!c || recipient_len == 0)
        return SC_OK;

    const char *channel = NULL;
    size_t channel_len = 0;
    const char *thread_ts = NULL;
    size_t thread_ts_len = 0;
    parse_target(recipient, recipient_len, &channel, &channel_len, &thread_ts, &thread_ts_len);
    if (!thread_ts || thread_ts_len == 0)
        return SC_OK;

    return slack_set_thread_status(c, channel, channel_len, thread_ts, thread_ts_len, "", 0);
}

static const char *slack_name(void *ctx) {
    (void)ctx;
    return "slack";
}

static bool slack_health_check(void *ctx) {
    sc_slack_ctx_t *c = (sc_slack_ctx_t *)ctx;
#if SC_IS_TEST
    return c != NULL;
#else
    return c && c->running;
#endif
}

static const sc_channel_vtable_t slack_vtable = {
    .start = slack_start,
    .stop = slack_stop,
    .send = slack_send,
    .name = slack_name,
    .health_check = slack_health_check,
    .send_event = NULL,
    .start_typing = slack_start_typing,
    .stop_typing = slack_stop_typing,
};

sc_error_t sc_slack_create(sc_allocator_t *alloc, const char *token, size_t token_len,
                           sc_channel_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_slack_ctx_t *c = (sc_slack_ctx_t *)calloc(1, sizeof(*c));
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
    out->ctx = c;
    out->vtable = &slack_vtable;
    return SC_OK;
}

void sc_slack_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_slack_ctx_t *c = (sc_slack_ctx_t *)ch->ctx;
        if (c->token)
            free(c->token);
        if (c->bot_user_id && c->alloc)
            c->alloc->free(c->alloc->ctx, c->bot_user_id, strlen(c->bot_user_id) + 1);
        free(c);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
