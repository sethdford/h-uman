#ifndef HU_AGENT_PLANNING_H
#define HU_AGENT_PLANNING_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_plan_status {
    HU_PLAN_PROPOSED = 0,
    HU_PLAN_ACCEPTED,
    HU_PLAN_CONFIRMED,
    HU_PLAN_COMPLETED,
    HU_PLAN_CANCELLED,
    HU_PLAN_DECLINED,
    HU_PLAN_EXPIRED,
    HU_PLAN_STATUS_COUNT
} hu_plan_status_t;

typedef struct hu_plan {
    int64_t id;
    char *contact_id;
    size_t contact_id_len;
    char *activity; /* "dinner at that new Thai place" */
    size_t activity_len;
    char *suggested_time; /* "this weekend", "next Friday", "sometime soon" */
    size_t suggested_time_len;
    char *location;
    size_t location_len;
    hu_plan_status_t status;
    bool proposed_by_self; /* true if we proposed, false if they proposed */
    uint64_t proposed_at;
    uint64_t scheduled_time; /* epoch ms, 0 if unscheduled */
    uint64_t completed_at;   /* 0 if not completed */
    bool reminder_sent;
} hu_plan_t;

typedef struct hu_plan_proposal {
    char *activity;
    size_t activity_len;
    char *suggested_time;
    size_t suggested_time_len;
    char *reasoning; /* why this makes sense (for LLM context) */
    size_t reasoning_len;
    double confidence; /* 0.0-1.0 how good this proposal is */
} hu_plan_proposal_t;

typedef struct hu_planning_config {
    uint8_t max_proposals_per_contact_per_month; /* default 2 */
    uint16_t rejection_cooldown_days;            /* default 14 */
    uint16_t last_hangout_window_days;           /* suggest if > this since last completed plan */
} hu_planning_config_t;

/* Map plan status enum to string */
const char *hu_plan_status_str(hu_plan_status_t status);

/* Map string to plan status enum. Returns false if unrecognized. */
bool hu_plan_status_from_str(const char *str, hu_plan_status_t *out);

#endif
