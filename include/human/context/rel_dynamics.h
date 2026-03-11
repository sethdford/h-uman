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
    float velocity;             /* computed */
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

/* Build SQL to create the relationship_dynamics table */
hu_error_t hu_rel_dynamics_create_table_sql(char *buf, size_t cap, size_t *out_len);

/* Build SQL to record a velocity measurement */
hu_error_t hu_rel_dynamics_insert_sql(const hu_rel_velocity_t *vel, uint64_t timestamp_ms,
                                       char *buf, size_t cap, size_t *out_len);

/* Build SQL to fetch history for a contact */
hu_error_t hu_rel_dynamics_query_sql(const char *contact_id, size_t contact_id_len,
                                      size_t limit, char *buf, size_t cap, size_t *out_len);

/* Compute velocity from metrics; mutates vel->velocity and vel->trend */
float hu_rel_velocity_compute(hu_rel_velocity_t *vel);

/* Classify trend from current and previous velocity */
hu_rel_trend_t hu_rel_trend_classify(float velocity, float prev_velocity);

/* Detect drift from 2+ consecutive negative velocity readings.
   measurements ordered oldest first; count >= 2. */
bool hu_drift_detect(const hu_rel_velocity_t *measurements, size_t count,
                     hu_drift_signal_t *signal);

/* Returns true when repair should activate (drift + low interaction quality) */
bool hu_repair_should_activate(const hu_drift_signal_t *signal, float interaction_quality);

/* Budget multiplier for governor integration */
float hu_rel_dynamics_budget_multiplier(hu_rel_trend_t trend);

/* Build prompt context from velocity, drift signal, and repair state.
   Allocates *out. Caller frees. */
hu_error_t hu_rel_dynamics_build_prompt(hu_allocator_t *alloc, const hu_rel_velocity_t *vel,
                                        const hu_drift_signal_t *signal,
                                        const hu_repair_state_t *repair, char **out,
                                        size_t *out_len);

void hu_rel_velocity_deinit(hu_allocator_t *alloc, hu_rel_velocity_t *vel);
void hu_repair_state_deinit(hu_allocator_t *alloc, hu_repair_state_t *state);

const char *hu_rel_trend_str(hu_rel_trend_t trend);

#endif
