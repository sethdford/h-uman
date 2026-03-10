/* WASM-compatible channel using WASI stdin/stdout for CLI mode. */
#ifdef __wasi__

#include "human/channel.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/wasm/wasi_bindings.h"
#include <string.h>
#include <stdlib.h>

#define HU_WASI_STDOUT 1
#define HU_WASI_STDIN  0

typedef struct hu_wasm_channel_ctx {
    hu_allocator_t alloc;
    bool running;
} hu_wasm_channel_ctx_t;

static hu_error_t wasm_channel_start(void *ctx) {
    hu_wasm_channel_ctx_t *c = (hu_wasm_channel_ctx_t *)ctx;
    if (!c) return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
    return HU_OK;
}

static void wasm_channel_stop(void *ctx) {
    hu_wasm_channel_ctx_t *c = (hu_wasm_channel_ctx_t *)ctx;
    if (c) c->running = false;
}

static hu_error_t wasm_channel_send(void *ctx,
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
        hu_wasi_fd_write(HU_WASI_STDOUT, message, message_len, &n);
        const char newline = '\n';
        hu_wasi_fd_write(HU_WASI_STDOUT, &newline, 1, &n);
    }
    return HU_OK;
}

static const char *wasm_channel_name(void *ctx) {
    (void)ctx;
    return "wasm_cli";
}

static bool wasm_channel_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static const hu_channel_vtable_t wasm_channel_vtable = {
    .start = wasm_channel_start,
    .stop = wasm_channel_stop,
    .send = wasm_channel_send,
    .name = wasm_channel_name,
    .health_check = wasm_channel_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

hu_error_t hu_wasm_channel_create(hu_allocator_t *alloc, hu_channel_t *out) {
    hu_wasm_channel_ctx_t *c = (hu_wasm_channel_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c) return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = *alloc;
    c->running = false;
    out->ctx = c;
    out->vtable = &wasm_channel_vtable;
    return HU_OK;
}

void hu_wasm_channel_destroy(hu_channel_t *ch) {
    if (ch && ch->ctx) {
        hu_wasm_channel_ctx_t *c = (hu_wasm_channel_ctx_t *)ch->ctx;
        hu_allocator_t *a = &c->alloc;
        a->free(a->ctx, ch->ctx, sizeof(hu_wasm_channel_ctx_t));
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

/* Read line from stdin via WASI fd_read. Caller frees. Returns NULL on EOF. */
char *hu_wasm_channel_readline(hu_allocator_t *alloc, size_t *out_len) {
    if (!alloc || !out_len) return NULL;
    *out_len = 0;
    size_t cap = 256;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) return NULL;
    size_t len = 0;
    for (;;) {
        if (len >= cap - 1) break;
        size_t nread = 0;
        int r = hu_wasi_fd_read(HU_WASI_STDIN, buf + len, cap - len - 1, &nread);
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
