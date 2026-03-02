#include "seaclaw/channels/dispatch.h"
#include <string.h>
#include <stdlib.h>

#define SC_DISPATCH_INITIAL_CAP 4

typedef struct sc_dispatch_ctx {
    sc_allocator_t *alloc;
    sc_channel_t *sub_channels;
    size_t sub_count;
    size_t sub_cap;
    bool running;
} sc_dispatch_ctx_t;

static sc_error_t dispatch_start(void *ctx) {
    sc_dispatch_ctx_t *c = (sc_dispatch_ctx_t *)ctx;
    if (!c) return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void dispatch_stop(void *ctx) {
    sc_dispatch_ctx_t *c = (sc_dispatch_ctx_t *)ctx;
    if (c) c->running = false;
}

static sc_error_t dispatch_send(void *ctx,
    const char *target, size_t target_len,
    const char *message, size_t message_len,
    const char *const *media, size_t media_count)
{
#if SC_IS_TEST
    (void)ctx; (void)target; (void)target_len; (void)message; (void)message_len;
    (void)media; (void)media_count;
    return SC_OK;
#else
    sc_dispatch_ctx_t *c = (sc_dispatch_ctx_t *)ctx;
    if (!c || c->sub_count == 0) return SC_ERR_NOT_SUPPORTED;

    sc_error_t last_err = SC_OK;
    for (size_t i = 0; i < c->sub_count; i++) {
        sc_channel_t *sub = &c->sub_channels[i];
        if (sub->vtable && sub->vtable->send) {
            sc_error_t err = sub->vtable->send(sub->ctx, target, target_len,
                message, message_len, media, media_count);
            if (err) last_err = err;
        }
    }
    return last_err;
#endif
}

static const char *dispatch_name(void *ctx) { (void)ctx; return "dispatch"; }
static bool dispatch_health_check(void *ctx) {
    sc_dispatch_ctx_t *c = (sc_dispatch_ctx_t *)ctx;
    return c && c->running;
}

static const sc_channel_vtable_t dispatch_vtable = {
    .start = dispatch_start, .stop = dispatch_stop, .send = dispatch_send,
    .name = dispatch_name, .health_check = dispatch_health_check,
    .send_event = NULL, .start_typing = NULL, .stop_typing = NULL,
};

sc_error_t sc_dispatch_create(sc_allocator_t *alloc, sc_channel_t *out) {
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    sc_dispatch_ctx_t *c = (sc_dispatch_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    c->sub_channels = NULL;
    c->sub_count = 0;
    c->sub_cap = 0;
    out->ctx = c;
    out->vtable = &dispatch_vtable;
    return SC_OK;
}

void sc_dispatch_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_dispatch_ctx_t *c = (sc_dispatch_ctx_t *)ch->ctx;
        if (c->sub_channels) free(c->sub_channels);
        free(ch->ctx);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

sc_error_t sc_dispatch_add_channel(sc_channel_t *dispatch_ch, const sc_channel_t *sub) {
    if (!dispatch_ch || !dispatch_ch->ctx || !sub) return SC_ERR_INVALID_ARGUMENT;
    sc_dispatch_ctx_t *c = (sc_dispatch_ctx_t *)dispatch_ch->ctx;
    if (c->sub_count >= c->sub_cap) {
        size_t new_cap = c->sub_cap == 0 ? SC_DISPATCH_INITIAL_CAP : c->sub_cap * 2;
        sc_channel_t *n = (sc_channel_t *)realloc(c->sub_channels,
            new_cap * sizeof(sc_channel_t));
        if (!n) return SC_ERR_OUT_OF_MEMORY;
        c->sub_channels = n;
        c->sub_cap = new_cap;
    }
    memcpy(&c->sub_channels[c->sub_count], sub, sizeof(sc_channel_t));
    c->sub_count++;
    return SC_OK;
}
