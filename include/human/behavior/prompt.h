#ifndef HU_BEHAVIOR_PROMPT_H
#define HU_BEHAVIOR_PROMPT_H

#include "human/behavior/policy.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

/* B-prompt: turn a `hu_behavior_decision_t` into a system-prompt snippet.
 *
 * Output format (when an act is emitted):
 *
 *   "\n\n[Behavior: <act> — <directive sentence>"
 *   "\n  Evidence: <source>. Confidence: <int>%.]"
 *
 * Caller owns the buffer (allocated with `alloc`). On low-confidence or
 * default ANSWER decisions, returns HU_OK with `*out = NULL` and `*out_len = 0`
 * so the caller can skip appending without a special case.
 *
 * The text is intentionally short (≤200 bytes) to stay within token budgets
 * when added to every turn.
 */
hu_error_t hu_behavior_build_directive(hu_allocator_t *alloc,
                                       const hu_behavior_decision_t *decision,
                                       char **out, size_t *out_len);

/* True when the policy decision is worth surfacing to the model.
 * Equivalent to `act != ANSWER || confidence >= 0.7`.
 */
int hu_behavior_directive_is_worth_emitting(const hu_behavior_decision_t *d);

#endif /* HU_BEHAVIOR_PROMPT_H */
