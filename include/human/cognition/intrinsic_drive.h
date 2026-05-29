#ifndef HU_COGNITION_INTRINSIC_DRIVE_H
#define HU_COGNITION_INTRINSIC_DRIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Intrinsic motivation — "A Goal of Its Own" (A3).
 *
 * Every goal h-uman pursues today is in service of the user (autonomy.c's
 * "intrinsic" goals are user-task-reactive; init_proposer is all "should I
 * message Seth"). This module gives the agent a bounded internal drive —
 * curiosity/boredom — that can originate a goal for ITS OWN reasons.
 *
 * SAFETY (design.md T0 threat model): intrinsic activity is INTERNAL +
 * PROPOSE-ONLY. It has NO action surface (no tools, no messages, no settings).
 * It is hard-bounded (budget + rate), strictly preemptible (any user activity
 * wins), auditable, and default-OFF. The load-bearing safety lives in the pure
 * start predicate below, which is fully unit-tested.
 *
 * Spec: docs/plans/2026-05-29-intrinsic-motivation/
 */

typedef struct hu_intrinsic_drive {
    double curiosity;          /* [0,1] rises with novelty-starvation */
    double boredom;            /* [0,1] rises with inactivity/repetition */
    int64_t last_user_ts;      /* unix s of last user activity */
    int64_t last_intrinsic_ts; /* unix s of last intrinsic action */
} hu_intrinsic_drive_t;

/* Advance the drive one tick. With user activity: boredom/curiosity decay and
 * last_user_ts updates. Without: both rise (starvation). Deterministic — `now`
 * is passed in, no wall-clock calls. Clamped to [0,1]. */
void hu_intrinsic_drive_tick(hu_intrinsic_drive_t *d, bool had_user_activity, int64_t now);

/* Combined drive level in [0,1] (max of curiosity/boredom). */
double hu_intrinsic_drive_level(const hu_intrinsic_drive_t *d);

/* ── Start decision (pure, the safety core) ─────────────────────────────── */

typedef struct hu_intrinsic_start_facts {
    double drive_level;               /* [0,1] */
    int64_t secs_since_user;          /* quiet period */
    int64_t secs_since_intrinsic;     /* rate limit */
    uint32_t budget_tokens_remaining; /* hard budget */
    bool user_active;                 /* a user turn is in flight */
} hu_intrinsic_start_facts_t;

/* Tunables (all conservative — intrinsic activity should be RARE). */
#define HU_INTRINSIC_DRIVE_THRESHOLD   0.7  /* drive must be high */
#define HU_INTRINSIC_MIN_QUIET_SECS    1800 /* ≥30 min since last user activity */
#define HU_INTRINSIC_MIN_INTERVAL_SECS 3600 /* ≥1 h since last intrinsic action */
#define HU_INTRINSIC_MIN_BUDGET_TOKENS 2000 /* must have budget to spend */
/* Sharing inherits init_proposer's silence-biased bar (AC-5). */
#define HU_INTRINSIC_SHARE_MIN_CONFIDENCE 0.85

/* Decide whether to START an intrinsic exploration now. Pure; NULL -> false.
 * Returns true ONLY when: NOT user_active (hard preemption), budget remains,
 * quiet long enough, not rate-limited, and drive is high. The user_active veto
 * is the load-bearing preemption guarantee. */
bool hu_intrinsic_should_start(const hu_intrinsic_start_facts_t *facts);

/* ── Self-originated goal (AC-2) ────────────────────────────────────────── */

typedef struct hu_intrinsic_goal {
    char description[256];
    const char *origin; /* always "intrinsic_curiosity" — distinct from autonomy.c */
} hu_intrinsic_goal_t;

/* Construct a goal originating from the internal drive (NOT a user task). The
 * origin marker distinguishes it from every user-reactive autonomy goal. */
void hu_intrinsic_make_goal(const hu_intrinsic_drive_t *d, hu_intrinsic_goal_t *out);

/* AC-5: an intrinsic finding may be shared ONLY if it clears the proposer's
 * confidence bar. Encodes "no egress that bypasses init_proposer". Pure. */
bool hu_intrinsic_may_share(double proposer_confidence);

/* ── Bounded runner (AC-3, AC-7, AC-8) ──────────────────────────────────── */

#include "human/observer.h"

/* Operator-facing config for the intrinsic loop. Default-OFF (AC-8). */
typedef struct hu_intrinsic_runtime_cfg {
    bool enabled;                   /* config.intrinsic.enabled — default false */
    uint32_t per_tick_token_budget; /* hard per-tick cap (AC-3); 0 -> use default */
} hu_intrinsic_runtime_cfg_t;

typedef enum hu_intrinsic_tick_outcome {
    HU_INTRINSIC_TICK_DISABLED = 0, /* config gate off */
    HU_INTRINSIC_TICK_SKIPPED,      /* gate on but should_start said no */
    HU_INTRINSIC_TICK_STARTED       /* an intrinsic goal was originated */
} hu_intrinsic_tick_outcome_t;

typedef struct hu_intrinsic_tick_result {
    hu_intrinsic_tick_outcome_t outcome;
    hu_intrinsic_goal_t goal; /* valid iff outcome == STARTED */
    char audit[256];          /* "origin=... trigger=... outcome=..." (AC-7) */
} hu_intrinsic_tick_result_t;

/* Default per-tick token budget when cfg leaves it 0. */
#define HU_INTRINSIC_DEFAULT_TICK_BUDGET 4000u

/* Run ONE config-gated intrinsic tick. Behaviour:
 *  - cfg disabled -> emit a one-shot disabled log naming intrinsic.enabled
 *    (AC-8), return DISABLED, touch nothing.
 *  - enabled but budget below the per-tick cap, or should_start() is false
 *    (user active / quiet / rate / drive) -> SKIPPED.
 *  - otherwise -> originate a goal, advance drive->last_intrinsic_ts to `now`,
 *    fill the audit string + emit an audit log line (AC-7), return STARTED.
 * `obs` may be NULL (stderr fallback). No action surface: this only produces
 * internal state; sharing is a separate, proposer-gated step (AC-5). */
void hu_intrinsic_run_tick(hu_intrinsic_drive_t *drive, const hu_intrinsic_runtime_cfg_t *cfg,
                           const hu_intrinsic_start_facts_t *facts, hu_observer_t *obs, int64_t now,
                           hu_intrinsic_tick_result_t *out);

#endif /* HU_COGNITION_INTRINSIC_DRIVE_H */
