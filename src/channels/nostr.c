#include "seaclaw/channels/nostr.h"
#include "seaclaw/core/process_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SC_NOSTR_MAX_MSG       65536
#define SC_NOSTR_MAX_TARGET    128
#define SC_NOSTR_LAST_MSG_SIZE 4096

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
    (void)target;
    (void)target_len;
    sc_nostr_ctx_t *c = (sc_nostr_ctx_t *)ctx;
    if (c && message && message_len > 0) {
        size_t copy = message_len;
        if (copy >= SC_NOSTR_LAST_MSG_SIZE)
            copy = SC_NOSTR_LAST_MSG_SIZE - 1;
        memcpy(c->last_message, message, copy);
        c->last_message[copy] = '\0';
        c->last_message_len = copy;
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
#endif
