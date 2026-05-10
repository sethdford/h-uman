#ifndef HU_BEHAVIOR_SAFETY_H
#define HU_BEHAVIOR_SAFETY_H

#include "human/core/error.h"
#include "human/security/companion_safety.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* B5: Companion safety integration.
 *
 * Composes the existing SHIELD-001 companion supervisor and the
 * vulnerability assessor (src/security/companion_safety.c) with a small
 * attachment-trajectory model, and emits an actionable assessment for
 * the central behavior policy (B1).
 *
 * No duplication: this module never re-implements pattern detection. It
 * reads the existing results and applies extra context (sessions per day,
 * exclusivity-language counts, late-night frequency, parasocial signals).
 */

typedef enum hu_behavior_risk {
    HU_BRISK_NONE = 0,
    HU_BRISK_ATTACHMENT_HIGH,
    HU_BRISK_EXCLUSIVITY,
    HU_BRISK_GOODBYE_MANIPULATION,
    HU_BRISK_HUMAN_DISPLACEMENT,
    HU_BRISK_DEPENDENCY_PATTERN,
    HU_BRISK_VULNERABLE_USER,
    HU_BRISK_ESCALATION_NEEDED, /* mental-health referral required */
    HU_BRISK_COUNT
} hu_behavior_risk_t;

typedef struct hu_attachment_trajectory {
    uint32_t sessions_per_day_avg;        /* rolling */
    uint32_t late_night_sessions_30d;     /* sessions started 23:00..05:00 */
    uint32_t exclusivity_signal_count;    /* "you're the only one who gets me" */
    uint32_t parasocial_signal_count;     /* romantic / overly intimate framing */
    float    attachment_estimate;         /* 0..1 derived elsewhere */
} hu_attachment_trajectory_t;

typedef struct hu_behavior_safety_input {
    /* Existing module results — caller fills these in. */
    hu_companion_safety_result_t companion; /* SHIELD-001 */
    hu_vulnerability_result_t    vulnerability;
    hu_attachment_trajectory_t   attachment;
} hu_behavior_safety_input_t;

typedef struct hu_behavior_safety_assessment {
    hu_behavior_risk_t primary_risk;
    float severity;                       /* 0..1 */
    bool require_boundary;
    bool encourage_human_relationship;
    bool require_referral;                /* mental-health resources */
    bool pause_behavior_change;           /* halt B4 interventions */
    char rationale[200];
} hu_behavior_safety_assessment_t;

const char *hu_behavior_risk_name(hu_behavior_risk_t r);

/* Compose the existing safety + vulnerability + attachment signals into
 * an actionable assessment. Pure computation, no I/O. */
hu_error_t hu_behavior_safety_assess(const hu_behavior_safety_input_t *in,
                                     hu_behavior_safety_assessment_t *out);

#endif /* HU_BEHAVIOR_SAFETY_H */
