#ifndef HU_PERSONA_CELEBRATION_H
#define HU_PERSONA_CELEBRATION_H

#include "human/behavior/safety.h"     /* hu_behavior_risk_t */
#include "human/behavior/win_detect.h" /* hu_win_kind_t */
#include "human/core/allocator.h"
#include <stddef.h>

/*
 * Celebration builder (B1b) — turns a detected win into a warm, HONEST
 * celebration directive for the agent's voice. Modeled-Person context.
 *
 * This is B0's first production consumer: the builder constructs a directive
 * that is honest by construction (no claimed feelings, grounded in the specific
 * win) and then passes it through hu_prosocial_gate. If the gate SUPPRESSes
 * (e.g. a dependency/attachment risk), the builder returns NULL — h-uman does
 * not pour celebration onto an unhealthy attachment pattern. That wiring is
 * what makes the B0 guardrail load-bearing.
 *
 * Pure (no persistence): anti-re-celebration is the celebration_repo's job.
 *
 * Spec: docs/plans/2026-05-29-prosocial-uplift/
 */

/* Build a celebration directive for `kind`, gated by prosocial integrity given
 * the current `dependency_risk` (from hu_behavior_safety_assess). Returns an
 * allocated directive (caller frees via alloc) or NULL when the gate SUPPRESSes
 * or kind is HU_WIN_NONE. The directive is framed as grounded, specific warmth
 * — never a claimed feeling. */
char *hu_celebration_build_directive(hu_allocator_t *alloc, hu_win_kind_t kind,
                                     hu_behavior_risk_t dependency_risk, size_t *out_len);

#endif /* HU_PERSONA_CELEBRATION_H */
