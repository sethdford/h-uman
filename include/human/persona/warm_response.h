#ifndef HU_PERSONA_WARM_RESPONSE_H
#define HU_PERSONA_WARM_RESPONSE_H

#include "human/behavior/prosocial_moment.h" /* hu_pmoment_kind_t */
#include "human/behavior/safety.h"           /* hu_behavior_risk_t */
#include "human/core/allocator.h"
#include <stddef.h>

/*
 * Warm-response builder (B2/B4/B5) — turns a detected prosocial moment into an
 * honest, grounded directive for the agent's voice. Modeled-Person context.
 *
 * Like the celebration builder, every directive is gated by hu_prosocial_gate
 * (B0): honest by construction (no claimed feelings, grounded), and SUPPRESSed
 * when the current dependency_risk says warmth would reinforce an unhealthy
 * pattern. Pure (no persistence).
 *
 * Spec: docs/plans/2026-05-29-prosocial-uplift/
 */

/* Build a warm-response directive for `kind`, gated by B0 given the current
 * `dependency_risk`. Returns an allocated directive (caller frees via alloc)
 * or NULL when the gate SUPPRESSes or kind is HU_PMOMENT_NONE. */
char *hu_warm_response_build_directive(hu_allocator_t *alloc, hu_pmoment_kind_t kind,
                                       hu_behavior_risk_t dependency_risk, size_t *out_len);

#endif /* HU_PERSONA_WARM_RESPONSE_H */
