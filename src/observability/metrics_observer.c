#include "seaclaw/observability/metrics_observer.h"
#include "seaclaw/core/error.h"
#include "seaclaw/observer.h"
#include <stdint.h>
#include <string.h>

typedef struct sc_metrics_impl_ctx {
    sc_allocator_t *alloc;
    uint64_t total_requests;
    uint64_t total_tokens;
    uint64_t total_tool_calls;
    uint64_t total_errors;
    uint64_t latency_sum_ms;
    uint64_t latency_count;
    uint64_t active_sessions;
} sc_metrics_impl_ctx_t;

static void metrics_record_event(void *ctx, const sc_observer_event_t *event) {
    sc_metrics_impl_ctx_t *c = (sc_metrics_impl_ctx_t *)ctx;
    if (!c)
        return;

    switch (event->tag) {
    case SC_OBSERVER_EVENT_AGENT_START:
        c->active_sessions++;
        c->total_requests++;
        break;
    case SC_OBSERVER_EVENT_LLM_REQUEST:
        /* Request counted at agent_start */
        break;
    case SC_OBSERVER_EVENT_LLM_RESPONSE:
        c->latency_sum_ms += event->data.llm_response.duration_ms;
        c->latency_count++;
        if (!event->data.llm_response.success)
            c->total_errors++;
        break;
    case SC_OBSERVER_EVENT_AGENT_END:
        c->total_tokens += event->data.agent_end.tokens_used;
        if (c->active_sessions > 0)
            c->active_sessions--;
        break;
    case SC_OBSERVER_EVENT_TOOL_CALL_START:
        break;
    case SC_OBSERVER_EVENT_TOOL_CALL:
        c->total_tool_calls++;
        if (!event->data.tool_call.success)
            c->total_errors++;
        break;
    case SC_OBSERVER_EVENT_TOOL_ITERATIONS_EXHAUSTED:
    case SC_OBSERVER_EVENT_TURN_COMPLETE:
    case SC_OBSERVER_EVENT_CHANNEL_MESSAGE:
    case SC_OBSERVER_EVENT_HEARTBEAT_TICK:
        break;
    case SC_OBSERVER_EVENT_ERR:
        c->total_errors++;
        break;
    }
}

static void metrics_record_metric(void *ctx, const sc_observer_metric_t *metric) {
    sc_metrics_impl_ctx_t *c = (sc_metrics_impl_ctx_t *)ctx;
    if (!c)
        return;

    switch (metric->tag) {
    case SC_OBSERVER_METRIC_REQUEST_LATENCY_MS:
        c->latency_sum_ms += metric->value;
        c->latency_count++;
        break;
    case SC_OBSERVER_METRIC_TOKENS_USED:
        c->total_tokens += metric->value;
        break;
    case SC_OBSERVER_METRIC_ACTIVE_SESSIONS:
        c->active_sessions = metric->value;
        break;
    case SC_OBSERVER_METRIC_QUEUE_DEPTH:
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
    sc_metrics_impl_ctx_t *c = (sc_metrics_impl_ctx_t *)ctx;
    sc_allocator_t *alloc = c->alloc;
    if (alloc && alloc->free)
        alloc->free(alloc->ctx, c, sizeof(*c));
}

static const sc_observer_vtable_t metrics_vtable = {
    .record_event = metrics_record_event,
    .record_metric = metrics_record_metric,
    .flush = metrics_flush,
    .name = metrics_name,
    .deinit = metrics_deinit,
};

sc_observer_t sc_metrics_observer_create(sc_allocator_t *alloc) {
    if (!alloc)
        return (sc_observer_t){.ctx = NULL, .vtable = NULL};

    sc_metrics_impl_ctx_t *ctx =
        (sc_metrics_impl_ctx_t *)alloc->alloc(alloc->ctx, sizeof(sc_metrics_impl_ctx_t));
    if (!ctx)
        return (sc_observer_t){.ctx = NULL, .vtable = NULL};

    memset(ctx, 0, sizeof(*ctx));
    ctx->alloc = alloc;

    return (sc_observer_t){.ctx = ctx, .vtable = &metrics_vtable};
}

void sc_metrics_observer_snapshot(sc_observer_t observer, sc_metrics_snapshot_t *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));

    if (!observer.ctx || !observer.vtable)
        return;
    if (observer.vtable->name && strcmp(observer.vtable->name(observer.ctx), "metrics") != 0)
        return;

    sc_metrics_impl_ctx_t *c = (sc_metrics_impl_ctx_t *)observer.ctx;
    out->total_requests = c->total_requests;
    out->total_tokens = c->total_tokens;
    out->total_tool_calls = c->total_tool_calls;
    out->total_errors = c->total_errors;
    out->active_sessions = c->active_sessions;
    out->avg_latency_ms =
        (c->latency_count > 0) ? (double)c->latency_sum_ms / (double)c->latency_count : 0.0;
}
