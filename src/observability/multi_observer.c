#include "human/observability/multi_observer.h"
#include "human/core/error.h"
#include "human/observer.h"
#include <string.h>

typedef struct hu_multi_observer_ctx {
    hu_allocator_t *alloc;
    hu_observer_t *observers;
    size_t count;
} hu_multi_observer_ctx_t;

static void multi_record_event(void *ctx, const hu_observer_event_t *event) {
    hu_multi_observer_ctx_t *c = (hu_multi_observer_ctx_t *)ctx;
    if (!c)
        return;
    for (size_t i = 0; i < c->count; i++)
        hu_observer_record_event(c->observers[i], event);
}

static void multi_record_metric(void *ctx, const hu_observer_metric_t *metric) {
    hu_multi_observer_ctx_t *c = (hu_multi_observer_ctx_t *)ctx;
    if (!c)
        return;
    for (size_t i = 0; i < c->count; i++)
        hu_observer_record_metric(c->observers[i], metric);
}

static void multi_flush(void *ctx) {
    hu_multi_observer_ctx_t *c = (hu_multi_observer_ctx_t *)ctx;
    if (!c)
        return;
    for (size_t i = 0; i < c->count; i++)
        hu_observer_flush(c->observers[i]);
}

static const char *multi_name(void *ctx) {
    (void)ctx;
    return "multi";
}

static void multi_deinit(void *ctx) {
    if (!ctx)
        return;
    hu_multi_observer_ctx_t *c = (hu_multi_observer_ctx_t *)ctx;
    hu_allocator_t *alloc = c->alloc;
    if (c->observers && alloc && alloc->free)
        alloc->free(alloc->ctx, c->observers, c->count * sizeof(hu_observer_t));
    if (alloc && alloc->free)
        alloc->free(alloc->ctx, c, sizeof(*c));
}

static const hu_observer_vtable_t multi_vtable = {
    .record_event = multi_record_event,
    .record_metric = multi_record_metric,
    .flush = multi_flush,
    .name = multi_name,
    .deinit = multi_deinit,
};

hu_observer_t hu_multi_observer_create(hu_allocator_t *alloc, const hu_observer_t *observers,
                                       size_t count) {
    if (!alloc)
        return (hu_observer_t){.ctx = NULL, .vtable = NULL};
    if (!observers && count > 0)
        return (hu_observer_t){.ctx = NULL, .vtable = NULL};

    hu_multi_observer_ctx_t *ctx =
        (hu_multi_observer_ctx_t *)alloc->alloc(alloc->ctx, sizeof(hu_multi_observer_ctx_t));
    if (!ctx)
        return (hu_observer_t){.ctx = NULL, .vtable = NULL};

    ctx->alloc = alloc;
    ctx->count = count;
    ctx->observers = NULL;

    if (count > 0) {
        ctx->observers = (hu_observer_t *)alloc->alloc(alloc->ctx, count * sizeof(hu_observer_t));
        if (!ctx->observers) {
            alloc->free(alloc->ctx, ctx, sizeof(*ctx));
            return (hu_observer_t){.ctx = NULL, .vtable = NULL};
        }
        memcpy(ctx->observers, observers, count * sizeof(hu_observer_t));
    }

    return (hu_observer_t){.ctx = ctx, .vtable = &multi_vtable};
}
