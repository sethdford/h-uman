/* WASM-compatible channel using WASI stdin/stdout for CLI mode. */
#ifdef __wasi__

#include "seaclaw/channel.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/wasm/wasi_bindings.h"
#include <string.h>
#include <stdlib.h>

#define SC_WASI_STDOUT 1
#define SC_WASI_STDIN  0

typedef struct sc_wasm_channel_ctx {
    sc_allocator_t alloc;
    bool running;
} sc_wasm_channel_ctx_t;

static sc_error_t wasm_channel_start(void *ctx) {
    sc_wasm_channel_ctx_t *c = (sc_wasm_channel_ctx_t *)ctx;
    if (!c) return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void wasm_channel_stop(void *ctx) {
    sc_wasm_channel_ctx_t *c = (sc_wasm_channel_ctx_t *)ctx;
    if (c) c->running = false;
}

static sc_error_t wasm_channel_send(void *ctx,
    const char *target, size_t target_len,
    const char *message, size_t message_len,
    const char *const *media, size_t media_count)
{
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)media;
    (void)media_count;
    if (message && message_len > 0) {
        size_t n = 0;
        sc_wasi_fd_write(SC_WASI_STDOUT, message, message_len, &n);
        const char newline = '\n';
        sc_wasi_fd_write(SC_WASI_STDOUT, &newline, 1, &n);
    }
    return SC_OK;
}

static const char *wasm_channel_name(void *ctx) {
    (void)ctx;
    return "wasm_cli";
}

static bool wasm_channel_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static const sc_channel_vtable_t wasm_channel_vtable = {
    .start = wasm_channel_start,
    .stop = wasm_channel_stop,
    .send = wasm_channel_send,
    .name = wasm_channel_name,
    .health_check = wasm_channel_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

sc_error_t sc_wasm_channel_create(sc_allocator_t *alloc, sc_channel_t *out) {
    sc_wasm_channel_ctx_t *c = (sc_wasm_channel_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = *alloc;
    c->running = false;
    out->ctx = c;
    out->vtable = &wasm_channel_vtable;
    return SC_OK;
}

void sc_wasm_channel_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_wasm_channel_ctx_t *c = (sc_wasm_channel_ctx_t *)ch->ctx;
        sc_allocator_t *a = &c->alloc;
        a->free(a->ctx, ch->ctx, sizeof(sc_wasm_channel_ctx_t));
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

/* Read line from stdin via WASI fd_read. Caller frees. Returns NULL on EOF. */
char *sc_wasm_channel_readline(sc_allocator_t *alloc, size_t *out_len) {
    if (!alloc || !out_len) return NULL;
    *out_len = 0;
    size_t cap = 256;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) return NULL;
    size_t len = 0;
    for (;;) {
        if (len >= cap - 1) break;
        size_t nread = 0;
        int r = sc_wasi_fd_read(SC_WASI_STDIN, buf + len, cap - len - 1, &nread);
        if (r != 0 || nread == 0) break;
        for (size_t i = 0; i < nread && len < cap - 1; i++) {
            char ch = buf[len + i];
            if (ch == '\n' || ch == '\r' || ch == '\0') {
                buf[len] = '\0';
                *out_len = len;
                return buf;
            }
            buf[len++] = ch;
        }
    }
    buf[len] = '\0';
    *out_len = len;
    if (len == 0) {
        alloc->free(alloc->ctx, buf, cap);
        return NULL;
    }
    return buf;
}

#endif /* __wasi__ */
