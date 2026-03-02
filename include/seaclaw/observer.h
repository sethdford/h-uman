#ifndef SC_OBSERVER_H
#define SC_OBSERVER_H

#include "core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum sc_observer_event_tag {
    SC_OBSERVER_EVENT_AGENT_START,
    SC_OBSERVER_EVENT_LLM_REQUEST,
    SC_OBSERVER_EVENT_LLM_RESPONSE,
    SC_OBSERVER_EVENT_AGENT_END,
    SC_OBSERVER_EVENT_TOOL_CALL_START,
    SC_OBSERVER_EVENT_TOOL_CALL,
    SC_OBSERVER_EVENT_TOOL_ITERATIONS_EXHAUSTED,
    SC_OBSERVER_EVENT_TURN_COMPLETE,
    SC_OBSERVER_EVENT_CHANNEL_MESSAGE,
    SC_OBSERVER_EVENT_HEARTBEAT_TICK,
    SC_OBSERVER_EVENT_ERR,
} sc_observer_event_tag_t;

typedef struct sc_observer_event {
    sc_observer_event_tag_t tag;
    union {
        struct { const char *provider; const char *model; } agent_start;
        struct { const char *provider; const char *model; size_t messages_count; } llm_request;
        struct { const char *provider; const char *model; uint64_t duration_ms; bool success; const char *error_message; } llm_response;
        struct { uint64_t duration_ms; uint64_t tokens_used; } agent_end;
        struct { const char *tool; } tool_call_start;
        struct { const char *tool; uint64_t duration_ms; bool success; const char *detail; } tool_call;
        struct { uint32_t iterations; } tool_iterations_exhausted;
        struct { const char *channel; const char *direction; } channel_message;
        struct { const char *component; const char *message; } err;
    } data;
} sc_observer_event_t;

typedef enum sc_observer_metric_tag {
    SC_OBSERVER_METRIC_REQUEST_LATENCY_MS,
    SC_OBSERVER_METRIC_TOKENS_USED,
    SC_OBSERVER_METRIC_ACTIVE_SESSIONS,
    SC_OBSERVER_METRIC_QUEUE_DEPTH,
} sc_observer_metric_tag_t;

typedef struct sc_observer_metric {
    sc_observer_metric_tag_t tag;
    uint64_t value;
} sc_observer_metric_t;

struct sc_observer_vtable;

typedef struct sc_observer {
    void *ctx;
    const struct sc_observer_vtable *vtable;
} sc_observer_t;

typedef struct sc_observer_vtable {
    void (*record_event)(void *ctx, const sc_observer_event_t *event);
    void (*record_metric)(void *ctx, const sc_observer_metric_t *metric);
    void (*flush)(void *ctx);
    const char *(*name)(void *ctx);
    void (*deinit)(void *ctx);
} sc_observer_vtable_t;

static inline void sc_observer_record_event(sc_observer_t obs, const sc_observer_event_t *event) {
    if (obs.vtable && obs.vtable->record_event)
        obs.vtable->record_event(obs.ctx, event);
}

static inline void sc_observer_record_metric(sc_observer_t obs, const sc_observer_metric_t *metric) {
    if (obs.vtable && obs.vtable->record_metric)
        obs.vtable->record_metric(obs.ctx, metric);
}

static inline void sc_observer_flush(sc_observer_t obs) {
    if (obs.vtable && obs.vtable->flush)
        obs.vtable->flush(obs.ctx);
}

static inline const char *sc_observer_name(sc_observer_t obs) {
    if (obs.vtable && obs.vtable->name)
        return obs.vtable->name(obs.ctx);
    return "none";
}

/* ── Concrete observers ───────────────────────────────────────────── */

/** Noop observer — all methods no-op. */
sc_observer_t sc_observer_noop(void);

/** Log observer — writes events to stderr or FILE. */
sc_observer_t sc_observer_log_create(FILE *output);
sc_observer_t sc_observer_log_stderr(void);

/** Metrics observer — tracks counters and histograms. */
typedef struct sc_metrics_observer_ctx {
    uint64_t request_latency_ms;
    uint64_t tokens_used;
    uint64_t active_sessions;
    uint64_t queue_depth;
} sc_metrics_observer_ctx_t;
sc_observer_t sc_observer_metrics_create(sc_metrics_observer_ctx_t *ctx);
uint64_t sc_observer_metrics_get(sc_metrics_observer_ctx_t *ctx,
    sc_observer_metric_tag_t tag);

/** Composite observer — fans out to multiple observers. */
typedef struct sc_composite_observer_ctx {
    sc_observer_t *observers;
    size_t count;
} sc_composite_observer_ctx_t;
sc_observer_t sc_observer_composite_create(sc_composite_observer_ctx_t *ctx,
    sc_observer_t *observers, size_t count);

/** Registry — create observer from backend string (log, verbose, noop, none). */
sc_observer_t sc_observer_registry_create(const char *backend, void *user_ctx);

#endif /* SC_OBSERVER_H */
