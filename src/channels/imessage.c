#include "seaclaw/channels/imessage.h"
#include "seaclaw/core/process_util.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct sc_imessage_ctx {
    sc_allocator_t *alloc;
    char *default_target;
    size_t default_target_len;
    bool running;
} sc_imessage_ctx_t;

static sc_error_t imessage_start(void *ctx) {
    sc_imessage_ctx_t *c = (sc_imessage_ctx_t *)ctx;
    if (!c) return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void imessage_stop(void *ctx) {
    sc_imessage_ctx_t *c = (sc_imessage_ctx_t *)ctx;
    if (c) c->running = false;
}

#if !SC_IS_TEST && defined(__APPLE__) && defined(__MACH__)
/* Escape " and \ for AppleScript string literal */
static size_t escape_for_applescript(char *out, size_t out_cap,
    const char *in, size_t in_len)
{
    size_t j = 0;
    for (size_t i = 0; i < in_len && j + 2 < out_cap; i++) {
        if (in[i] == '\\' || in[i] == '"') {
            out[j++] = '\\';
            out[j++] = in[i];
        } else {
            out[j++] = in[i];
        }
    }
    out[j] = '\0';
    return j;
}
#endif

static sc_error_t imessage_send(void *ctx,
    const char *target, size_t target_len,
    const char *message, size_t message_len,
    const char *const *media, size_t media_count)
{
    (void)media;
    (void)media_count;
#if !defined(__APPLE__) || !defined(__MACH__)
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    return SC_ERR_NOT_SUPPORTED;
#elif SC_IS_TEST
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    return SC_OK;
#else
    sc_imessage_ctx_t *c = (sc_imessage_ctx_t *)ctx;
    /* Use target if provided, else default_target */
    const char *tgt = target;
    size_t tgt_len = target_len;
    if ((!tgt || tgt_len == 0) && c->default_target && c->default_target_len > 0) {
        tgt = c->default_target;
        tgt_len = c->default_target_len;
    }
    if (!c || !c->alloc || !tgt || tgt_len == 0 || !message)
        return SC_ERR_INVALID_ARGUMENT;

    /* Escaped strings: worst case 2x length */
    size_t msg_esc_cap = message_len * 2 + 1;
    size_t tgt_esc_cap = tgt_len * 2 + 1;
    if (msg_esc_cap > 65536 || tgt_esc_cap > 4096) return SC_ERR_INVALID_ARGUMENT;

    char *msg_esc = (char *)c->alloc->alloc(c->alloc->ctx, msg_esc_cap);
    char *tgt_esc = (char *)c->alloc->alloc(c->alloc->ctx, tgt_esc_cap);
    if (!msg_esc || !tgt_esc) {
        if (msg_esc) c->alloc->free(c->alloc->ctx, msg_esc, msg_esc_cap);
        if (tgt_esc) c->alloc->free(c->alloc->ctx, tgt_esc, tgt_esc_cap);
        return SC_ERR_OUT_OF_MEMORY;
    }
    escape_for_applescript(msg_esc, msg_esc_cap, message, message_len);
    escape_for_applescript(tgt_esc, tgt_esc_cap, tgt, tgt_len);

    /* Script: tell application "Messages" to send "MSG" to buddy "TGT" */
    size_t script_cap = 64 + strlen(msg_esc) + strlen(tgt_esc);
    char *script = (char *)c->alloc->alloc(c->alloc->ctx, script_cap);
    if (!script) {
        c->alloc->free(c->alloc->ctx, msg_esc, msg_esc_cap);
        c->alloc->free(c->alloc->ctx, tgt_esc, tgt_esc_cap);
        return SC_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(script, script_cap,
        "tell application \"Messages\" to send \"%s\" to buddy \"%s\"",
        msg_esc, tgt_esc);
    c->alloc->free(c->alloc->ctx, msg_esc, msg_esc_cap);
    c->alloc->free(c->alloc->ctx, tgt_esc, tgt_esc_cap);
    if (n < 0 || (size_t)n >= script_cap) {
        c->alloc->free(c->alloc->ctx, script, script_cap);
        return SC_ERR_INTERNAL;
    }

    const char *argv[] = { "osascript", "-e", script, NULL };
    sc_run_result_t result = {0};
    sc_error_t err = sc_process_run(c->alloc, argv, NULL, 65536, &result);
    c->alloc->free(c->alloc->ctx, script, script_cap);
    bool ok = (err == SC_OK && result.success && result.exit_code == 0);
    sc_run_result_free(c->alloc, &result);
    if (err) return SC_ERR_CHANNEL_SEND;
    if (!ok) return SC_ERR_CHANNEL_SEND;
    return SC_OK;
#endif
}

static const char *imessage_name(void *ctx) { (void)ctx; return "imessage"; }
static bool imessage_health_check(void *ctx) { (void)ctx; return true; }

static const sc_channel_vtable_t imessage_vtable = {
    .start = imessage_start, .stop = imessage_stop, .send = imessage_send,
    .name = imessage_name, .health_check = imessage_health_check,
    .send_event = NULL, .start_typing = NULL, .stop_typing = NULL,
};

sc_error_t sc_imessage_create(sc_allocator_t *alloc,
    const char *default_target, size_t default_target_len,
    sc_channel_t *out) {
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    sc_imessage_ctx_t *c = (sc_imessage_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    c->default_target = NULL;
    c->default_target_len = 0;
    if (default_target && default_target_len > 0) {
        c->default_target = (char *)malloc(default_target_len + 1);
        if (!c->default_target) { free(c); return SC_ERR_OUT_OF_MEMORY; }
        memcpy(c->default_target, default_target, default_target_len);
        c->default_target[default_target_len] = '\0';
        c->default_target_len = default_target_len;
    }
    out->ctx = c;
    out->vtable = &imessage_vtable;
    return SC_OK;
}

bool sc_imessage_is_configured(sc_channel_t *ch) {
    if (!ch || !ch->ctx) return false;
    sc_imessage_ctx_t *c = (sc_imessage_ctx_t *)ch->ctx;
    return c->default_target != NULL && c->default_target_len > 0;
}

void sc_imessage_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_imessage_ctx_t *c = (sc_imessage_ctx_t *)ch->ctx;
        if (c->default_target) free(c->default_target);
        free(ch->ctx);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
