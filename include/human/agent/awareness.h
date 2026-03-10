#ifndef HU_AGENT_AWARENESS_H
#define HU_AGENT_AWARENESS_H

#include "human/bus.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Proactive awareness tracks bus events and builds situational context
 * that the agent can inject into its prompt — e.g. recent errors,
 * health state changes, active channels, tool usage patterns. */

#define HU_AWARENESS_MAX_RECENT_ERRORS   5
#define HU_AWARENESS_MAX_ACTIVE_CHANNELS 8
#define HU_AWARENESS_CHANNEL_NAME_LEN    32

typedef struct hu_awareness_state {
    /* Recent errors (circular buffer) */
    struct {
        char text[HU_BUS_MSG_LEN];
        uint64_t timestamp_ms;
    } recent_errors[HU_AWARENESS_MAX_RECENT_ERRORS];
    size_t error_write_idx;
    size_t total_errors;

    /* Active channels seen */
    char active_channels[HU_AWARENESS_MAX_ACTIVE_CHANNELS][HU_AWARENESS_CHANNEL_NAME_LEN];
    size_t active_channel_count;

    /* Counters */
    uint64_t messages_received;
    uint64_t messages_sent;
    uint64_t tool_calls;
    bool health_degraded;
} hu_awareness_state_t;

typedef struct hu_awareness {
    hu_awareness_state_t state;
    hu_bus_t *bus;
} hu_awareness_t;

/* Initialize and subscribe to bus events. */
hu_error_t hu_awareness_init(hu_awareness_t *aw, hu_bus_t *bus);

/* Unsubscribe from bus. */
void hu_awareness_deinit(hu_awareness_t *aw);

/* Build a formatted context string from current awareness state.
 * Caller owns the returned string. Returns NULL if nothing notable. */
char *hu_awareness_context(const hu_awareness_t *aw, hu_allocator_t *alloc, size_t *out_len);

#endif /* HU_AGENT_AWARENESS_H */
