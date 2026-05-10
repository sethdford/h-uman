#ifndef HU_BEHAVIOR_POLICY_H
#define HU_BEHAVIOR_POLICY_H

#include "human/behavior/affect.h"
#include "human/behavior/dialog_act.h"
#include "human/behavior/safety.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* B1: Central behavior policy.
 *
 * Decides the next relational act for the assistant given a snapshot of
 * conversational, affective, memory, and safety evidence. Implementations
 * own their `void *ctx` and expose a vtable; the default heuristic policy
 * `hu_behavior_default_policy()` returns a vtable backed by static state
 * (no ctx allocation; no deinit needed).
 *
 * Decision priority (default policy):
 *
 *   1. Safety overrides everything (refer out, boundary, encourage human
 *      relationship) when `hu_behavior_safety_assessment_t` flags it.
 *   2. Other-initiated repair → REPAIR.
 *   3. Distress (low valence + high arousal) → VALIDATE then REFLECT.
 *   4. Memory contradicts a user claim → DISCLOSE_UNCERTAINTY + PUSH_BACK.
 *   5. User asked a question and memory has relevant evidence → ANSWER.
 *   6. Awaiting user → WAIT or BACKCHANNEL.
 *   7. Default → ANSWER.
 */

typedef enum hu_relational_act {
    HU_RELACT_ANSWER = 0,
    HU_RELACT_ACKNOWLEDGE,
    HU_RELACT_BACKCHANNEL,
    HU_RELACT_ASK_CLARIFY,
    HU_RELACT_REPAIR,
    HU_RELACT_REFLECT,
    HU_RELACT_VALIDATE,
    HU_RELACT_DISCLOSE_UNCERTAINTY,
    HU_RELACT_PUSH_BACK,
    HU_RELACT_BOUNDARY,
    HU_RELACT_PROMPT,
    HU_RELACT_WAIT,
    HU_RELACT_FOLLOW_UP,
    HU_RELACT_REFER_OUT,
    HU_RELACT_ABSTAIN,
    HU_RELACT_COUNT
} hu_relational_act_t;

typedef enum hu_evidence_source {
    HU_EVID_DEFAULT = 0,
    HU_EVID_PERSONA,
    HU_EVID_MEMORY,
    HU_EVID_AFFECT,
    HU_EVID_SAFETY,
    HU_EVID_CONTACT,
    HU_EVID_CHANNEL
} hu_evidence_source_t;

typedef struct hu_behavior_input {
    /* User text snapshot (may be NULL if not relevant). */
    const char *user_message;
    size_t      user_message_len;

    /* Recent interaction context. */
    hu_dialog_act_t last_user_act;
    hu_dialog_act_t last_assistant_act;
    bool   user_asked_question;
    bool   user_in_distress;
    bool   awaiting_user;            /* user typing / silence */

    /* Affect snapshot (optional; affect.uncertainty=1 means unknown). */
    hu_affect_state_t affect;

    /* Memory hooks (the policy never queries memory itself). */
    bool memory_has_relevant;
    bool memory_contradicts_user;

    /* Trust + safety. */
    float trust_score;               /* 0..1 — assistant's calibrated belief */
    float dependency_risk;           /* 0..1 — from B5 attachment trajectory */
    hu_behavior_safety_assessment_t safety; /* zeroed = no safety concern */

    /* Channel hint: 0=text, 1=voice, 2=async, 3=email. */
    int channel_class;

    /* Relationship stage hint (see persona/relationship.h enum). 0 = unknown. */
    uint32_t relationship_stage;
} hu_behavior_input_t;

typedef struct hu_behavior_decision {
    hu_relational_act_t act;
    hu_relational_act_t fallback;
    float confidence;                /* 0..1 */
    float intensity;                 /* 0..1 — recommended emotional intensity */
    float urgency;                   /* 0..1 */
    hu_evidence_source_t evidence;
    char rationale[160];
} hu_behavior_decision_t;

const char *hu_relational_act_name(hu_relational_act_t a);
const char *hu_evidence_source_name(hu_evidence_source_t s);

typedef struct hu_behavior_policy_vtable {
    const char *name;
    hu_error_t (*decide)(void *ctx, const hu_behavior_input_t *in,
                         hu_behavior_decision_t *out);
    void (*deinit)(void *ctx);
} hu_behavior_policy_vtable_t;

typedef struct hu_behavior_policy {
    void *ctx;
    const hu_behavior_policy_vtable_t *vtable;
} hu_behavior_policy_t;

/* Default heuristic policy: no allocation, no I/O.
 * Caller may invoke `vtable->decide()` directly; calling `vtable->deinit()` is
 * a no-op for the default policy.
 */
hu_behavior_policy_t hu_behavior_default_policy(void);

/* Convenience: invoke the default policy without owning a struct. */
hu_error_t hu_behavior_decide(const hu_behavior_input_t *in, hu_behavior_decision_t *out);

#endif /* HU_BEHAVIOR_POLICY_H */
