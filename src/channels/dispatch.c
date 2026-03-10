#include "human/channels/dispatch.h"
#include <string.h>

#define HU_DISPATCH_INITIAL_CAP 4

typedef struct hu_dispatch_ctx {
    hu_allocator_t *alloc;
    hu_channel_t *sub_channels;
    size_t sub_count;
    size_t sub_cap;
    bool running;
} hu_dispatch_ctx_t;

static hu_error_t dispatch_start(void *ctx) {
    hu_dispatch_ctx_t *c = (hu_dispatch_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
    return HU_OK;
}

static void dispatch_stop(void *ctx) {
    hu_dispatch_ctx_t *c = (hu_dispatch_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static hu_error_t dispatch_send(void *ctx, const char *target, size_t target_len,
                                const char *message, size_t message_len, const char *const *media,
                                size_t media_count) {
#if HU_IS_TEST
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    (void)media;
    (void)media_count;
    return HU_OK;
#else
    hu_dispatch_ctx_t *c = (hu_dispatch_ctx_t *)ctx;
    if (!c || c->sub_count == 0)
        return HU_ERR_NOT_SUPPORTED;

    hu_error_t last_err = HU_OK;
    for (size_t i = 0; i < c->sub_count; i++) {
        hu_channel_t *sub = &c->sub_channels[i];
        if (sub->vtable && sub->vtable->send) {
            hu_error_t err = sub->vtable->send(sub->ctx, target, target_len, message, message_len,
                                               media, media_count);
            if (err)
                last_err = err;
        }
    }
    return last_err;
#endif
}

static const char *dispatch_name(void *ctx) {
    (void)ctx;
    return "dispatch";
}
static bool dispatch_health_check(void *ctx) {
    hu_dispatch_ctx_t *c = (hu_dispatch_ctx_t *)ctx;
    return c && c->running;
}

static const hu_channel_vtable_t dispatch_vtable = {
    .start = dispatch_start,
    .stop = dispatch_stop,
    .send = dispatch_send,
    .name = dispatch_name,
    .health_check = dispatch_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

hu_error_t hu_dispatch_create(hu_allocator_t *alloc, hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_dispatch_ctx_t *c = (hu_dispatch_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    c->sub_channels = NULL;
    c->sub_count = 0;
    c->sub_cap = 0;
    out->ctx = c;
    out->vtable = &dispatch_vtable;
    return HU_OK;
}

void hu_dispatch_destroy(hu_channel_t *ch) {
    if (ch && ch->ctx) {
        hu_dispatch_ctx_t *c = (hu_dispatch_ctx_t *)ch->ctx;
        hu_allocator_t *a = c->alloc;
        if (c->sub_channels)
            a->free(a->ctx, c->sub_channels, c->sub_cap * sizeof(hu_channel_t));
        a->free(a->ctx, c, sizeof(*c));
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

hu_error_t hu_dispatch_add_channel(hu_channel_t *dispatch_ch, const hu_channel_t *sub) {
    if (!dispatch_ch || !dispatch_ch->ctx || !sub)
        return HU_ERR_INVALID_ARGUMENT;
    hu_dispatch_ctx_t *c = (hu_dispatch_ctx_t *)dispatch_ch->ctx;
    if (c->sub_count >= c->sub_cap) {
        size_t new_cap = c->sub_cap == 0 ? HU_DISPATCH_INITIAL_CAP : c->sub_cap * 2;
        hu_channel_t *n =
            (hu_channel_t *)c->alloc->alloc(c->alloc->ctx, new_cap * sizeof(hu_channel_t));
        if (!n)
            return HU_ERR_OUT_OF_MEMORY;
        if (c->sub_channels) {
            memcpy(n, c->sub_channels, c->sub_count * sizeof(hu_channel_t));
            c->alloc->free(c->alloc->ctx, c->sub_channels, c->sub_cap * sizeof(hu_channel_t));
        }
        c->sub_channels = n;
        c->sub_cap = new_cap;
    }
    memcpy(&c->sub_channels[c->sub_count], sub, sizeof(hu_channel_t));
    c->sub_count++;
    return HU_OK;
}
