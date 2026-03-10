#ifndef HU_METRICS_OBSERVER_H
#define HU_METRICS_OBSERVER_H

#include "human/core/allocator.h"
#include "human/observer.h"
#include <stddef.h>
#include <stdint.h>

typedef struct hu_metrics_snapshot {
    uint64_t total_requests;
    uint64_t total_tokens;
    uint64_t total_tool_calls;
    uint64_t total_errors;
    double avg_latency_ms;
    uint64_t active_sessions;
} hu_metrics_snapshot_t;

/**
 * Create a metrics observer that tracks counters from events.
 * Caller must call hu_observer's deinit when done; ctx is allocated via alloc.
 */
hu_observer_t hu_metrics_observer_create(hu_allocator_t *alloc);

/**
 * Read current metrics into snapshot. Observer must be a metrics observer.
 */
void hu_metrics_observer_snapshot(hu_observer_t observer, hu_metrics_snapshot_t *out);

#endif /* HU_METRICS_OBSERVER_H */
