#ifndef HU_OBSERVER_H
#define HU_OBSERVER_H

#include "core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum hu_observer_event_tag {
    HU_OBSERVER_EVENT_AGENT_START,
    HU_OBSERVER_EVENT_LLM_REQUEST,
    HU_OBSERVER_EVENT_LLM_RESPONSE,
    HU_OBSERVER_EVENT_AGENT_END,
    HU_OBSERVER_EVENT_TOOL_CALL_START,
    HU_OBSERVER_EVENT_TOOL_CALL,
    HU_OBSERVER_EVENT_TOOL_ITERATIONS_EXHAUSTED,
    HU_OBSERVER_EVENT_TURN_COMPLETE,
    HU_OBSERVER_EVENT_CHANNEL_MESSAGE,
    HU_OBSERVER_EVENT_HEARTBEAT_TICK,
    HU_OBSERVER_EVENT_ERR,
    HU_OBSERVER_EVENT_COGNITION_MODE,
    HU_OBSERVER_EVENT_METACOG_ACTION,
    HU_OBSERVER_EVENT_HULA_NODE_START,
    HU_OBSERVER_EVENT_HULA_NODE_END,
    HU_OBSERVER_EVENT_HULA_NODE_OUTPUT,
    HU_OBSERVER_EVENT_HULA_PROGRAM_END,
    HU_OBSERVER_EVENT_FRONTIER,
    /* Emitted after hu_output_validator_chain_execute for REJECT or REWRITE
     * outcomes only. PASS outcomes are suppressed to avoid per-token noise.
     * All const char * fields are non-owning; caller guarantees lifetime
     * through the synchronous record_event call. */
    HU_OBSERVER_EVENT_VALIDATOR_DECISION,
} hu_observer_event_tag_t;

typedef struct hu_observer_event {
    hu_observer_event_tag_t tag;
    const char *trace_id; /* NULL if no trace context; points to agent->trace_id */
    union {
        struct {
            const char *provider;
            const char *model;
        } agent_start;
        struct {
            const char *provider;
            const char *model;
            size_t messages_count;
        } llm_request;
        struct {
            const char *provider;
            const char *model;
            uint64_t duration_ms;
            bool success;
            const char *error_message;
        } llm_response;
        struct {
            uint64_t duration_ms;
            uint64_t tokens_used;
        } agent_end;
        struct {
            const char *tool;
        } tool_call_start;
        struct {
            const char *tool;
            uint64_t duration_ms;
            bool success;
            const char *detail;
        } tool_call;
        struct {
            uint32_t iterations;
        } tool_iterations_exhausted;
        struct {
            const char *channel;
            const char *direction;
        } channel_message;
        struct {
            const char *component;
            const char *message;
        } err;
        struct {
            const char *mode; /* "fast", "slow", "emotional" */
        } cognition_mode;
        struct {
            const char *action; /* from hu_metacog_action_name */
            float confidence;
            float coherence;
        } metacog_action;
        struct {
            const char *node_id;
            const char *op_name;
        } hula_node_start;
        struct {
            const char *node_id;
            const char *op_name;
            const char *status;
            uint64_t elapsed_ms;
        } hula_node_end;
        struct {
            const char *node_id;
            const char *output;
            size_t output_len;
        } hula_node_output;
        struct {
            const char *program_name;
            bool success;
            uint64_t total_ms;
            size_t node_count;
        } hula_program_end;
        struct {
            const char *frontier;
            const char *transition;
            float value;
        } frontier;
        struct {
            /* "reject" or "rewrite" — PASS is never emitted */
            const char *decision;
            /* Name of the deciding validator, or "<chain>" if unavailable */
            const char *validator_name;
            /* Channel context from hu_validator_context_t (may be NULL) */
            const char *channel_id;
            /* Persona name from hu_validator_context_t (may be NULL) */
            const char *persona_name;
            /* Original response_len passed to hu_output_validator_chain_execute */
            size_t response_len;
            /* Bytes stripped for REWRITE; 0 for REJECT */
            size_t bytes_stripped;
        } validator_decision;
    } data;
} hu_observer_event_t;

typedef enum hu_observer_metric_tag {
    HU_OBSERVER_METRIC_REQUEST_LATENCY_MS,
    HU_OBSERVER_METRIC_TOKENS_USED,
    HU_OBSERVER_METRIC_ACTIVE_SESSIONS,
    HU_OBSERVER_METRIC_QUEUE_DEPTH,
} hu_observer_metric_tag_t;

typedef struct hu_observer_metric {
    hu_observer_metric_tag_t tag;
    uint64_t value;
} hu_observer_metric_t;

struct hu_observer_vtable;

typedef struct hu_observer {
    void *ctx;
    const struct hu_observer_vtable *vtable;
} hu_observer_t;

typedef struct hu_observer_vtable {
    void (*record_event)(void *ctx, const hu_observer_event_t *event);
    void (*record_metric)(void *ctx, const hu_observer_metric_t *metric);
    void (*flush)(void *ctx);
    const char *(*name)(void *ctx);
    void (*deinit)(void *ctx);
} hu_observer_vtable_t;

static inline void hu_observer_record_event(hu_observer_t obs, const hu_observer_event_t *event) {
    if (obs.vtable && obs.vtable->record_event)
        obs.vtable->record_event(obs.ctx, event);
}

static inline void hu_observer_record_metric(hu_observer_t obs,
                                             const hu_observer_metric_t *metric) {
    if (obs.vtable && obs.vtable->record_metric)
        obs.vtable->record_metric(obs.ctx, metric);
}

static inline void hu_observer_flush(hu_observer_t obs) {
    if (obs.vtable && obs.vtable->flush)
        obs.vtable->flush(obs.ctx);
}

static inline const char *hu_observer_name(hu_observer_t obs) {
    if (obs.vtable && obs.vtable->name)
        return obs.vtable->name(obs.ctx);
    return "none";
}

/* ── Concrete observers ───────────────────────────────────────────── */

/** Noop observer — all methods no-op. */
hu_observer_t hu_observer_noop(void);

/** Log observer — writes events to stderr or FILE. */
hu_observer_t hu_observer_log_create(FILE *output);
hu_observer_t hu_observer_log_stderr(void);

/** Metrics observer — tracks counters and histograms. */
typedef struct hu_metrics_observer_ctx {
    uint64_t request_latency_ms;
    uint64_t tokens_used;
    uint64_t active_sessions;
    uint64_t queue_depth;
} hu_metrics_observer_ctx_t;
hu_observer_t hu_observer_metrics_create(hu_metrics_observer_ctx_t *ctx);
uint64_t hu_observer_metrics_get(hu_metrics_observer_ctx_t *ctx, hu_observer_metric_tag_t tag);

/** Composite observer — fans out to multiple observers. */
typedef struct hu_composite_observer_ctx {
    hu_observer_t *observers;
    size_t count;
} hu_composite_observer_ctx_t;
hu_observer_t hu_observer_composite_create(hu_composite_observer_ctx_t *ctx,
                                           hu_observer_t *observers, size_t count);

/** Registry — create observer from backend string (log, verbose, noop, none). */
hu_observer_t hu_observer_registry_create(const char *backend, void *user_ctx);

#endif /* HU_OBSERVER_H */
