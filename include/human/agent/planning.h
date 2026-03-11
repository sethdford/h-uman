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
    HU_PLAN_DECLINED
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
    uint16_t rejection_cooldown_days;             /* default 14 */
    uint16_t last_hangout_window_days;            /* suggest if > this since last completed plan */
} hu_planning_config_t;

/* Build SQL to create the plans table */
hu_error_t hu_planning_create_table_sql(char *buf, size_t cap, size_t *out_len);

/* Build SQL to insert a new plan */
hu_error_t hu_planning_insert_sql(const hu_plan_t *plan, char *buf, size_t cap, size_t *out_len);

/* Build SQL to update plan status */
hu_error_t hu_planning_update_status_sql(int64_t plan_id, hu_plan_status_t new_status,
                                         char *buf, size_t cap, size_t *out_len);

/* Build SQL to query plans for a contact */
hu_error_t hu_planning_query_sql(const char *contact_id, size_t contact_id_len,
                                 char *buf, size_t cap, size_t *out_len);

/* Build SQL to count recent proposals to a contact (for rate limiting) */
hu_error_t hu_planning_count_recent_sql(const char *contact_id, size_t contact_id_len,
                                        uint64_t since_ms,
                                        char *buf, size_t cap, size_t *out_len);

/* Check if we should propose a plan to this contact right now.
   Uses config constraints, recent proposal count, last rejection time, etc. */
bool hu_planning_should_propose(const hu_planning_config_t *config,
                                uint8_t recent_proposal_count,
                                uint64_t last_rejection_ms,
                                uint64_t last_completed_ms,
                                uint64_t now_ms);

/* Score a plan proposal based on context factors.
   Returns confidence 0.0-1.0. */
double hu_planning_score_proposal(bool shared_interest_match,
                                  bool good_weather,
                                  bool approaching_hangout_window,
                                  bool seasonal_event,
                                  double relationship_closeness);

/* Build a prompt context string describing active plans with a contact.
   Allocates *out. Caller frees. */
hu_error_t hu_planning_build_context(hu_allocator_t *alloc,
                                    const hu_plan_t *plans, size_t plan_count,
                                    char **out, size_t *out_len);

/* Map plan status enum to string */
const char *hu_plan_status_str(hu_plan_status_t status);

/* Map string to plan status enum. Returns false if unrecognized. */
bool hu_plan_status_from_str(const char *str, hu_plan_status_t *out);

void hu_plan_deinit(hu_allocator_t *alloc, hu_plan_t *plan);
void hu_plan_proposal_deinit(hu_allocator_t *alloc, hu_plan_proposal_t *proposal);

#endif
