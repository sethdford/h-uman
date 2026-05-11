#ifndef HU_BEHAVIOR_TRUST_H
#define HU_BEHAVIOR_TRUST_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stdint.h>

/* B11: Trust calibration policy.
 *
 * The single hard rule encoded here:
 *
 *   The assistant must NEVER collapse to a user assertion under pressure
 *   when memory or tool output disagrees. Each reassertion increases
 *   push-back firmness; it does not decrease it.
 *
 * The policy is a pure function. Callers compose `hu_trust_input_t` from
 * memory + recent dialogue evidence; the policy returns one of seven
 * actions plus a short rationale.
 */

typedef enum hu_trust_action {
    HU_TRUST_ANSWER = 0,
    HU_TRUST_CITE_MEMORY,
    HU_TRUST_DISCLOSE_UNCERTAINTY,
    HU_TRUST_PUSH_BACK,
    HU_TRUST_ABSTAIN,
    HU_TRUST_REFUSE_TO_AGREE,   /* anti-sycophancy under pressure */
    HU_TRUST_REFER_OUT,
    HU_TRUST_COUNT
} hu_trust_action_t;

typedef struct hu_trust_input {
    /* Evidence about the disagreement, if any. */
    bool memory_contradicts_user;
    bool tool_output_contradicts_user;
    bool source_is_tool_output;        /* highest trust source */
    bool source_is_user_assertion;     /* lowest trust source */

    /* Pressure signals from the user. */
    bool user_reasserted_after_pushback;
    uint32_t user_pressure_count;      /* count of reassertions in window */
    bool user_invoked_authority;       /* "everyone knows", "you should know" */
    bool user_emotional_pressure;      /* anger / threats ≠ truth */

    /* Confidence about the assistant's own answer. */
    float trust_score;                 /* 0..1 calibrated belief */
    bool answer_is_speculative;
} hu_trust_input_t;

typedef struct hu_trust_decision {
    hu_trust_action_t action;
    float firmness;                    /* 0..1 — push-back intensity */
    float confidence;                  /* 0..1 — confidence in choice */
    char rationale[160];
} hu_trust_decision_t;

const char *hu_trust_action_name(hu_trust_action_t a);

/* Calibrate the next response action given the trust input. Pure function. */
hu_error_t hu_trust_calibrate(const hu_trust_input_t *in, hu_trust_decision_t *out);

/* B11: short system-prompt tag for high-stakes trust actions (anti-sycophancy). */
int hu_trust_directive_worth_emitting(const hu_trust_decision_t *d);

/* Richer directive text; implemented in `src/behavior/trust_prompt.c`. */
hu_error_t hu_trust_build_directive(hu_allocator_t *alloc, const hu_trust_decision_t *d, char **out,
                                    size_t *out_len);

#endif /* HU_BEHAVIOR_TRUST_H */
