#ifndef HU_AGENT_COLLAB_PLANNING_H
#define HU_AGENT_COLLAB_PLANNING_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* F130-F133 — Collaborative Planning */

typedef enum hu_plan_status {
    HU_PLAN_PROPOSED = 0,
    HU_PLAN_ACCEPTED,
    HU_PLAN_DECLINED,
    HU_PLAN_COMPLETED,
    HU_PLAN_CANCELLED,
    HU_PLAN_EXPIRED,
    HU_PLAN_STATUS_COUNT
} hu_plan_status_t;

typedef enum hu_plan_trigger_type {
    HU_PLAN_TRIGGER_INTEREST_MATCH = 0,
    HU_PLAN_TRIGGER_SEASONAL,
    HU_PLAN_TRIGGER_EXTERNAL_EVENT,
    HU_PLAN_TRIGGER_MILESTONE,
    HU_PLAN_TRIGGER_SPONTANEOUS,
    HU_PLAN_TRIGGER_TYPE_COUNT
} hu_plan_trigger_type_t;

typedef struct hu_collab_plan {
    int64_t plan_id;
    char *contact_id;
    size_t contact_id_len;
    char *description;
    size_t description_len;
    char *location; /* nullable */
    size_t location_len;
    uint64_t proposed_time_ms;
    hu_plan_status_t status;
    hu_plan_trigger_type_t trigger_type;
} hu_collab_plan_t;

typedef struct hu_plan_trigger {
    char *contact_id;
    size_t contact_id_len;
    char *reason;
    size_t reason_len;
    hu_plan_trigger_type_t trigger_type;
    double relevance_score;
} hu_plan_trigger_t;

/* Build SQL to create the plans table */
hu_error_t hu_collab_plan_create_table_sql(char *buf, size_t cap, size_t *out_len);

/* Build SQL to insert a plan */
hu_error_t hu_collab_plan_insert_sql(const hu_collab_plan_t *plan, char *buf, size_t cap,
                                     size_t *out_len);

/* Build SQL to query plans by contact and optional status filter.
   status_filter may be NULL for no filter. */
hu_error_t hu_collab_plan_query_sql(const char *contact_id, size_t contact_id_len,
                                   const hu_plan_status_t *status_filter, char *buf, size_t cap,
                                   size_t *out_len);

/* Build SQL to update plan status */
hu_error_t hu_collab_plan_update_status_sql(int64_t plan_id, hu_plan_status_t new_status,
                                            char *buf, size_t cap, size_t *out_len);

/* Score a trigger against contact interests (word overlap). Returns 0.0–1.0. */
double hu_collab_plan_score_trigger(const hu_plan_trigger_t *trigger, const char **interests,
                                    size_t interest_count);

/* Gating: should we propose a plan to this contact? */
bool hu_collab_plan_should_propose(const char *contact_id, size_t contact_id_len,
                                   unsigned int daily_proposals_sent, unsigned int max_daily,
                                   double relationship_closeness);

/* Build prompt context from triggers and recent plans. Allocates *out. Caller frees. */
hu_error_t hu_collab_plan_build_prompt(hu_allocator_t *alloc,
                                      const hu_plan_trigger_t *triggers, size_t trigger_count,
                                      const hu_collab_plan_t *recent_plans, size_t plan_count,
                                      char **out, size_t *out_len);

void hu_collab_plan_deinit(hu_allocator_t *alloc, hu_collab_plan_t *plan);
void hu_plan_trigger_deinit(hu_allocator_t *alloc, hu_plan_trigger_t *trigger);

const char *hu_plan_status_str(hu_plan_status_t status);

#endif /* HU_AGENT_COLLAB_PLANNING_H */
