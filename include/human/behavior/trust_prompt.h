#ifndef HU_BEHAVIOR_TRUST_PROMPT_H
#define HU_BEHAVIOR_TRUST_PROMPT_H

#include "human/behavior/trust.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

/* B-trust-prompt: turn a `hu_trust_decision_t` into a system-prompt snippet.
 *
 * Output format (when emitted):
 *
 *   "\n\n[Trust: <action> — <directive sentence>"
 *   "\n  Rationale: <short>. Firmness: <int>%.]"
 *
 * Caller owns the buffer (allocated with `alloc`). For low-impact actions
 * (default ANSWER), returns `(NULL, 0)` so callers can append unconditionally.
 *
 * The directive is short (≤220 bytes) to stay within token budgets when
 * added to every turn that detects pressure or contradiction.
 *
 * Implemented in `trust_prompt.c`.
 */

/* True when the trust decision is worth surfacing to the model.
 * Equivalent to `action != ANSWER`. */
int hu_trust_directive_is_worth_emitting(const hu_trust_decision_t *d);

hu_error_t hu_trust_build_directive(hu_allocator_t *alloc, const hu_trust_decision_t *decision,
                                    char **out, size_t *out_len);

#endif /* HU_BEHAVIOR_TRUST_PROMPT_H */
