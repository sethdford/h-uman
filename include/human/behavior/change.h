#ifndef HU_BEHAVIOR_CHANGE_H
#define HU_BEHAVIOR_CHANGE_H

#include "human/core/error.h"
#include "human/persona/circadian.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* B4: Behavior-change technique (BCT) selection grounded in the
 * Fogg Behavior Model and just-in-time adaptive intervention (JITAI)
 * principles.
 *
 * Hard rules:
 *   - Never run VERBAL_PERSUASION when autonomy_risk > 0.3.
 *   - Never push a prompt outside reasonable hours unless the user
 *     opted in explicitly.
 *   - Never suggest behavior change while distress is escalating.
 *
 * The selector is a pure function over its inputs (no I/O, no state).
 */

typedef enum hu_bct {
    HU_BCT_NONE = 0,
    HU_BCT_GOAL_SETTING,
    HU_BCT_ACTION_PLANNING,
    HU_BCT_FEEDBACK_MONITORING,
    HU_BCT_SELF_MONITORING,
    HU_BCT_PROMPTS_CUES,
    HU_BCT_REWARD,
    HU_BCT_SOCIAL_SUPPORT,
    HU_BCT_REDUCE_FRICTION,
    HU_BCT_REFRAMING,
    HU_BCT_BEHAVIORAL_REHEARSAL,
    HU_BCT_HABIT_REVERSAL,
    HU_BCT_VERBAL_PERSUASION, /* gated; rarely allowed */
    HU_BCT_COUNT
} hu_bct_t;

typedef struct hu_fogg_state {
    float motivation;       /* 0..1 */
    float ability;          /* 0..1 (1 = easy)  */
    float prompt_readiness; /* 0..1 (is now a good moment?) */
} hu_fogg_state_t;

typedef struct hu_behavior_change_input {
    hu_fogg_state_t fogg;
    bool user_invited_help;
    bool user_explicit_consent;
    float autonomy_risk;            /* 0..1 — high = avoid pushing */
    float burden;                   /* 0..1 — high = user is fatigued */
    float distress_escalation;      /* 0..1 — high = pause interventions */
    uint8_t hour;                   /* 0..23 (local) for JITAI gating */
    bool late_night_opt_in;         /* user explicitly allows late prompts */
    /* B16: when not HU_CHRONO_UNKNOWN, `hour` outside chronotype active band defers
     * (unless late_night_opt_in). UNKNOWN preserves legacy 23:00–05:59 gate only. */
    hu_chronotype_t jitai_chronotype;
} hu_behavior_change_input_t;

typedef struct hu_behavior_change_decision {
    hu_bct_t technique;
    bool act_now;
    bool ask_permission_first;
    bool defer;                     /* defer to a better moment */
    char rationale[160];
} hu_behavior_change_decision_t;

const char *hu_bct_name(hu_bct_t b);

/* Select an evidence-based BCT given the input state.
 * Returns HU_OK on success, HU_ERR_INVALID_ARGUMENT for NULL inputs.
 */
hu_error_t hu_behavior_change_select(const hu_behavior_change_input_t *in,
                                     hu_behavior_change_decision_t *out);

#endif /* HU_BEHAVIOR_CHANGE_H */
