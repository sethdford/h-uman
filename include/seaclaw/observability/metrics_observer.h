#ifndef SC_METRICS_OBSERVER_H
#define SC_METRICS_OBSERVER_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/observer.h"
#include <stddef.h>
#include <stdint.h>

typedef struct sc_metrics_snapshot {
    uint64_t total_requests;
    uint64_t total_tokens;
    uint64_t total_tool_calls;
    uint64_t total_errors;
    double avg_latency_ms;
    uint64_t active_sessions;
} sc_metrics_snapshot_t;

/**
 * Create a metrics observer that tracks counters from events.
 * Caller must call sc_observer's deinit when done; ctx is allocated via alloc.
 */
sc_observer_t sc_metrics_observer_create(sc_allocator_t *alloc);

/**
 * Read current metrics into snapshot. Observer must be a metrics observer.
 */
void sc_metrics_observer_snapshot(sc_observer_t observer, sc_metrics_snapshot_t *out);

#endif /* SC_METRICS_OBSERVER_H */
