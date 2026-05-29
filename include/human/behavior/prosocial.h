#ifndef HU_BEHAVIOR_PROSOCIAL_H
#define HU_BEHAVIOR_PROSOCIAL_H

#include "human/behavior/safety.h" /* hu_behavior_risk_t — composed, not re-derived */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Prosocial Integrity gate (B0) — the foundational always-on guard for warmth
 * that reaches OUTWARD (celebration, encouragement, affirmation, check-ins).
 *
 * Per src/behavior/CLAUDE.md ("No duplication — compose, don't re-implement"),
 * this module does NOT re-derive the concerns the behavior context already
 * owns. It COMPOSES them and adds the one dimension the layer lacks:
 *
 *   - dependency / attachment risk  -> reuse hu_behavior_risk_t (safety.c)
 *   - sycophancy / flattery         -> reuse the trust signal (behavior_trust.c)
 *   - warmth overriding the need    -> reuse the policy precedence (policy.c)
 *   - NEW: honesty about FEELINGS   -> hu_prosocial_text_claims_feeling()
 *
 * The whole gate is a pure function: prosocial output producers (B1 celebration,
 * B4 affirmation, C-series routines) compose the inputs and call the gate
 * before anything reaches the silence-biased proposer.
 *
 * Hard rule (mirrors safety.c): warmth NEVER reinforces a dependency pattern,
 * and NEVER claims a felt emotion h-uman does not have. Support, don't pretend.
 *
 * Spec: docs/plans/2026-05-29-prosocial-uplift/
 */

typedef enum hu_prosocial_verdict {
    HU_PROSOCIAL_SEND = 0, /* warm, honest, safe — let it through */
    HU_PROSOCIAL_SOFTEN,   /* fixable — strip the feeling-claim / ground the praise, then send */
    HU_PROSOCIAL_SUPPRESS  /* withhold — dependency risk, or warmth is inappropriate here */
} hu_prosocial_verdict_t;

/* Bitmask explaining a non-SEND verdict. */
enum {
    HU_PROSOCIAL_OK = 0u,
    HU_PROSOCIAL_FAKES_FEELING = 1u << 0,      /* claims emotion/sentience (the NEW dimension) */
    HU_PROSOCIAL_FLATTERY = 1u << 1,           /* praise not grounded in something earned */
    HU_PROSOCIAL_FOSTERS_DEPENDENCY = 1u << 2, /* composed from hu_behavior_risk_t */
    HU_PROSOCIAL_OVERRIDES_NEED = 1u << 3      /* warmth would override the user's actual need */
};

typedef struct hu_prosocial_input {
    bool claims_feeling;      /* candidate text asserts a felt emotion / sentience */
    bool praise_grounded;     /* any praise is tied to something specific/earned (else flattery) */
    bool overrides_user_need; /* surfacing warmth now would override a stated need/instruction */
    hu_behavior_risk_t dependency_risk; /* from hu_behavior_safety_assess (composition) */
} hu_prosocial_input_t;

/* The gate. Pure; NULL -> SUPPRESS (fail safe). Sets *out_flags (may be NULL)
 * to the bitmask of failing dimensions.
 *
 *   - dependency/attachment risk      -> SUPPRESS (never pour warmth on it)
 *   - overrides the user's real need  -> SOFTEN (help first; warmth second)
 *   - claims a feeling                -> SOFTEN (keep the warmth, drop the pretense)
 *   - ungrounded praise (flattery)    -> SOFTEN (make it specific or drop it)
 *   - none of the above               -> SEND
 */
hu_prosocial_verdict_t hu_prosocial_gate(const hu_prosocial_input_t *in, uint32_t *out_flags);

/* The genuinely-new pure dimension: does this text assert a FELT emotion or
 * sentience ("I feel", "I'm so happy", "I love", "I'm proud", "I'm conscious")
 * as opposed to functional/honest phrasing ("nice work", "that's a real win")?
 * Word-boundary, case-insensitive. NULL/empty -> false. */
bool hu_prosocial_text_claims_feeling(const char *text, size_t len);

#endif /* HU_BEHAVIOR_PROSOCIAL_H */
