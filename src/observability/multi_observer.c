#include "seaclaw/observability/multi_observer.h"
#include "seaclaw/observer.h"
#include "seaclaw/core/error.h"
#include <string.h>

typedef struct sc_multi_observer_ctx {
    sc_allocator_t *alloc;
    sc_observer_t *observers;
    size_t count;
} sc_multi_observer_ctx_t;

static void multi_record_event(void *ctx, const sc_observer_event_t *event) {
    sc_multi_observer_ctx_t *c = (sc_multi_observer_ctx_t *)ctx;
    if (!c) return;
    for (size_t i = 0; i < c->count; i++)
        sc_observer_record_event(c->observers[i], event);
}

static void multi_record_metric(void *ctx, const sc_observer_metric_t *metric) {
    sc_multi_observer_ctx_t *c = (sc_multi_observer_ctx_t *)ctx;
    if (!c) return;
    for (size_t i = 0; i < c->count; i++)
        sc_observer_record_metric(c->observers[i], metric);
}

static void multi_flush(void *ctx) {
    sc_multi_observer_ctx_t *c = (sc_multi_observer_ctx_t *)ctx;
    if (!c) return;
    for (size_t i = 0; i < c->count; i++)
        sc_observer_flush(c->observers[i]);
}

static const char *multi_name(void *ctx) { (void)ctx; return "multi"; }

static void multi_deinit(void *ctx) {
    if (!ctx) return;
    sc_multi_observer_ctx_t *c = (sc_multi_observer_ctx_t *)ctx;
    sc_allocator_t *alloc = c->alloc;
    if (c->observers && alloc && alloc->free)
        alloc->free(alloc->ctx, c->observers, c->count * sizeof(sc_observer_t));
    if (alloc && alloc->free)
        alloc->free(alloc->ctx, c, sizeof(*c));
}

static const sc_observer_vtable_t multi_vtable = {
    .record_event = multi_record_event,
    .record_metric = multi_record_metric,
    .flush = multi_flush,
    .name = multi_name,
    .deinit = multi_deinit,
};

sc_observer_t sc_multi_observer_create(sc_allocator_t *alloc,
    const sc_observer_t *observers, size_t count)
{
    if (!alloc) return (sc_observer_t){ .ctx = NULL, .vtable = NULL };
    if (!observers && count > 0) return (sc_observer_t){ .ctx = NULL, .vtable = NULL };

    sc_multi_observer_ctx_t *ctx = (sc_multi_observer_ctx_t *)alloc->alloc(alloc->ctx, sizeof(sc_multi_observer_ctx_t));
    if (!ctx) return (sc_observer_t){ .ctx = NULL, .vtable = NULL };

    ctx->alloc = alloc;
    ctx->count = count;
    ctx->observers = NULL;

    if (count > 0) {
        ctx->observers = (sc_observer_t *)alloc->alloc(alloc->ctx, count * sizeof(sc_observer_t));
        if (!ctx->observers) {
            alloc->free(alloc->ctx, ctx, sizeof(*ctx));
            return (sc_observer_t){ .ctx = NULL, .vtable = NULL };
        }
        memcpy(ctx->observers, observers, count * sizeof(sc_observer_t));
    }

    return (sc_observer_t){ .ctx = ctx, .vtable = &multi_vtable };
}
