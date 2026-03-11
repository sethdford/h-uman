#ifndef HU_AGENT_GOVERNOR_H
#define HU_AGENT_GOVERNOR_H

#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hu_proactive_budget_config {
    uint8_t daily_max;           /* hard cap, default 3 */
    uint8_t weekly_max;          /* soft cap, default 10 */
    double relationship_multiplier; /* 0.5=acquaintance, 1.0=friend, 1.5=close, 2.0=partner */
    uint8_t cool_off_after_unanswered; /* enter cool-off after N unanswered, default 2 */
    uint32_t cool_off_hours;     /* how long to cool off, default 72 */
} hu_proactive_budget_config_t;

typedef struct hu_proactive_budget {
    uint8_t daily_max;
    uint8_t weekly_max;
    uint8_t daily_used;
    uint8_t weekly_used;
    uint64_t last_reset_day;     /* day number (ms / 86400000) for daily reset */
    uint64_t last_reset_week;    /* week number for weekly reset */
    double relationship_multiplier;
    uint8_t unanswered_count;   /* consecutive unanswered proactive messages */
    uint64_t cool_off_until;     /* if > now_ms, in cool-off mode */
    uint8_t cool_off_after_unanswered;
    uint32_t cool_off_hours;
} hu_proactive_budget_t;

/* Initialize budget from config. NULL config uses defaults (3/10/1.0/2/72). */
hu_error_t hu_governor_init(const hu_proactive_budget_config_t *config,
                            hu_proactive_budget_t *out);

/* Filter by priority. priorities[i] corresponds to actions[i].
   Sets allowed[i] = true for actions that fit within budget.
   Returns number of allowed actions via out_allowed_count. */
hu_error_t hu_governor_filter_by_priority(hu_proactive_budget_t *budget,
                                          const double *priorities, size_t count,
                                          bool *allowed, size_t *out_allowed_count,
                                          uint64_t now_ms);

/* Record that a proactive message was sent (increment counters) */
hu_error_t hu_governor_record_sent(hu_proactive_budget_t *budget, uint64_t now_ms);

/* Record that contact responded (resets unanswered counter and clears cool-off) */
hu_error_t hu_governor_record_response(hu_proactive_budget_t *budget);

/* Check if any budget remains */
bool hu_governor_has_budget(const hu_proactive_budget_t *budget, uint64_t now_ms);

/* Get effective daily max (daily_max * relationship_multiplier, capped, min 1) */
uint8_t hu_governor_effective_daily_max(const hu_proactive_budget_t *budget);

/* Compute priority score for a proactive action type using weighted factors.
   Inputs 0-10, returns 0.0-1.0. */
double hu_governor_compute_priority(uint8_t urgency_0_to_10,
                                    uint8_t relevance_0_to_10,
                                    uint8_t freshness_0_to_10,
                                    uint8_t social_debt_0_to_10,
                                    uint8_t emotional_weight_0_to_10);

#endif /* HU_AGENT_GOVERNOR_H */
