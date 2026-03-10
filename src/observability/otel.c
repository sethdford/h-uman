#include "human/observability/otel.h"
#include "human/core/http.h"
#include "human/core/string.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_ATTRS 32

typedef struct hu_otel_ctx {
    hu_allocator_t *alloc;
    char *endpoint;
    char *service_name;
    bool enable_traces;
    bool enable_metrics;
    bool enable_logs;
} hu_otel_ctx_t;

typedef struct hu_attr {
    char *key;
    char *str_val;
    int64_t int_val;
    double dbl_val;
    enum { ATTR_STR, ATTR_INT, ATTR_DBL } kind;
} hu_attr_t;

struct hu_span {
    hu_allocator_t *alloc;
    char *name;
    int64_t start_time;
    hu_attr_t attrs[MAX_ATTRS];
    size_t attr_count;
};

#if !HU_IS_TEST && defined(HU_HTTP_CURL)
static const char *event_tag_str(hu_observer_event_tag_t tag) {
    switch (tag) {
    case HU_OBSERVER_EVENT_AGENT_START:
        return "agent.start";
    case HU_OBSERVER_EVENT_LLM_REQUEST:
        return "llm.request";
    case HU_OBSERVER_EVENT_LLM_RESPONSE:
        return "llm.response";
    case HU_OBSERVER_EVENT_AGENT_END:
        return "agent.end";
    case HU_OBSERVER_EVENT_TOOL_CALL_START:
        return "tool.call.start";
    case HU_OBSERVER_EVENT_TOOL_CALL:
        return "tool.call";
    case HU_OBSERVER_EVENT_TOOL_ITERATIONS_EXHAUSTED:
        return "tool.iterations.exhausted";
    case HU_OBSERVER_EVENT_TURN_COMPLETE:
        return "turn.complete";
    case HU_OBSERVER_EVENT_CHANNEL_MESSAGE:
        return "channel.message";
    case HU_OBSERVER_EVENT_HEARTBEAT_TICK:
        return "heartbeat";
    case HU_OBSERVER_EVENT_ERR:
        return "error";
    default:
        return "unknown";
    }
}

static const char *metric_tag_str(hu_observer_metric_tag_t tag) {
    switch (tag) {
    case HU_OBSERVER_METRIC_REQUEST_LATENCY_MS:
        return "request.latency_ms";
    case HU_OBSERVER_METRIC_TOKENS_USED:
        return "tokens.used";
    case HU_OBSERVER_METRIC_ACTIVE_SESSIONS:
        return "sessions.active";
    case HU_OBSERVER_METRIC_QUEUE_DEPTH:
        return "queue.depth";
    default:
        return "unknown";
    }
}
#endif

static void otel_record_event(void *ctx, const hu_observer_event_t *event) {
    hu_otel_ctx_t *c = (hu_otel_ctx_t *)ctx;
    if (!c || !event || !c->enable_logs || !c->endpoint)
        return;
#if !HU_IS_TEST && defined(HU_HTTP_CURL)
    char body[2048];
    const char *svc = c->service_name ? c->service_name : "human";
    int n = snprintf(
        body, sizeof(body),
        "{\"resourceLogs\":[{\"resource\":{\"attributes\":[{\"key\":\"service.name\","
        "\"value\":{\"stringValue\":\"%s\"}}]},\"scopeLogs\":[{\"logRecords\":[{"
        "\"timeUnixNano\":\"%lld000000000\",\"body\":{\"stringValue\":\"%s\"},"
        "\"attributes\":[{\"key\":\"event.tag\",\"value\":{\"stringValue\":\"%s\"}}]}]}]}]}",
        svc, (long long)time(NULL), event_tag_str(event->tag), event_tag_str(event->tag));
    if (n <= 0 || (size_t)n >= sizeof(body))
        return;
    char url[512];
    snprintf(url, sizeof(url), "%s/v1/logs", c->endpoint);
    hu_http_response_t resp = {0};
    hu_error_t herr = hu_http_post_json(c->alloc, url, NULL, body, (size_t)n, &resp);
    (void)herr;
    if (resp.owned && resp.body)
        hu_http_response_free(c->alloc, &resp);
#else
    (void)c;
    (void)event;
#endif
}

static void otel_record_metric(void *ctx, const hu_observer_metric_t *metric) {
    hu_otel_ctx_t *c = (hu_otel_ctx_t *)ctx;
    if (!c || !metric || !c->enable_metrics || !c->endpoint)
        return;
#if !HU_IS_TEST && defined(HU_HTTP_CURL)
    char body[2048];
    const char *svc = c->service_name ? c->service_name : "human";
    int n = snprintf(
        body, sizeof(body),
        "{\"resourceMetrics\":[{\"resource\":{\"attributes\":[{\"key\":\"service.name\","
        "\"value\":{\"stringValue\":\"%s\"}}]},\"scopeMetrics\":[{\"metrics\":[{"
        "\"name\":\"%s\",\"gauge\":{\"dataPoints\":[{\"timeUnixNano\":\"%lld000000000\","
        "\"asInt\":\"%llu\"}]}}]}]}]}",
        svc, metric_tag_str(metric->tag), (long long)time(NULL), (unsigned long long)metric->value);
    if (n <= 0 || (size_t)n >= sizeof(body))
        return;
    char url[512];
    snprintf(url, sizeof(url), "%s/v1/metrics", c->endpoint);
    hu_http_response_t resp = {0};
    hu_error_t merr = hu_http_post_json(c->alloc, url, NULL, body, (size_t)n, &resp);
    (void)merr;
    if (resp.owned && resp.body)
        hu_http_response_free(c->alloc, &resp);
#else
    (void)c;
    (void)metric;
#endif
}

static void otel_flush(void *ctx) {
    (void)ctx;
}

static const char *otel_observer_name(void *ctx) {
    (void)ctx;
    return "otel";
}

static void otel_deinit(void *ctx) {
    hu_otel_ctx_t *c = (hu_otel_ctx_t *)ctx;
    if (!c || !c->alloc)
        return;
    hu_allocator_t *alloc = c->alloc;
    if (c->endpoint)
        alloc->free(alloc->ctx, c->endpoint, strlen(c->endpoint) + 1);
    if (c->service_name)
        alloc->free(alloc->ctx, c->service_name, strlen(c->service_name) + 1);
    alloc->free(alloc->ctx, c, sizeof(*c));
}

static const hu_observer_vtable_t otel_vtable = {
    .record_event = otel_record_event,
    .record_metric = otel_record_metric,
    .flush = otel_flush,
    .name = otel_observer_name,
    .deinit = otel_deinit,
};

hu_error_t hu_otel_observer_create(hu_allocator_t *alloc, const hu_otel_config_t *cfg,
                                   hu_observer_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_otel_ctx_t *c = (hu_otel_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    if (cfg) {
        c->endpoint = cfg->endpoint ? hu_strndup(alloc, cfg->endpoint, cfg->endpoint_len) : NULL;
        c->service_name =
            cfg->service_name ? hu_strndup(alloc, cfg->service_name, cfg->service_name_len) : NULL;
        c->enable_traces = cfg->enable_traces;
        c->enable_metrics = cfg->enable_metrics;
        c->enable_logs = cfg->enable_logs;
    }
    out->ctx = c;
    out->vtable = &otel_vtable;
    return HU_OK;
}

hu_span_t *hu_span_start(hu_allocator_t *alloc, const char *name, size_t name_len) {
    if (!alloc || !name)
        return NULL;
    hu_span_t *s = (hu_span_t *)alloc->alloc(alloc->ctx, sizeof(*s));
    if (!s)
        return NULL;
    memset(s, 0, sizeof(*s));
    s->alloc = alloc;
    s->name = hu_strndup(alloc, name, name_len);
    s->start_time = (int64_t)time(NULL);
    return s;
}

void hu_span_set_attr_str(hu_span_t *span, const char *key, const char *value) {
    if (!span || !key || span->attr_count >= MAX_ATTRS)
        return;
    hu_attr_t *a = &span->attrs[span->attr_count++];
    a->kind = ATTR_STR;
    a->key = hu_strndup(span->alloc, key, strlen(key));
    a->str_val = value ? hu_strndup(span->alloc, value, strlen(value)) : NULL;
}

void hu_span_set_attr_int(hu_span_t *span, const char *key, int64_t value) {
    if (!span || !key || span->attr_count >= MAX_ATTRS)
        return;
    hu_attr_t *a = &span->attrs[span->attr_count++];
    a->kind = ATTR_INT;
    a->key = hu_strndup(span->alloc, key, strlen(key));
    a->int_val = value;
}

void hu_span_set_attr_double(hu_span_t *span, const char *key, double value) {
    if (!span || !key || span->attr_count >= MAX_ATTRS)
        return;
    hu_attr_t *a = &span->attrs[span->attr_count++];
    a->kind = ATTR_DBL;
    a->key = hu_strndup(span->alloc, key, strlen(key));
    a->dbl_val = value;
}

hu_error_t hu_span_end(hu_span_t *span, hu_allocator_t *alloc) {
    if (!span || !alloc)
        return HU_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < span->attr_count; i++) {
        hu_attr_t *a = &span->attrs[i];
        if (a->key)
            alloc->free(alloc->ctx, a->key, strlen(a->key) + 1);
        if (a->kind == ATTR_STR && a->str_val)
            alloc->free(alloc->ctx, a->str_val, strlen(a->str_val) + 1);
    }
    if (span->name)
        alloc->free(alloc->ctx, span->name, strlen(span->name) + 1);
    alloc->free(alloc->ctx, span, sizeof(*span));
    return HU_OK;
}
