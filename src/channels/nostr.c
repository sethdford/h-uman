#include "human/channels/nostr.h"
#include "human/core/json.h"
#include "human/core/process_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HU_NOSTR_MAX_MSG         65536
#define HU_NOSTR_SESSION_MAX     127
#define HU_NOSTR_CONTENT_MAX     4095
#define HU_NOSTR_MAX_TARGET      128
#define HU_NOSTR_LAST_MSG_SIZE   4096
#define HU_NOSTR_MOCK_EVENTS_MAX 8

typedef struct hu_nostr_mock_event {
    char session_key[HU_NOSTR_SESSION_MAX + 1];
    char content[HU_NOSTR_CONTENT_MAX + 1];
} hu_nostr_mock_event_t;

typedef struct hu_nostr_ctx {
    hu_allocator_t *alloc;
    char *nak_path;
    char *bot_pubkey;
    char *relay_url;
    char *seckey_hex;
    bool running;
#if HU_IS_TEST
    char last_message[HU_NOSTR_LAST_MSG_SIZE];
    size_t last_message_len;
    size_t mock_event_count;
    hu_nostr_mock_event_t mock_events[HU_NOSTR_MOCK_EVENTS_MAX];
#endif
} hu_nostr_ctx_t;

static hu_error_t nostr_start(void *ctx) {
    hu_nostr_ctx_t *c = (hu_nostr_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    c->running = true;
    return HU_OK;
#else
    if (!c->nak_path || c->nak_path[0] == '\0') {
        return HU_ERR_NOT_SUPPORTED;
    }
    c->running = true;
    return HU_OK;
#endif
}

static void nostr_stop(void *ctx) {
    hu_nostr_ctx_t *c = (hu_nostr_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static hu_error_t nostr_send(void *ctx, const char *target, size_t target_len, const char *message,
                             size_t message_len, const char *const *media, size_t media_count) {
    (void)media;
    (void)media_count;
#if HU_IS_TEST
    hu_nostr_ctx_t *c = (hu_nostr_ctx_t *)ctx;
    if (c && message && message_len > 0) {
        /* Format as Nostr event JSON (kind 4 for DM). Store in buffer for test verification. */
        size_t tlen = (target && target_len > 0)
                          ? (target_len > HU_NOSTR_MAX_TARGET ? HU_NOSTR_MAX_TARGET : target_len)
                          : 0;
        int n = snprintf(c->last_message, HU_NOSTR_LAST_MSG_SIZE,
                         "{\"kind\":4,\"content\":\"%.*s\",\"tags\":[[\"p\",\"%.*s\"]]}",
                         (int)message_len, message, (int)tlen, (target && tlen > 0) ? target : "");
        if (n > 0 && (size_t)n < HU_NOSTR_LAST_MSG_SIZE) {
            c->last_message_len = (size_t)n;
        } else {
            size_t copy = message_len;
            if (copy >= HU_NOSTR_LAST_MSG_SIZE)
                copy = HU_NOSTR_LAST_MSG_SIZE - 1;
            memcpy(c->last_message, message, copy);
            c->last_message[copy] = '\0';
            c->last_message_len = copy;
        }
    }
    return HU_OK;
#else
    hu_nostr_ctx_t *c = (hu_nostr_ctx_t *)ctx;
    if (!c || !c->alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (!c->nak_path || c->nak_path[0] == '\0')
        return HU_ERR_NOT_SUPPORTED;
    if (!c->relay_url || c->relay_url[0] == '\0' || !c->seckey_hex || c->seckey_hex[0] == '\0')
        return HU_ERR_NOT_SUPPORTED;
    if (!target || target_len == 0 || target_len > HU_NOSTR_MAX_TARGET)
        return HU_ERR_INVALID_ARGUMENT;
    if (!message || message_len > HU_NOSTR_MAX_MSG)
        return HU_ERR_INVALID_ARGUMENT;

    char tmppath[] = "/tmp/hu_nostr_XXXXXX";
    int fd = mkstemp(tmppath);
    if (fd < 0)
        return HU_ERR_IO;
    ssize_t nw = write(fd, message, message_len);
    close(fd);
    if (nw < 0 || (size_t)nw != message_len) {
        unlink(tmppath);
        return HU_ERR_IO;
    }

    /* target is recipient hex pubkey; nak expects p=<hex> */
    char p_tag[HU_NOSTR_MAX_TARGET + 4];
    int nt = snprintf(p_tag, sizeof(p_tag), "p=%.*s", (int)target_len, target);
    if (nt < 0 || (size_t)nt >= sizeof(p_tag)) {
        unlink(tmppath);
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* sh -c 'nak event -k 14 -c "$(cat TMP)" -t p=X --sec S 2>/dev/null | nak event RELAY' */
    char cmd[2048];
    int nc = snprintf(cmd, sizeof(cmd),
                      "%s event -k 14 -c \"$(cat %s)\" -t %s --sec %s 2>/dev/null | %s event %s",
                      c->nak_path, tmppath, p_tag, c->seckey_hex, c->nak_path, c->relay_url);
    unlink(tmppath);
    if (nc < 0 || nc >= (int)sizeof(cmd))
        return HU_ERR_INTERNAL;

    char *sh_cmd = (char *)c->alloc->alloc(c->alloc->ctx, (size_t)nc + 1);
    if (!sh_cmd)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(sh_cmd, cmd, (size_t)nc + 1);

    const char *argv[] = {"sh", "-c", sh_cmd, NULL};
    hu_run_result_t run = {0};
    hu_error_t err = hu_process_run(c->alloc, argv, NULL, 4096, &run);
    c->alloc->free(c->alloc->ctx, sh_cmd, (size_t)nc + 1);
    hu_run_result_free(c->alloc, &run);
    return (err == HU_OK && run.success) ? HU_OK : HU_ERR_CHANNEL_SEND;
#endif
}

static const char *nostr_name(void *ctx) {
    (void)ctx;
    return "nostr";
}

static bool nostr_health_check(void *ctx) {
    hu_nostr_ctx_t *c = (hu_nostr_ctx_t *)ctx;
#if HU_IS_TEST
    return c != NULL;
#else
    return c && c->running;
#endif
}

static const hu_channel_vtable_t nostr_vtable = {
    .start = nostr_start,
    .stop = nostr_stop,
    .send = nostr_send,
    .name = nostr_name,
    .health_check = nostr_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

hu_error_t hu_nostr_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                         size_t max_msgs, size_t *out_count) {
    hu_nostr_ctx_t *ctx = (hu_nostr_ctx_t *)channel_ctx;
    if (!ctx || !msgs || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;
#if HU_IS_TEST
    (void)alloc;
    for (size_t i = 0; i < ctx->mock_event_count && i < max_msgs; i++) {
        size_t sk = strlen(ctx->mock_events[i].session_key);
        if (sk > HU_NOSTR_SESSION_MAX)
            sk = HU_NOSTR_SESSION_MAX;
        memcpy(msgs[i].session_key, ctx->mock_events[i].session_key, sk);
        msgs[i].session_key[sk] = '\0';
        size_t ct = strlen(ctx->mock_events[i].content);
        if (ct > HU_NOSTR_CONTENT_MAX)
            ct = HU_NOSTR_CONTENT_MAX;
        memcpy(msgs[i].content, ctx->mock_events[i].content, ct);
        msgs[i].content[ct] = '\0';
    }
    *out_count = ctx->mock_event_count > max_msgs ? max_msgs : ctx->mock_event_count;
    return HU_OK;
#else
#ifdef HU_GATEWAY_POSIX
    if (!ctx->nak_path || ctx->nak_path[0] == '\0')
        return HU_OK;
    if (!ctx->relay_url || ctx->relay_url[0] == '\0')
        return HU_OK;
    if (!ctx->running)
        return HU_OK;

    /* nak req -k 1 -k 4 -r <relay> fetches kind 1 (notes) and kind 4 (DMs) */
    char cmd[2048];
    int nc = snprintf(cmd, sizeof(cmd), "%s req -k 1 -k 4 -r %s 2>/dev/null", ctx->nak_path,
                      ctx->relay_url);
    if (nc < 0 || nc >= (int)sizeof(cmd))
        return HU_OK;

    char *sh_cmd = (char *)ctx->alloc->alloc(ctx->alloc->ctx, (size_t)nc + 1);
    if (!sh_cmd)
        return HU_OK;
    memcpy(sh_cmd, cmd, (size_t)nc + 1);

    const char *argv[] = {"sh", "-c", sh_cmd, NULL};
    hu_run_result_t run = {0};
    hu_error_t err = hu_process_run(ctx->alloc, argv, NULL, 65536, &run);
    ctx->alloc->free(ctx->alloc->ctx, sh_cmd, (size_t)nc + 1);
    if (err != HU_OK || !run.success || !run.stdout_buf || run.stdout_len == 0) {
        hu_run_result_free(ctx->alloc, &run);
        return HU_OK;
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

        hu_json_value_t *ev = NULL;
        if (hu_json_parse(alloc, line_start, line_len, &ev) != HU_OK || !ev)
            continue;
        if (ev->type != HU_JSON_OBJECT) {
            hu_json_free(alloc, ev);
            continue;
        }
        int kind = (int)hu_json_get_number(ev, "kind", -1);
        if (kind != 1 && kind != 4) {
            hu_json_free(alloc, ev);
            continue;
        }
        const char *content = hu_json_get_string(ev, "content");
        if (!content || strlen(content) == 0) {
            hu_json_free(alloc, ev);
            continue;
        }
        const char *pubkey = hu_json_get_string(ev, "pubkey");
        if (pubkey) {
            size_t sk = strlen(pubkey);
            if (sk > HU_NOSTR_SESSION_MAX)
                sk = HU_NOSTR_SESSION_MAX;
            memcpy(msgs[cnt].session_key, pubkey, sk);
            msgs[cnt].session_key[sk] = '\0';
        } else {
            msgs[cnt].session_key[0] = '\0';
        }
        size_t ct = strlen(content);
        if (ct > HU_NOSTR_CONTENT_MAX)
            ct = HU_NOSTR_CONTENT_MAX;
        memcpy(msgs[cnt].content, content, ct);
        msgs[cnt].content[ct] = '\0';
        cnt++;
        hu_json_free(alloc, ev);
    }
    hu_run_result_free(ctx->alloc, &run);
    *out_count = cnt;
    return HU_OK;
#else
    (void)alloc;
    (void)max_msgs;
    return HU_ERR_NOT_SUPPORTED;
#endif
#endif
}

hu_error_t hu_nostr_create(hu_allocator_t *alloc, const char *nak_path, size_t nak_path_len,
                           const char *bot_pubkey, size_t bot_pubkey_len, const char *relay_url,
                           size_t relay_url_len, const char *seckey_hex, size_t seckey_len,
                           hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_nostr_ctx_t *c = (hu_nostr_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    if (nak_path && nak_path_len > 0) {
        c->nak_path = (char *)alloc->alloc(alloc->ctx, nak_path_len + 1);
        if (!c->nak_path)
            goto oom;
        memcpy(c->nak_path, nak_path, nak_path_len);
        c->nak_path[nak_path_len] = '\0';
    }
    if (bot_pubkey && bot_pubkey_len > 0) {
        c->bot_pubkey = (char *)alloc->alloc(alloc->ctx, bot_pubkey_len + 1);
        if (!c->bot_pubkey)
            goto oom;
        memcpy(c->bot_pubkey, bot_pubkey, bot_pubkey_len);
        c->bot_pubkey[bot_pubkey_len] = '\0';
    }
    if (relay_url && relay_url_len > 0) {
        c->relay_url = (char *)alloc->alloc(alloc->ctx, relay_url_len + 1);
        if (!c->relay_url)
            goto oom;
        memcpy(c->relay_url, relay_url, relay_url_len);
        c->relay_url[relay_url_len] = '\0';
    }
    if (seckey_hex && seckey_len > 0) {
        c->seckey_hex = (char *)alloc->alloc(alloc->ctx, seckey_len + 1);
        if (!c->seckey_hex)
            goto oom;
        memcpy(c->seckey_hex, seckey_hex, seckey_len);
        c->seckey_hex[seckey_len] = '\0';
    }
    out->ctx = c;
    out->vtable = &nostr_vtable;
    return HU_OK;
oom:
    if (c->nak_path)
        alloc->free(alloc->ctx, c->nak_path, nak_path_len + 1);
    if (c->bot_pubkey)
        alloc->free(alloc->ctx, c->bot_pubkey, bot_pubkey_len + 1);
    if (c->relay_url)
        alloc->free(alloc->ctx, c->relay_url, relay_url_len + 1);
    if (c->seckey_hex)
        alloc->free(alloc->ctx, c->seckey_hex, seckey_len + 1);
    alloc->free(alloc->ctx, c, sizeof(*c));
    return HU_ERR_OUT_OF_MEMORY;
}

void hu_nostr_destroy(hu_channel_t *ch) {
    if (ch && ch->ctx) {
        hu_nostr_ctx_t *c = (hu_nostr_ctx_t *)ch->ctx;
        if (c->alloc) {
            if (c->nak_path)
                c->alloc->free(c->alloc->ctx, c->nak_path, strlen(c->nak_path) + 1);
            if (c->bot_pubkey)
                c->alloc->free(c->alloc->ctx, c->bot_pubkey, strlen(c->bot_pubkey) + 1);
            if (c->relay_url)
                c->alloc->free(c->alloc->ctx, c->relay_url, strlen(c->relay_url) + 1);
            if (c->seckey_hex)
                c->alloc->free(c->alloc->ctx, c->seckey_hex, strlen(c->seckey_hex) + 1);
            c->alloc->free(c->alloc->ctx, c, sizeof(*c));
        }
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

bool hu_nostr_is_configured(hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return false;
    hu_nostr_ctx_t *c = (hu_nostr_ctx_t *)ch->ctx;
    return c->relay_url != NULL && c->relay_url[0] != '\0' && c->seckey_hex != NULL &&
           c->seckey_hex[0] != '\0';
}

#if HU_IS_TEST
const char *hu_nostr_test_last_message(hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return NULL;
    hu_nostr_ctx_t *c = (hu_nostr_ctx_t *)ch->ctx;
    return c->last_message_len > 0 ? c->last_message : NULL;
}

hu_error_t hu_nostr_test_inject_mock_event(hu_channel_t *ch, const char *session_key,
                                           size_t session_key_len, const char *content,
                                           size_t content_len) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_nostr_ctx_t *c = (hu_nostr_ctx_t *)ch->ctx;
    if (c->mock_event_count >= HU_NOSTR_MOCK_EVENTS_MAX)
        return HU_ERR_OUT_OF_MEMORY;
    size_t i = c->mock_event_count++;
    size_t sk = session_key_len > HU_NOSTR_SESSION_MAX ? HU_NOSTR_SESSION_MAX : session_key_len;
    size_t ct = content_len > HU_NOSTR_CONTENT_MAX ? HU_NOSTR_CONTENT_MAX : content_len;
    if (session_key && sk > 0)
        memcpy(c->mock_events[i].session_key, session_key, sk);
    c->mock_events[i].session_key[sk] = '\0';
    if (content && ct > 0)
        memcpy(c->mock_events[i].content, content, ct);
    c->mock_events[i].content[ct] = '\0';
    return HU_OK;
}
#endif
