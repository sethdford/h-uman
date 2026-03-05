#include "seaclaw/channels/nostr.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/process_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SC_NOSTR_MAX_MSG          65536
#define SC_NOSTR_SESSION_MAX      127
#define SC_NOSTR_CONTENT_MAX      4095
#define SC_NOSTR_MAX_TARGET       128
#define SC_NOSTR_LAST_MSG_SIZE    4096
#define SC_NOSTR_MOCK_EVENTS_MAX  8

typedef struct sc_nostr_mock_event {
    char session_key[SC_NOSTR_SESSION_MAX + 1];
    char content[SC_NOSTR_CONTENT_MAX + 1];
} sc_nostr_mock_event_t;

typedef struct sc_nostr_ctx {
    sc_allocator_t *alloc;
    char *nak_path;
    char *bot_pubkey;
    char *relay_url;
    char *seckey_hex;
    bool running;
#if SC_IS_TEST
    char last_message[SC_NOSTR_LAST_MSG_SIZE];
    size_t last_message_len;
    size_t mock_event_count;
    sc_nostr_mock_event_t mock_events[SC_NOSTR_MOCK_EVENTS_MAX];
#endif
} sc_nostr_ctx_t;

static sc_error_t nostr_start(void *ctx) {
    sc_nostr_ctx_t *c = (sc_nostr_ctx_t *)ctx;
    if (!c)
        return SC_ERR_INVALID_ARGUMENT;
#if SC_IS_TEST
    c->running = true;
    return SC_OK;
#else
    if (!c->nak_path || c->nak_path[0] == '\0') {
        return SC_ERR_NOT_SUPPORTED;
    }
    c->running = true;
    return SC_OK;
#endif
}

static void nostr_stop(void *ctx) {
    sc_nostr_ctx_t *c = (sc_nostr_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static sc_error_t nostr_send(void *ctx, const char *target, size_t target_len, const char *message,
                             size_t message_len, const char *const *media, size_t media_count) {
    (void)media;
    (void)media_count;
#if SC_IS_TEST
    sc_nostr_ctx_t *c = (sc_nostr_ctx_t *)ctx;
    if (c && message && message_len > 0) {
        /* Format as Nostr event JSON (kind 4 for DM). Store in buffer for test verification. */
        size_t tlen = (target && target_len > 0) ? (target_len > SC_NOSTR_MAX_TARGET ? SC_NOSTR_MAX_TARGET : target_len) : 0;
        int n = snprintf(c->last_message, SC_NOSTR_LAST_MSG_SIZE,
                         "{\"kind\":4,\"content\":\"%.*s\",\"tags\":[[\"p\",\"%.*s\"]]}",
                         (int)message_len, message, (int)tlen, (target && tlen > 0) ? target : "");
        if (n > 0 && (size_t)n < SC_NOSTR_LAST_MSG_SIZE) {
            c->last_message_len = (size_t)n;
        } else {
            size_t copy = message_len;
            if (copy >= SC_NOSTR_LAST_MSG_SIZE)
                copy = SC_NOSTR_LAST_MSG_SIZE - 1;
            memcpy(c->last_message, message, copy);
            c->last_message[copy] = '\0';
            c->last_message_len = copy;
        }
    }
    return SC_OK;
#else
    sc_nostr_ctx_t *c = (sc_nostr_ctx_t *)ctx;
    if (!c || !c->alloc)
        return SC_ERR_INVALID_ARGUMENT;
    if (!c->nak_path || c->nak_path[0] == '\0')
        return SC_ERR_NOT_SUPPORTED;
    if (!c->relay_url || c->relay_url[0] == '\0' || !c->seckey_hex || c->seckey_hex[0] == '\0')
        return SC_ERR_NOT_SUPPORTED;
    if (!target || target_len == 0 || target_len > SC_NOSTR_MAX_TARGET)
        return SC_ERR_INVALID_ARGUMENT;
    if (!message || message_len > SC_NOSTR_MAX_MSG)
        return SC_ERR_INVALID_ARGUMENT;

    char tmppath[] = "/tmp/sc_nostr_XXXXXX";
    int fd = mkstemp(tmppath);
    if (fd < 0)
        return SC_ERR_IO;
    ssize_t nw = write(fd, message, message_len);
    close(fd);
    if (nw < 0 || (size_t)nw != message_len) {
        unlink(tmppath);
        return SC_ERR_IO;
    }

    /* target is recipient hex pubkey; nak expects p=<hex> */
    char p_tag[SC_NOSTR_MAX_TARGET + 4];
    int nt = snprintf(p_tag, sizeof(p_tag), "p=%.*s", (int)target_len, target);
    if (nt < 0 || (size_t)nt >= sizeof(p_tag)) {
        unlink(tmppath);
        return SC_ERR_INVALID_ARGUMENT;
    }

    /* sh -c 'nak event -k 14 -c "$(cat TMP)" -t p=X --sec S 2>/dev/null | nak event RELAY' */
    char cmd[2048];
    int nc = snprintf(cmd, sizeof(cmd),
                      "%s event -k 14 -c \"$(cat %s)\" -t %s --sec %s 2>/dev/null | %s event %s",
                      c->nak_path, tmppath, p_tag, c->seckey_hex, c->nak_path, c->relay_url);
    unlink(tmppath);
    if (nc < 0 || nc >= (int)sizeof(cmd))
        return SC_ERR_INTERNAL;

    char *sh_cmd = (char *)c->alloc->alloc(c->alloc->ctx, (size_t)nc + 1);
    if (!sh_cmd)
        return SC_ERR_OUT_OF_MEMORY;
    memcpy(sh_cmd, cmd, (size_t)nc + 1);

    const char *argv[] = {"sh", "-c", sh_cmd, NULL};
    sc_run_result_t run = {0};
    sc_error_t err = sc_process_run(c->alloc, argv, NULL, 4096, &run);
    c->alloc->free(c->alloc->ctx, sh_cmd, (size_t)nc + 1);
    sc_run_result_free(c->alloc, &run);
    return (err == SC_OK && run.success) ? SC_OK : SC_ERR_CHANNEL_SEND;
#endif
}

static const char *nostr_name(void *ctx) {
    (void)ctx;
    return "nostr";
}

static bool nostr_health_check(void *ctx) {
    sc_nostr_ctx_t *c = (sc_nostr_ctx_t *)ctx;
#if SC_IS_TEST
    return c != NULL;
#else
    return c && c->running;
#endif
}

static const sc_channel_vtable_t nostr_vtable = {
    .start = nostr_start,
    .stop = nostr_stop,
    .send = nostr_send,
    .name = nostr_name,
    .health_check = nostr_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

sc_error_t sc_nostr_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                         size_t max_msgs, size_t *out_count) {
    sc_nostr_ctx_t *ctx = (sc_nostr_ctx_t *)channel_ctx;
    if (!ctx || !msgs || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out_count = 0;
#if SC_IS_TEST
    (void)alloc;
    for (size_t i = 0; i < ctx->mock_event_count && i < max_msgs; i++) {
        size_t sk = strlen(ctx->mock_events[i].session_key);
        if (sk > SC_NOSTR_SESSION_MAX)
            sk = SC_NOSTR_SESSION_MAX;
        memcpy(msgs[i].session_key, ctx->mock_events[i].session_key, sk);
        msgs[i].session_key[sk] = '\0';
        size_t ct = strlen(ctx->mock_events[i].content);
        if (ct > SC_NOSTR_CONTENT_MAX)
            ct = SC_NOSTR_CONTENT_MAX;
        memcpy(msgs[i].content, ctx->mock_events[i].content, ct);
        msgs[i].content[ct] = '\0';
    }
    *out_count = ctx->mock_event_count > max_msgs ? max_msgs : ctx->mock_event_count;
    return SC_OK;
#else
#ifdef SC_GATEWAY_POSIX
    if (!ctx->nak_path || ctx->nak_path[0] == '\0')
        return SC_OK;
    if (!ctx->relay_url || ctx->relay_url[0] == '\0')
        return SC_OK;
    if (!ctx->running)
        return SC_OK;

    /* nak req -k 1 -k 4 -r <relay> fetches kind 1 (notes) and kind 4 (DMs) */
    char cmd[2048];
    int nc = snprintf(cmd, sizeof(cmd), "%s req -k 1 -k 4 -r %s 2>/dev/null", ctx->nak_path,
                      ctx->relay_url);
    if (nc < 0 || nc >= (int)sizeof(cmd))
        return SC_OK;

    char *sh_cmd = (char *)ctx->alloc->alloc(ctx->alloc->ctx, (size_t)nc + 1);
    if (!sh_cmd)
        return SC_OK;
    memcpy(sh_cmd, cmd, (size_t)nc + 1);

    const char *argv[] = {"sh", "-c", sh_cmd, NULL};
    sc_run_result_t run = {0};
    sc_error_t err = sc_process_run(ctx->alloc, argv, NULL, 65536, &run);
    ctx->alloc->free(ctx->alloc->ctx, sh_cmd, (size_t)nc + 1);
    if (err != SC_OK || !run.success || !run.stdout_buf || run.stdout_len == 0) {
        sc_run_result_free(ctx->alloc, &run);
        return SC_OK;
    }

    /* Parse NDJSON or JSON array: one event per line or [event,...] */
    size_t cnt = 0;
    const char *p = run.stdout_buf;
    const char *end = run.stdout_buf + run.stdout_len;

    while (p < end && cnt < max_msgs) {
        const char *line_start = p;
        while (p < end && *p != '\n')
            p++;
        size_t line_len = (size_t)(p - line_start);
        if (p < end)
            p++;
        if (line_len == 0)
            continue;

        sc_json_value_t *ev = NULL;
        if (sc_json_parse(alloc, line_start, line_len, &ev) != SC_OK || !ev)
            continue;
        if (ev->type != SC_JSON_OBJECT) {
            sc_json_free(alloc, ev);
            continue;
        }
        int kind = (int)sc_json_get_number(ev, "kind", -1);
        if (kind != 1 && kind != 4) {
            sc_json_free(alloc, ev);
            continue;
        }
        const char *content = sc_json_get_string(ev, "content");
        if (!content || strlen(content) == 0) {
            sc_json_free(alloc, ev);
            continue;
        }
        const char *pubkey = sc_json_get_string(ev, "pubkey");
        if (pubkey) {
            size_t sk = strlen(pubkey);
            if (sk > SC_NOSTR_SESSION_MAX)
                sk = SC_NOSTR_SESSION_MAX;
            memcpy(msgs[cnt].session_key, pubkey, sk);
            msgs[cnt].session_key[sk] = '\0';
        } else {
            msgs[cnt].session_key[0] = '\0';
        }
        size_t ct = strlen(content);
        if (ct > SC_NOSTR_CONTENT_MAX)
            ct = SC_NOSTR_CONTENT_MAX;
        memcpy(msgs[cnt].content, content, ct);
        msgs[cnt].content[ct] = '\0';
        cnt++;
        sc_json_free(alloc, ev);
    }
    sc_run_result_free(ctx->alloc, &run);
    *out_count = cnt;
    return SC_OK;
#else
    (void)alloc;
    (void)max_msgs;
    return SC_ERR_NOT_SUPPORTED;
#endif
#endif
}

sc_error_t sc_nostr_create(sc_allocator_t *alloc, const char *nak_path, size_t nak_path_len,
                           const char *bot_pubkey, size_t bot_pubkey_len, const char *relay_url,
                           size_t relay_url_len, const char *seckey_hex, size_t seckey_len,
                           sc_channel_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_nostr_ctx_t *c = (sc_nostr_ctx_t *)calloc(1, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    if (nak_path && nak_path_len > 0) {
        c->nak_path = (char *)malloc(nak_path_len + 1);
        if (!c->nak_path)
            goto oom;
        memcpy(c->nak_path, nak_path, nak_path_len);
        c->nak_path[nak_path_len] = '\0';
    }
    if (bot_pubkey && bot_pubkey_len > 0) {
        c->bot_pubkey = (char *)malloc(bot_pubkey_len + 1);
        if (!c->bot_pubkey)
            goto oom;
        memcpy(c->bot_pubkey, bot_pubkey, bot_pubkey_len);
        c->bot_pubkey[bot_pubkey_len] = '\0';
    }
    if (relay_url && relay_url_len > 0) {
        c->relay_url = (char *)malloc(relay_url_len + 1);
        if (!c->relay_url)
            goto oom;
        memcpy(c->relay_url, relay_url, relay_url_len);
        c->relay_url[relay_url_len] = '\0';
    }
    if (seckey_hex && seckey_len > 0) {
        c->seckey_hex = (char *)malloc(seckey_len + 1);
        if (!c->seckey_hex)
            goto oom;
        memcpy(c->seckey_hex, seckey_hex, seckey_len);
        c->seckey_hex[seckey_len] = '\0';
    }
    out->ctx = c;
    out->vtable = &nostr_vtable;
    return SC_OK;
oom:
    if (c->nak_path)
        free(c->nak_path);
    if (c->bot_pubkey)
        free(c->bot_pubkey);
    if (c->relay_url)
        free(c->relay_url);
    if (c->seckey_hex)
        free(c->seckey_hex);
    free(c);
    return SC_ERR_OUT_OF_MEMORY;
}

void sc_nostr_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_nostr_ctx_t *c = (sc_nostr_ctx_t *)ch->ctx;
        if (c->nak_path)
            free(c->nak_path);
        if (c->bot_pubkey)
            free(c->bot_pubkey);
        if (c->relay_url)
            free(c->relay_url);
        if (c->seckey_hex)
            free(c->seckey_hex);
        free(c);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

bool sc_nostr_is_configured(sc_channel_t *ch) {
    if (!ch || !ch->ctx)
        return false;
    sc_nostr_ctx_t *c = (sc_nostr_ctx_t *)ch->ctx;
    return c->relay_url != NULL && c->relay_url[0] != '\0' && c->seckey_hex != NULL &&
           c->seckey_hex[0] != '\0';
}

#if SC_IS_TEST
const char *sc_nostr_test_last_message(sc_channel_t *ch) {
    if (!ch || !ch->ctx)
        return NULL;
    sc_nostr_ctx_t *c = (sc_nostr_ctx_t *)ch->ctx;
    return c->last_message_len > 0 ? c->last_message : NULL;
}

sc_error_t sc_nostr_test_inject_mock_event(sc_channel_t *ch, const char *session_key,
                                          size_t session_key_len, const char *content,
                                          size_t content_len) {
    if (!ch || !ch->ctx)
        return SC_ERR_INVALID_ARGUMENT;
    sc_nostr_ctx_t *c = (sc_nostr_ctx_t *)ch->ctx;
    if (c->mock_event_count >= SC_NOSTR_MOCK_EVENTS_MAX)
        return SC_ERR_OUT_OF_MEMORY;
    size_t i = c->mock_event_count++;
    size_t sk = session_key_len > SC_NOSTR_SESSION_MAX ? SC_NOSTR_SESSION_MAX : session_key_len;
    size_t ct = content_len > SC_NOSTR_CONTENT_MAX ? SC_NOSTR_CONTENT_MAX : content_len;
    if (session_key && sk > 0)
        memcpy(c->mock_events[i].session_key, session_key, sk);
    c->mock_events[i].session_key[sk] = '\0';
    if (content && ct > 0)
        memcpy(c->mock_events[i].content, content, ct);
    c->mock_events[i].content[ct] = '\0';
    return SC_OK;
}
#endif
