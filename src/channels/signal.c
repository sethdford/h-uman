/*
 * Signal channel — signal-cli daemon HTTP/JSON-RPC.
 * Sends via /api/v1/rpc, receives via SSE at /api/v1/events,
 * health at /api/v1/check.
 */
#include "seaclaw/channels/signal.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#if !SC_IS_TEST
#include <unistd.h>
#endif

#if !SC_IS_TEST && defined(SC_HTTP_CURL)
#include <pthread.h>
#endif

#define SIGNAL_RPC_ENDPOINT "/api/v1/rpc"
#define SIGNAL_SSE_ENDPOINT "/api/v1/events"
#define SIGNAL_HEALTH_ENDPOINT "/api/v1/check"
#define SIGNAL_TYPING_INTERVAL_SEC 8

typedef struct sc_signal_ctx {
    sc_allocator_t *alloc;
    char *http_url;
    size_t http_url_len;
    char *account;
    size_t account_len;
    bool running;
    const char *const *allow_from;
    size_t allow_from_count;
    const char *const *group_allow_from;
    size_t group_allow_from_count;
    char *group_policy;
    size_t group_policy_len;
#if !SC_IS_TEST && defined(SC_HTTP_CURL)
    pthread_mutex_t typing_mu;
    char *typing_target;
    size_t typing_target_len;
    volatile int typing_stop;
    pthread_t typing_thread;
#endif
} sc_signal_ctx_t;

#if !SC_IS_TEST
/* Build URL for RPC, SSE, or health. */
static int build_url(char *buf, size_t cap, const sc_signal_ctx_t *c,
    const char *endpoint)
{
    return snprintf(buf, cap, "%.*s%s",
        (int)c->http_url_len, c->http_url, endpoint);
}

static int build_sse_url(char *buf, size_t cap, const sc_signal_ctx_t *c)
{
    int n = snprintf(buf, cap, "%.*s%s?account=",
        (int)c->http_url_len, c->http_url, SIGNAL_SSE_ENDPOINT);
    if (n < 0 || (size_t)n >= cap) return -1;
    size_t pos = (size_t)n;
    for (size_t i = 0; i < c->account_len && pos + 4 < cap; i++) {
        if (c->account[i] == '+') {
            memcpy(buf + pos, "%2B", 3);
            pos += 3;
        } else {
            buf[pos++] = c->account[i];
        }
    }
    buf[pos] = '\0';
    return (int)pos;
}

/* Parse target: "group:xxx" -> group, else direct recipient. */
static int parse_target(const char *target, size_t target_len,
    int *out_is_group, const char **out_id, size_t *out_id_len)
{
    const char *prefix = SC_SIGNAL_GROUP_TARGET_PREFIX;
    size_t prefix_len = strlen(prefix);
    if (target_len >= prefix_len && strncasecmp(target, prefix, prefix_len) == 0) {
        *out_is_group = 1;
        *out_id = target + prefix_len;
        *out_id_len = target_len - prefix_len;
    } else {
        *out_is_group = 0;
        *out_id = target;
        *out_id_len = target_len;
    }
    return 0;
}

/* Chunk message at SC_SIGNAL_MAX_MSG, prefer newline then space. */
static size_t chunk_next(const char *buf, size_t len, size_t cursor, size_t *out_end)
{
    if (cursor >= len) {
        *out_end = cursor;
        return 0;
    }
    size_t max_chunk = len - cursor;
    if (max_chunk > SC_SIGNAL_MAX_MSG) max_chunk = SC_SIGNAL_MAX_MSG;
    size_t search_end = cursor + max_chunk;
    if (search_end > len) search_end = len;
    size_t half = cursor + max_chunk / 2;
    size_t split_at = search_end;
    for (size_t i = search_end; i > cursor; i--) {
        if (buf[i - 1] == '\n' && (i - 1) >= half) {
            split_at = i;
            goto done;
        }
    }
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

/* Build JSON-RPC body for send or sendTyping. */
static sc_error_t build_rpc_body(sc_allocator_t *alloc,
    const char *method,
    int is_group, const char *id, size_t id_len,
    const char *account, size_t account_len,
    const char *message, size_t message_len,
    char **out, size_t *out_len)
{
    sc_json_buf_t jbuf;
    sc_error_t err = sc_json_buf_init(&jbuf, alloc);
    if (err) return err;

    err = sc_json_buf_append_raw(&jbuf, "{\"jsonrpc\":\"2.0\",\"method\":", 28);
    if (err) goto fail;
    err = sc_json_append_string(&jbuf, method, strlen(method));
    if (err) goto fail;
    err = sc_json_buf_append_raw(&jbuf, ",\"params\":{", 10);
    if (err) goto fail;

    if (is_group) {
        err = sc_json_buf_append_raw(&jbuf, "\"groupId\":", 10);
        if (err) goto fail;
        err = sc_json_append_string(&jbuf, id, id_len);
    } else {
        err = sc_json_buf_append_raw(&jbuf, "\"recipient\":[", 13);
        if (err) goto fail;
        err = sc_json_append_string(&jbuf, id, id_len);
        if (err) goto fail;
        err = sc_json_buf_append_raw(&jbuf, "]", 1);
    }
    if (err) goto fail;

    err = sc_json_buf_append_raw(&jbuf, ",\"account\":", 11);
    if (err) goto fail;
    err = sc_json_append_string(&jbuf, account, account_len);
    if (err) goto fail;

    if (message && message_len > 0) {
        err = sc_json_buf_append_raw(&jbuf, ",\"message\":", 11);
        if (err) goto fail;
        err = sc_json_append_string(&jbuf, message, message_len);
        if (err) goto fail;
    }

    err = sc_json_buf_append_raw(&jbuf, "},\"id\":\"1\"}", 10);
    if (err) goto fail;

    *out_len = jbuf.len;
    *out = (char *)alloc->alloc(alloc->ctx, jbuf.len + 1);
    if (!*out) { err = SC_ERR_OUT_OF_MEMORY; goto fail; }
    memcpy(*out, jbuf.ptr, jbuf.len + 1);
    sc_json_buf_free(&jbuf);
    return SC_OK;
fail:
    sc_json_buf_free(&jbuf);
    return err;
}

/* Send one JSON-RPC payload. */
static sc_error_t send_rpc(sc_signal_ctx_t *c, const char *body, size_t body_len)
{
    char url_buf[512];
    int n = build_url(url_buf, sizeof(url_buf), c, SIGNAL_RPC_ENDPOINT);
    if (n < 0 || (size_t)n >= sizeof(url_buf)) return SC_ERR_INTERNAL;

    sc_http_response_t resp = {0};
    sc_error_t err = sc_http_post_json(c->alloc, url_buf, NULL, body, body_len, &resp);
    if (err) {
        if (resp.owned && resp.body) sc_http_response_free(c->alloc, &resp);
        return SC_ERR_CHANNEL_SEND;
    }
    if (resp.owned && resp.body) sc_http_response_free(c->alloc, &resp);
    return SC_OK;
}
#endif /* !SC_IS_TEST */

#if !SC_IS_TEST && defined(SC_HTTP_CURL)
static void *typing_thread_fn(void *arg)
{
    sc_signal_ctx_t *c = (sc_signal_ctx_t *)arg;
    while (!c->typing_stop && c->running) {
        pthread_mutex_lock(&c->typing_mu);
        char *target = c->typing_target;
        size_t target_len = c->typing_target_len;
        pthread_mutex_unlock(&c->typing_mu);
        if (!target) break;

        int is_group = 0;
        const char *id = NULL;
        size_t id_len = 0;
        parse_target(target, target_len, &is_group, &id, &id_len);
        char *body = NULL;
        size_t body_len = 0;
        if (build_rpc_body(c->alloc, "sendTyping", is_group, id, id_len,
                c->account, c->account_len, NULL, 0, &body, &body_len) == SC_OK && body) {
            send_rpc(c, body, body_len);
            c->alloc->free(c->alloc->ctx, body, body_len + 1);
        }

        for (int s = 0; s < SIGNAL_TYPING_INTERVAL_SEC && !c->typing_stop; s++)
            sleep(1);
    }
    return NULL;
}
#endif

static sc_error_t signal_start(void *ctx)
{
    sc_signal_ctx_t *c = (sc_signal_ctx_t *)ctx;
    if (!c) return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void signal_stop(void *ctx)
{
    sc_signal_ctx_t *c = (sc_signal_ctx_t *)ctx;
    if (c) c->running = false;
#if !SC_IS_TEST && defined(SC_HTTP_CURL)
    c->typing_stop = 1;
    if (c->typing_thread) {
        pthread_join(c->typing_thread, NULL);
        c->typing_thread = 0;
    }
    pthread_mutex_lock(&c->typing_mu);
    if (c->typing_target) {
        c->alloc->free(c->alloc->ctx, c->typing_target, c->typing_target_len + 1);
        c->typing_target = NULL;
        c->typing_target_len = 0;
    }
    pthread_mutex_unlock(&c->typing_mu);
#endif
}

static sc_error_t signal_send(void *ctx,
    const char *target, size_t target_len,
    const char *message, size_t message_len,
    const char *const *media, size_t media_count)
{
    (void)media;
    (void)media_count;
    sc_signal_ctx_t *c = (sc_signal_ctx_t *)ctx;
    if (!c || !c->alloc) return SC_ERR_INVALID_ARGUMENT;
    if (!c->http_url || !target || target_len == 0 || !message) return SC_ERR_INVALID_ARGUMENT;

#if SC_IS_TEST
    (void)message_len;
    (void)media;
    (void)media_count;
    return SC_OK;
#else
    int is_group = 0;
    const char *id = NULL;
    size_t id_len = 0;
    parse_target(target, target_len, &is_group, &id, &id_len);

    size_t cursor = 0;
    sc_error_t last_err = SC_OK;
    while (cursor < message_len) {
        size_t chunk_end = 0;
        size_t chunk_sz = chunk_next(message, message_len, cursor, &chunk_end);
        if (chunk_sz == 0) break;

        char *body = NULL;
        size_t body_len = 0;
        sc_error_t err = build_rpc_body(c->alloc, "send", is_group, id, id_len,
            c->account, c->account_len,
            message + cursor, chunk_sz, &body, &body_len);
        if (err) return err;
        if (!body) return SC_ERR_OUT_OF_MEMORY;

        last_err = send_rpc(c, body, body_len);
        c->alloc->free(c->alloc->ctx, body, body_len + 1);
        if (last_err != SC_OK) return last_err;
        cursor = chunk_end;
    }
    return last_err;
#endif
}

static sc_error_t signal_start_typing(void *ctx, const char *recipient, size_t recipient_len)
{
#if SC_IS_TEST
    (void)ctx;
    (void)recipient;
    (void)recipient_len;
    return SC_OK;
#else
#if defined(SC_HTTP_CURL)
    sc_signal_ctx_t *c = (sc_signal_ctx_t *)ctx;
    if (!c || recipient_len == 0) return SC_OK;

    pthread_mutex_lock(&c->typing_mu);
    if (c->typing_target) {
        c->alloc->free(c->alloc->ctx, c->typing_target, c->typing_target_len + 1);
        c->typing_target = NULL;
    }
    c->typing_target = sc_strndup(c->alloc, recipient, recipient_len);
    if (!c->typing_target) {
        pthread_mutex_unlock(&c->typing_mu);
        return SC_ERR_OUT_OF_MEMORY;
    }
    c->typing_target_len = recipient_len;
    c->typing_stop = 0;
    if (!c->typing_thread) {
        if (pthread_create(&c->typing_thread, NULL, typing_thread_fn, c) != 0) {
            c->alloc->free(c->alloc->ctx, c->typing_target, recipient_len + 1);
            c->typing_target = NULL;
        }
    }
    pthread_mutex_unlock(&c->typing_mu);
#endif
    return SC_OK;
#endif
}

static sc_error_t signal_stop_typing(void *ctx, const char *recipient, size_t recipient_len)
{
#if SC_IS_TEST
    (void)ctx;
    (void)recipient;
    (void)recipient_len;
    return SC_OK;
#else
#if defined(SC_HTTP_CURL)
    sc_signal_ctx_t *c = (sc_signal_ctx_t *)ctx;
    if (!c) return SC_OK;

    pthread_mutex_lock(&c->typing_mu);
    if (c->typing_target && recipient_len == c->typing_target_len &&
        memcmp(c->typing_target, recipient, recipient_len) == 0) {
        c->typing_stop = 1;
        c->alloc->free(c->alloc->ctx, c->typing_target, c->typing_target_len + 1);
        c->typing_target = NULL;
        c->typing_target_len = 0;
    }
    pthread_mutex_unlock(&c->typing_mu);
    if (c->typing_thread) {
        pthread_join(c->typing_thread, NULL);
        c->typing_thread = 0;
    }
#endif
    return SC_OK;
#endif
}

static const char *signal_name(void *ctx) { (void)ctx; return "signal"; }

static bool signal_health_check(void *ctx)
{
    sc_signal_ctx_t *c = (sc_signal_ctx_t *)ctx;
    if (!c || !c->running) return false;

#if SC_IS_TEST
    return true;
#else
    if (!c->http_url || c->http_url_len == 0) return false;
    char url_buf[512];
    int n = build_url(url_buf, sizeof(url_buf), c, SIGNAL_HEALTH_ENDPOINT);
    if (n < 0 || (size_t)n >= sizeof(url_buf)) return false;

    sc_http_response_t resp = {0};
    sc_error_t err = sc_http_get(c->alloc, url_buf, NULL, &resp);
    if (err) {
        if (resp.owned && resp.body) sc_http_response_free(c->alloc, &resp);
        return false;
    }
    bool ok = (resp.status_code >= 200 && resp.status_code < 300);
    if (resp.owned && resp.body) sc_http_response_free(c->alloc, &resp);
    return ok;
#endif
}

static const sc_channel_vtable_t signal_vtable = {
    .start = signal_start,
    .stop = signal_stop,
    .send = signal_send,
    .name = signal_name,
    .health_check = signal_health_check,
    .send_event = NULL,
    .start_typing = signal_start_typing,
    .stop_typing = signal_stop_typing,
};

static void free_group_policy(sc_signal_ctx_t *c)
{
    if (c->group_policy) {
        c->alloc->free(c->alloc->ctx, c->group_policy, c->group_policy_len + 1);
        c->group_policy = NULL;
        c->group_policy_len = 0;
    }
}

sc_error_t sc_signal_create(sc_allocator_t *alloc,
    const char *http_url, size_t http_url_len,
    const char *account, size_t account_len,
    sc_channel_t *out)
{
    return sc_signal_create_ex(alloc, http_url, http_url_len, account, account_len,
        NULL, 0, NULL, 0, SC_SIGNAL_GROUP_POLICY_ALLOWLIST,
        strlen(SC_SIGNAL_GROUP_POLICY_ALLOWLIST), out);
}

sc_error_t sc_signal_create_ex(sc_allocator_t *alloc,
    const char *http_url, size_t http_url_len,
    const char *account, size_t account_len,
    const char *const *allow_from, size_t allow_from_count,
    const char *const *group_allow_from, size_t group_allow_from_count,
    const char *group_policy, size_t group_policy_len,
    sc_channel_t *out)
{
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    sc_signal_ctx_t *c = (sc_signal_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
#if !SC_IS_TEST && defined(SC_HTTP_CURL)
    pthread_mutex_init(&c->typing_mu, NULL);
#endif

    if (http_url && http_url_len > 0) {
        c->http_url = (char *)malloc(http_url_len + 1);
        if (!c->http_url) { free(c); return SC_ERR_OUT_OF_MEMORY; }
        memcpy(c->http_url, http_url, http_url_len);
        c->http_url[http_url_len] = '\0';
        c->http_url_len = http_url_len;
    }

    if (account && account_len > 0) {
        c->account = (char *)malloc(account_len + 1);
        if (!c->account) {
            if (c->http_url) free(c->http_url);
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->account, account, account_len);
        c->account[account_len] = '\0';
        c->account_len = account_len;
    }

    c->allow_from = allow_from;
    c->allow_from_count = allow_from_count;
    c->group_allow_from = group_allow_from;
    c->group_allow_from_count = group_allow_from_count;

    if (group_policy && group_policy_len > 0) {
        c->group_policy = (char *)alloc->alloc(alloc->ctx, group_policy_len + 1);
        if (!c->group_policy) {
            if (c->http_url) free(c->http_url);
            if (c->account) free(c->account);
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->group_policy, group_policy, group_policy_len);
        c->group_policy[group_policy_len] = '\0';
        c->group_policy_len = group_policy_len;
    }

    out->ctx = c;
    out->vtable = &signal_vtable;
    return SC_OK;
}

#if !SC_IS_TEST
/* Parse SSE data lines, extract "data: {json}" events. */
static size_t parse_sse_events(const char *buf, size_t buf_len,
    sc_channel_loop_msg_t *msgs, size_t max_msgs,
    sc_allocator_t *alloc)
{
    size_t count = 0;
    size_t i = 0;
    while (i < buf_len && count < max_msgs) {
        if (i + 5 < buf_len && strncmp(buf + i, "data:", 5) == 0) {
            i += 5;
            while (i < buf_len && (buf[i] == ' ' || buf[i] == '\t')) i++;
            size_t line_end = i;
            while (line_end < buf_len && buf[line_end] != '\n' && buf[line_end] != '\r')
                line_end++;
            if (line_end > i) {
                const char *json = buf + i;
                size_t json_len = line_end - i;
                if (json_len > 0 && json[0] == '{') {
                    sc_json_value_t *parsed = NULL;
                    if (sc_json_parse(alloc, json, json_len, &parsed) == SC_OK && parsed) {
                        sc_json_value_t *env = sc_json_object_get(parsed, "envelope");
                        if (env && env->type == SC_JSON_OBJECT) {
                            sc_json_value_t *dm = sc_json_object_get(env, "dataMessage");
                            if (dm && dm->type == SC_JSON_OBJECT) {
                                const char *msg = sc_json_get_string(dm, "message");
                                const char *src = sc_json_get_string(env, "sourceNumber");
                                if (!src) src = sc_json_get_string(env, "source");
                                if (msg && src) {
                                    size_t msg_len = msg ? strlen(msg) : 0;
                                    size_t src_len = src ? strlen(src) : 0;
                                    if (msg_len > 0 && msg_len < sizeof(msgs[count].content) &&
                                        src_len > 0 && src_len < sizeof(msgs[count].session_key)) {
                                        memcpy(msgs[count].session_key, src, src_len + 1);
                                        memcpy(msgs[count].content, msg, msg_len + 1);
                                        count++;
                                    }
                                }
                            }
                        }
                        sc_json_free(alloc, parsed);
                    }
                }
            }
            i = line_end;
        }
        while (i < buf_len && buf[i] != '\n') i++;
        if (i < buf_len) i++;
    }
    return count;
}
#endif

sc_error_t sc_signal_poll(void *channel_ctx,
    sc_allocator_t *alloc,
    sc_channel_loop_msg_t *msgs,
    size_t max_msgs,
    size_t *out_count)
{
    sc_signal_ctx_t *c = (sc_signal_ctx_t *)channel_ctx;
    if (!c || !alloc || !msgs || !out_count) return SC_ERR_INVALID_ARGUMENT;
    (void)max_msgs;
    *out_count = 0;

#if SC_IS_TEST
    return SC_OK;
#else
    if (!c->http_url || !c->account || c->account_len == 0) return SC_OK;

    char url_buf[1024];
    int n = build_sse_url(url_buf, sizeof(url_buf), c);
    if (n < 0 || (size_t)n >= sizeof(url_buf)) return SC_OK;

    sc_http_response_t resp = {0};
    sc_error_t err = sc_http_get_ex(c->alloc, url_buf, "Accept: text/event-stream\n", &resp);
    if (err || !resp.body || resp.body_len == 0) {
        if (resp.owned && resp.body) sc_http_response_free(c->alloc, &resp);
        return SC_OK;
    }

    *out_count = parse_sse_events(resp.body, resp.body_len, msgs, max_msgs, alloc);
    if (resp.owned && resp.body) sc_http_response_free(c->alloc, &resp);
    return SC_OK;
#endif
}

void sc_signal_destroy(sc_channel_t *ch)
{
    if (ch && ch->ctx) {
        sc_signal_ctx_t *c = (sc_signal_ctx_t *)ch->ctx;
        signal_stop(c);
        free_group_policy(c);
        if (c->http_url) free(c->http_url);
        if (c->account) free(c->account);
#if !SC_IS_TEST && defined(SC_HTTP_CURL)
        pthread_mutex_destroy(&c->typing_mu);
#endif
        free(c);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
