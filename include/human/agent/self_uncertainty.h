#ifndef HU_AGENT_SELF_UNCERTAINTY_H
#define HU_AGENT_SELF_UNCERTAINTY_H

/* Calibrated self-uncertainty (capability-maturity-map bet #1, the metacognition
 * half). The agent already tracks a recent self-assessed confidence via the
 * metacognition ring (hu_metacog_trajectory_confidence, [0,1], estimated from
 * hedging-word frequency). This module turns that signal into a reply directive:
 * when recent confidence is LOW, inject a directive telling the model to express
 * appropriate uncertainty (hedge, say "I'm not sure") rather than overclaim —
 * a calibration nudge toward honesty.
 *
 * Wired as a new directive source in agent_turn.c, gated by env
 * HU_SELF_UNCERTAINTY (off default | shadow | on). Activation off->shadow->on is
 * gated on the blind A/B, per feature-gate-requires-measurement.
 *
 * Pure + allocation-free in assess(), so the calibration is unit-testable in
 * isolation (the ECE-direction surrogate: low confidence => hedge, high => not).
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Below this recent-confidence value, the agent should hedge / express doubt. */
#define HU_SELF_UNCERTAINTY_THRESHOLD 0.5f

typedef struct hu_self_uncertainty {
    float confidence; /* 0..1 recent self-assessed confidence (clamped input)     */
    bool hedge;       /* true when confidence < HU_SELF_UNCERTAINTY_THRESHOLD     */
} hu_self_uncertainty_t;

/* Pure: map a recent trajectory-confidence value to a hedge decision.
 * NaN / out-of-range inputs are clamped to [0,1]; `out` must be non-NULL. */
void hu_self_uncertainty_assess(float trajectory_confidence, hu_self_uncertainty_t *out);

/* Build the terse self-uncertainty directive. On hedge, sets *dir (allocator-
 * owned, free with alloc->free(ctx, *dir, *dir_len + 1)) and *dir_len. When NOT
 * hedging, sets *dir=NULL / *dir_len=0 and returns HU_OK (inject nothing).
 * Returns HU_ERR_OUT_OF_MEMORY on allocation failure. */
hu_error_t hu_self_uncertainty_build_directive(const hu_allocator_t *alloc,
                                               const hu_self_uncertainty_t *a, char **dir,
                                               size_t *dir_len);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_SELF_UNCERTAINTY_H */
