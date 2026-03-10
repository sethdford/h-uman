#include "human/observability/metrics_observer.h"
#include "human/core/error.h"
#include "human/observer.h"
#include <stdint.h>
#include <string.h>

typedef struct hu_metrics_impl_ctx {
    hu_allocator_t *alloc;
    uint64_t total_requests;
    uint64_t total_tokens;
    uint64_t total_tool_calls;
    uint64_t total_errors;
    uint64_t latency_sum_ms;
    uint64_t latency_count;
    uint64_t active_sessions;
} hu_metrics_impl_ctx_t;

static void metrics_record_event(void *ctx, const hu_observer_event_t *event) {
    hu_metrics_impl_ctx_t *c = (hu_metrics_impl_ctx_t *)ctx;
    if (!c)
        return;

    switch (event->tag) {
    case HU_OBSERVER_EVENT_AGENT_START:
        c->active_sessions++;
        c->total_requests++;
        break;
    case HU_OBSERVER_EVENT_LLM_REQUEST:
        /* Request counted at agent_start */
        break;
    case HU_OBSERVER_EVENT_LLM_RESPONSE:
        c->latency_sum_ms += event->data.llm_response.duration_ms;
        c->latency_count++;
        if (!event->data.llm_response.success)
            c->total_errors++;
        break;
    case HU_OBSERVER_EVENT_AGENT_END:
        c->total_tokens += event->data.agent_end.tokens_used;
        if (c->active_sessions > 0)
            c->active_sessions--;
        break;
    case HU_OBSERVER_EVENT_TOOL_CALL_START:
        break;
    case HU_OBSERVER_EVENT_TOOL_CALL:
        c->total_tool_calls++;
        if (!event->data.tool_call.success)
            c->total_errors++;
        break;
    case HU_OBSERVER_EVENT_TOOL_ITERATIONS_EXHAUSTED:
    case HU_OBSERVER_EVENT_TURN_COMPLETE:
    case HU_OBSERVER_EVENT_CHANNEL_MESSAGE:
    case HU_OBSERVER_EVENT_HEARTBEAT_TICK:
        break;
    case HU_OBSERVER_EVENT_ERR:
        c->total_errors++;
        break;
    }
}

static void metrics_record_metric(void *ctx, const hu_observer_metric_t *metric) {
    hu_metrics_impl_ctx_t *c = (hu_metrics_impl_ctx_t *)ctx;
    if (!c)
        return;

    switch (metric->tag) {
    case HU_OBSERVER_METRIC_REQUEST_LATENCY_MS:
        c->latency_sum_ms += metric->value;
        c->latency_count++;
        break;
    case HU_OBSERVER_METRIC_TOKENS_USED:
        c->total_tokens += metric->value;
        break;
    case HU_OBSERVER_METRIC_ACTIVE_SESSIONS:
        c->active_sessions = metric->value;
        break;
    case HU_OBSERVER_METRIC_QUEUE_DEPTH:
        break;
    }
}

static void metrics_flush(void *ctx) {
    (void)ctx;
}

static const char *metrics_name(void *ctx) {
    (void)ctx;
    return "metrics";
}

static void metrics_deinit(void *ctx) {
    if (!ctx)
        return;
    hu_metrics_impl_ctx_t *c = (hu_metrics_impl_ctx_t *)ctx;
    hu_allocator_t *alloc = c->alloc;
    if (alloc && alloc->free)
        alloc->free(alloc->ctx, c, sizeof(*c));
}

static const hu_observer_vtable_t metrics_vtable = {
    .record_event = metrics_record_event,
    .record_metric = metrics_record_metric,
    .flush = metrics_flush,
    .name = metrics_name,
    .deinit = metrics_deinit,
};

hu_observer_t hu_metrics_observer_create(hu_allocator_t *alloc) {
    if (!alloc)
        return (hu_observer_t){.ctx = NULL, .vtable = NULL};

    hu_metrics_impl_ctx_t *ctx =
        (hu_metrics_impl_ctx_t *)alloc->alloc(alloc->ctx, sizeof(hu_metrics_impl_ctx_t));
    if (!ctx)
        return (hu_observer_t){.ctx = NULL, .vtable = NULL};

    memset(ctx, 0, sizeof(*ctx));
    ctx->alloc = alloc;

    return (hu_observer_t){.ctx = ctx, .vtable = &metrics_vtable};
}

void hu_metrics_observer_snapshot(hu_observer_t observer, hu_metrics_snapshot_t *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));

    if (!observer.ctx || !observer.vtable)
        return;
    if (observer.vtable->name && strcmp(observer.vtable->name(observer.ctx), "metrics") != 0)
        return;

    hu_metrics_impl_ctx_t *c = (hu_metrics_impl_ctx_t *)observer.ctx;
    out->total_requests = c->total_requests;
    out->total_tokens = c->total_tokens;
    out->total_tool_calls = c->total_tool_calls;
    out->total_errors = c->total_errors;
    out->active_sessions = c->active_sessions;
    out->avg_latency_ms =
        (c->latency_count > 0) ? (double)c->latency_sum_ms / (double)c->latency_count : 0.0;
}
