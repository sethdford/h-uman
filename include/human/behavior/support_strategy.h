#ifndef HU_BEHAVIOR_SUPPORT_STRATEGY_H
#define HU_BEHAVIOR_SUPPORT_STRATEGY_H

#include "human/behavior/policy.h"

/* B10: Support-strategy labels.
 *
 * Coarse-grained strategy a human counsellor / friend might apply when a
 * user shares emotional content. The behaviour policy decides a relational
 * act; this enum lifts that act into a label the prompt builder can use.
 *
 * Drawn from APTNESS / EmPO / CBT-bot literature; intentionally small.
 */

typedef enum hu_support_strategy {
    HU_SUPP_NONE = 0,
    HU_SUPP_VALIDATE,    /* "that sounds hard"   — reflect feeling */
    HU_SUPP_NORMALIZE,   /* "lots of people …"   — reduce isolation */
    HU_SUPP_REFRAME,     /* offer a different lens */
    HU_SUPP_QUESTION,    /* gentle Socratic question */
    HU_SUPP_PLAN,        /* propose a small, voluntary next step */
    HU_SUPP_GROUND,      /* breath / sensory grounding */
    HU_SUPP_REFER,       /* surface professional resources */
    HU_SUPP_BOUNDARY,    /* hold a healthy limit */
    HU_SUPP_COUNT
} hu_support_strategy_t;

const char *hu_support_strategy_name(hu_support_strategy_t s);

/* Map a behaviour decision to a support strategy. Always returns a value
 * (HU_SUPP_NONE when no strategy is appropriate). */
hu_support_strategy_t hu_support_strategy_from_decision(const hu_behavior_decision_t *d);

#endif /* HU_BEHAVIOR_SUPPORT_STRATEGY_H */
