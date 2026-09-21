#ifndef HU_CONTEXT_REL_DYNAMICS_H
#define HU_CONTEXT_REL_DYNAMICS_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_rel_trend {
    HU_REL_TREND_DEEPENING = 0,
    HU_REL_TREND_STABLE,
    HU_REL_TREND_COOLING,
    HU_REL_TREND_STRAINED,
    HU_REL_TREND_REPAIR
} hu_rel_trend_t;

typedef struct hu_rel_velocity {
    const char *contact_id;
    size_t contact_id_len;
    uint32_t messages_sent_30d;
    uint32_t messages_received_30d;
    uint32_t initiations_sent_30d;
    uint32_t initiations_received_30d;
    uint64_t avg_response_time_ms;
    float interaction_quality; /* -1.0..1.0 */
    float velocity;            /* computed */
    hu_rel_trend_t trend;
} hu_rel_velocity_t;

typedef struct hu_drift_signal {
    const char *contact_id;
    size_t contact_id_len;
    uint32_t consecutive_negative_periods;
    float last_velocity;
    float current_velocity;
    bool is_drifting;
} hu_drift_signal_t;

typedef struct hu_repair_state {
    char *contact_id;
    size_t contact_id_len;
    bool active;
    uint64_t started_ms;
    char *reason;
    size_t reason_len;
    bool reduced_initiative;
    bool warmer_tone;
} hu_repair_state_t;

/* Compute velocity from metrics; mutates vel->velocity and vel->trend */
float hu_rel_velocity_compute(hu_rel_velocity_t *vel);

/* Classify trend from current and previous velocity */
hu_rel_trend_t hu_rel_trend_classify(float velocity, float prev_velocity);

/* Build prompt context from velocity, drift signal, and repair state.
   Allocates *out. Caller frees. */
hu_error_t hu_rel_dynamics_build_prompt(hu_allocator_t *alloc, const hu_rel_velocity_t *vel,
                                        const hu_drift_signal_t *signal,
                                        const hu_repair_state_t *repair, char **out,
                                        size_t *out_len);

const char *hu_rel_trend_str(hu_rel_trend_t trend);

#endif
