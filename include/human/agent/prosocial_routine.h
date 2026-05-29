#ifndef HU_AGENT_PROSOCIAL_ROUTINE_H
#define HU_AGENT_PROSOCIAL_ROUTINE_H

#include "human/behavior/safety.h" /* hu_behavior_risk_t */
#include "human/core/allocator.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Prosocial routines (C-series) — scheduled warmth that reaches out on its own:
 * a morning intention, an evening "what went well", a weekly deeper check-in,
 * an occasional "thinking of you". Orchestration bounded context; rides the
 * A3 daemon idle-tick and routes EVERY share through the silence-biased
 * init_proposer (no new egress path).
 *
 * The scheduler is a pure predicate: given the local clock + per-routine
 * last-run ages + whether the user is active, decide which routine (if any) is
 * due. Conservative by design — at most one routine per day each, never while
 * the user is mid-conversation, all behind a default-OFF config flag.
 *
 * Spec: docs/plans/2026-05-29-prosocial-uplift/
 */

typedef enum hu_routine_kind {
    HU_ROUTINE_NONE = 0,
    HU_ROUTINE_MORNING_INTENTION,
    HU_ROUTINE_EVENING_REFLECTION,
    HU_ROUTINE_WEEKLY_CHECKIN,
    HU_ROUTINE_THINKING_OF_YOU
} hu_routine_kind_t;

typedef struct hu_routine_facts {
    int local_hour;             /* 0-23, user-local */
    int day_of_week;            /* 0=Sunday .. 6=Saturday */
    bool user_active;           /* a user turn is in flight -> never interrupt */
    int64_t secs_since_morning; /* age of last morning routine (large if never) */
    int64_t secs_since_evening;
    int64_t secs_since_weekly;
    int64_t secs_since_thinking;
} hu_routine_facts_t;

/* Windows / cadences (conservative). */
#define HU_ROUTINE_MORNING_START         7
#define HU_ROUTINE_MORNING_END           10 /* [7,10) */
#define HU_ROUTINE_EVENING_START         20
#define HU_ROUTINE_EVENING_END           23 /* [20,23) */
#define HU_ROUTINE_WEEKLY_DOW            0  /* Sunday */
#define HU_ROUTINE_WEEKLY_START          10
#define HU_ROUTINE_WEEKLY_END            14
#define HU_ROUTINE_DAILY_MIN_GAP_SECS    (20 * 3600) /* ~once/day */
#define HU_ROUTINE_WEEKLY_MIN_GAP_SECS   (6 * 24 * 3600)
#define HU_ROUTINE_THINKING_MIN_GAP_SECS (5 * 24 * 3600)

/* Decide which routine is due now. Pure; NULL -> NONE. user_active always
 * yields NONE (hard preemption). Precedence when several windows overlap:
 * weekly > morning > evening > thinking-of-you. */
hu_routine_kind_t hu_routine_due(const hu_routine_facts_t *f);

/* Build the init_proposer prompt for a due routine, gated by B0 given the
 * current dependency_risk. Returns an allocated prompt (caller frees via alloc)
 * or NULL when the gate SUPPRESSes or kind is NONE. Honest by construction —
 * an invitation, never a claimed feeling, never pressure. */
char *hu_routine_build_prompt(hu_allocator_t *alloc, hu_routine_kind_t kind,
                              hu_behavior_risk_t dependency_risk, size_t *out_len);

#endif /* HU_AGENT_PROSOCIAL_ROUTINE_H */
