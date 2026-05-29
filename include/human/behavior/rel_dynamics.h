#ifndef HU_AGENT_REL_DYNAMICS_H
#define HU_AGENT_REL_DYNAMICS_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_rel_mode {
    HU_REL_NORMAL = 0,
    HU_REL_DEEPENING,
    HU_REL_DRIFTING,
    HU_REL_REPAIR,
    HU_REL_RECONNECTING
} hu_rel_mode_t;

typedef struct hu_rel_signals {
    double frequency_delta;       /* -1 to 1: change in message frequency */
    double initiation_delta;      /* -1 to 1: change in who initiates */
    double response_time_delta;   /* -1 to 1: change in response speed */
    double msg_length_delta;      /* -1 to 1: change in message length */
    double vulnerability_delta;   /* -1 to 1: change in conversation depth */
    double plan_completion_rate;  /* 0 to 1: plans proposed vs completed */
    double sentiment_delta;      /* -1 to 1: change in positive sentiment */
    double topic_diversity_delta; /* -1 to 1: change in topic range */
} hu_rel_signals_t;

typedef struct hu_rel_state {
    char *contact_id;
    size_t contact_id_len;
    double closeness;             /* 0.0 (stranger) to 1.0 (intimate) */
    double velocity;              /* positive = deepening, negative = drifting */
    double vulnerability_depth;   /* 0.0-1.0: how deep conversations go */
    double reciprocity;          /* -1.0 to 1.0: 0=balanced */
    uint64_t last_interaction;
    uint64_t last_vulnerability_moment;
    hu_rel_mode_t mode;
    uint64_t mode_entered_at;
    uint64_t measured_at;
} hu_rel_state_t;

typedef struct hu_rel_config {
    double drift_threshold;        /* velocity below this = drifting, default -0.1 */
    double clear_drift_threshold;  /* velocity below this = clear drift, default -0.3 */
    uint32_t repair_exit_days;    /* days of normal signals before exiting repair, default 3 */
    double drift_budget_multiplier; /* governor multiplier during drift, default 0.5 */
} hu_rel_config_t;

/* Compute velocity from signal deltas using weighted sum */
double hu_rel_compute_velocity(const hu_rel_signals_t *signals);

/* Determine relationship mode from velocity and current state */
hu_rel_mode_t hu_rel_classify_mode(double velocity, double prev_velocity,
                                   hu_rel_mode_t current_mode);

/* Compute full relationship state from signals */
hu_error_t hu_rel_compute_state(const hu_rel_signals_t *signals, double current_closeness,
                                hu_rel_mode_t current_mode, double prev_velocity,
                                hu_rel_state_t *out);

/* Get governor budget multiplier based on relationship state */
double hu_rel_budget_multiplier(const hu_rel_state_t *state,
                                const hu_rel_config_t *config);

/* Check if repair mode should exit based on velocity recovery */
bool hu_rel_should_exit_repair(const hu_rel_state_t *state,
                               const hu_rel_config_t *config, uint64_t now_ms);

/* Build SQL to create the relationship_state table */
hu_error_t hu_rel_create_table_sql(char *buf, size_t cap, size_t *out_len);

/* Build SQL to insert a state measurement */
hu_error_t hu_rel_insert_sql(const hu_rel_state_t *state, char *buf, size_t cap,
                             size_t *out_len);

/* Build SQL to query latest state for a contact */
hu_error_t hu_rel_query_latest_sql(const char *contact_id, size_t contact_id_len,
                                   char *buf, size_t cap, size_t *out_len);

/* Convert mode enum to string */
const char *hu_rel_mode_str(hu_rel_mode_t mode);

/* Convert string to mode enum */
bool hu_rel_mode_from_str(const char *str, hu_rel_mode_t *out);

/* Build prompt context describing relationship state */
hu_error_t hu_rel_build_prompt(hu_allocator_t *alloc, const hu_rel_state_t *state,
                               char **out, size_t *out_len);

void hu_rel_state_deinit(hu_allocator_t *alloc, hu_rel_state_t *state);

#endif
