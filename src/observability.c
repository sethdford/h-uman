#include "human/core/error.h"
#include "human/observer.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HU_OBSERVER_DETAIL_MAX 256
#define HU_STR(s)              ((s) ? (s) : "")

static void noop_record_event(void *ctx, const hu_observer_event_t *event) {
    (void)ctx;
    (void)event;
}
static void noop_record_metric(void *ctx, const hu_observer_metric_t *metric) {
    (void)ctx;
    (void)metric;
}
static void noop_flush(void *ctx) {
    (void)ctx;
}
static const char *noop_name(void *ctx) {
    (void)ctx;
    return "noop";
}
static void noop_deinit(void *ctx) {
    (void)ctx;
}

static const hu_observer_vtable_t noop_vtable = {
    .record_event = noop_record_event,
    .record_metric = noop_record_metric,
    .flush = noop_flush,
    .name = noop_name,
    .deinit = noop_deinit,
};

static hu_observer_t noop_singleton = {.ctx = NULL, .vtable = &noop_vtable};

hu_observer_t hu_observer_noop(void) {
    return noop_singleton;
}

typedef struct {
    FILE *output;
} hu_log_observer_ctx_t;

static const char *detail_truncate(const char *detail) {
    if (!detail || !detail[0])
        return NULL;
    static char buf[HU_OBSERVER_DETAIL_MAX + 1];
    size_t len = strlen(detail);
    if (len > HU_OBSERVER_DETAIL_MAX)
        len = HU_OBSERVER_DETAIL_MAX;
    memcpy(buf, detail, len);
    buf[len] = '\0';
    return buf;
}

static void log_record_event(void *ctx, const hu_observer_event_t *event) {
    FILE *f =
        ((hu_log_observer_ctx_t *)ctx)->output ? ((hu_log_observer_ctx_t *)ctx)->output : stderr;
    if (event->trace_id && event->trace_id[0])
        fprintf(f, "[trace_id=%s] ", event->trace_id);
    switch (event->tag) {
    case HU_OBSERVER_EVENT_AGENT_START:
        fprintf(f, "agent.start provider=%s model=%s\n", HU_STR(event->data.agent_start.provider),
                HU_STR(event->data.agent_start.model));
        break;
    case HU_OBSERVER_EVENT_LLM_REQUEST:
        fprintf(f, "llm.request provider=%s model=%s messages=%zu\n",
                HU_STR(event->data.llm_request.provider), HU_STR(event->data.llm_request.model),
                event->data.llm_request.messages_count);
        break;
    case HU_OBSERVER_EVENT_LLM_RESPONSE:
        fprintf(f, "llm.response duration_ms=%llu success=%s\n",
                (unsigned long long)event->data.llm_response.duration_ms,
                event->data.llm_response.success ? "true" : "false");
        break;
    case HU_OBSERVER_EVENT_AGENT_END:
        fprintf(f, "agent.end duration_ms=%llu\n",
                (unsigned long long)event->data.agent_end.duration_ms);
        break;
    case HU_OBSERVER_EVENT_TOOL_CALL_START:
        fprintf(f, "tool.start tool=%s\n", HU_STR(event->data.tool_call_start.tool));
        break;
    case HU_OBSERVER_EVENT_TOOL_CALL: {
        const char *d = detail_truncate(event->data.tool_call.detail);
        if (d)
            fprintf(f, "tool.call tool=%s duration_ms=%llu success=%s detail=%s\n",
                    HU_STR(event->data.tool_call.tool),
                    (unsigned long long)event->data.tool_call.duration_ms,
                    event->data.tool_call.success ? "true" : "false", d);
        else
            fprintf(f, "tool.call tool=%s duration_ms=%llu success=%s\n",
                    HU_STR(event->data.tool_call.tool),
                    (unsigned long long)event->data.tool_call.duration_ms,
                    event->data.tool_call.success ? "true" : "false");
        break;
    }
    case HU_OBSERVER_EVENT_TOOL_ITERATIONS_EXHAUSTED:
        fprintf(f, "tool.iterations_exhausted iterations=%u\n",
                event->data.tool_iterations_exhausted.iterations);
        break;
    case HU_OBSERVER_EVENT_TURN_COMPLETE:
        fprintf(f, "turn.complete\n");
        break;
    case HU_OBSERVER_EVENT_CHANNEL_MESSAGE:
        fprintf(f, "channel.message channel=%s direction=%s\n",
                HU_STR(event->data.channel_message.channel),
                HU_STR(event->data.channel_message.direction));
        break;
    case HU_OBSERVER_EVENT_HEARTBEAT_TICK:
        fprintf(f, "heartbeat.tick\n");
        break;
    case HU_OBSERVER_EVENT_ERR:
        fprintf(f, "error component=%s message=%s\n", HU_STR(event->data.err.component),
                HU_STR(event->data.err.message));
        break;
    }
}

static void log_record_metric(void *ctx, const hu_observer_metric_t *metric) {
    FILE *f =
        ((hu_log_observer_ctx_t *)ctx)->output ? ((hu_log_observer_ctx_t *)ctx)->output : stderr;
    const char *name = "metric";
    switch (metric->tag) {
    case HU_OBSERVER_METRIC_REQUEST_LATENCY_MS:
        name = "request_latency_ms";
        break;
    case HU_OBSERVER_METRIC_TOKENS_USED:
        name = "tokens_used";
        break;
    case HU_OBSERVER_METRIC_ACTIVE_SESSIONS:
        name = "active_sessions";
        break;
    case HU_OBSERVER_METRIC_QUEUE_DEPTH:
        name = "queue_depth";
        break;
    }
    fprintf(f, "metric.%s value=%llu\n", name, (unsigned long long)metric->value);
}

static void log_flush(void *ctx) {
    (void)ctx;
}
static const char *log_name(void *ctx) {
    (void)ctx;
    return "log";
}
static void log_deinit(void *ctx) {
    (void)ctx;
}

static const hu_observer_vtable_t log_vtable = {
    .record_event = log_record_event,
    .record_metric = log_record_metric,
    .flush = log_flush,
    .name = log_name,
    .deinit = log_deinit,
};

hu_observer_t hu_observer_log_create(FILE *output) {
    static hu_log_observer_ctx_t static_ctx = {.output = NULL};
    static_ctx.output = output;
    return (hu_observer_t){.ctx = &static_ctx, .vtable = &log_vtable};
}
hu_observer_t hu_observer_log_stderr(void) {
    return hu_observer_log_create(NULL);
}

static void metrics_record_event(void *ctx, const hu_observer_event_t *event) {
    (void)ctx;
    (void)event;
}
static void metrics_record_metric(void *ctx, const hu_observer_metric_t *metric) {
    hu_metrics_observer_ctx_t *c = (hu_metrics_observer_ctx_t *)ctx;
    switch (metric->tag) {
    case HU_OBSERVER_METRIC_REQUEST_LATENCY_MS:
        c->request_latency_ms = metric->value;
        break;
    case HU_OBSERVER_METRIC_TOKENS_USED:
        c->tokens_used += metric->value;
        break;
    case HU_OBSERVER_METRIC_ACTIVE_SESSIONS:
        c->active_sessions = metric->value;
        break;
    case HU_OBSERVER_METRIC_QUEUE_DEPTH:
        c->queue_depth = metric->value;
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
    (void)ctx;
}

static const hu_observer_vtable_t metrics_vtable = {
    .record_event = metrics_record_event,
    .record_metric = metrics_record_metric,
    .flush = metrics_flush,
    .name = metrics_name,
    .deinit = metrics_deinit,
};

hu_observer_t hu_observer_metrics_create(hu_metrics_observer_ctx_t *ctx) {
    if (ctx)
        memset(ctx, 0, sizeof(*ctx));
    return (hu_observer_t){.ctx = ctx, .vtable = &metrics_vtable};
}

uint64_t hu_observer_metrics_get(hu_metrics_observer_ctx_t *ctx, hu_observer_metric_tag_t tag) {
    if (!ctx)
        return 0;
    switch (tag) {
    case HU_OBSERVER_METRIC_REQUEST_LATENCY_MS:
        return ctx->request_latency_ms;
    case HU_OBSERVER_METRIC_TOKENS_USED:
        return ctx->tokens_used;
    case HU_OBSERVER_METRIC_ACTIVE_SESSIONS:
        return ctx->active_sessions;
    case HU_OBSERVER_METRIC_QUEUE_DEPTH:
        return ctx->queue_depth;
    }
    return 0;
}

static void composite_record_event(void *ctx, const hu_observer_event_t *event) {
    hu_composite_observer_ctx_t *c = (hu_composite_observer_ctx_t *)ctx;
    for (size_t i = 0; i < c->count; i++)
        hu_observer_record_event(c->observers[i], event);
}
static void composite_record_metric(void *ctx, const hu_observer_metric_t *metric) {
    hu_composite_observer_ctx_t *c = (hu_composite_observer_ctx_t *)ctx;
    for (size_t i = 0; i < c->count; i++)
        hu_observer_record_metric(c->observers[i], metric);
}
static void composite_flush(void *ctx) {
    hu_composite_observer_ctx_t *c = (hu_composite_observer_ctx_t *)ctx;
    for (size_t i = 0; i < c->count; i++)
        hu_observer_flush(c->observers[i]);
}
static const char *composite_name(void *ctx) {
    (void)ctx;
    return "multi";
}
static void composite_deinit(void *ctx) {
    (void)ctx;
}

static const hu_observer_vtable_t composite_vtable = {
    .record_event = composite_record_event,
    .record_metric = composite_record_metric,
    .flush = composite_flush,
    .name = composite_name,
    .deinit = composite_deinit,
};

hu_observer_t hu_observer_composite_create(hu_composite_observer_ctx_t *ctx,
                                           hu_observer_t *observers, size_t count) {
    ctx->observers = observers;
    ctx->count = count;
    return (hu_observer_t){.ctx = ctx, .vtable = &composite_vtable};
}

hu_observer_t hu_observer_registry_create(const char *backend, void *user_ctx) {
    (void)user_ctx;
    if (!backend)
        return hu_observer_noop();
    if (strcmp(backend, "log") == 0 || strcmp(backend, "verbose") == 0)
        return hu_observer_log_stderr();
    if (strcmp(backend, "noop") == 0 || strcmp(backend, "none") == 0)
        return hu_observer_noop();
    return hu_observer_noop();
}
