/* include/human/persona/style_adapter.h
 *
 * Per-contact style adapter — Sprint B B-loop (2026-05-24).
 *
 * Sprint B added four memory blocks to the persona prompt
 * (EMOTIONAL CONTEXT, UPCOMING, WHAT WORKS, IDENTITY). The agent
 * READS them but doesn't yet REACT to them — every reply uses the
 * same persona overlay regardless of the contact's reaction history.
 *
 * This module closes the observe→react→adapt loop with the smallest
 * possible API surface:
 *
 *   1. hu_style_adapter_warmth — derive a per-contact warmth enum
 *      from causal_attribution counts. Pure.
 *   2. hu_style_adapter_render_hint — given the contact and the
 *      model, emit a one-line "STYLE HINT:" prompt block that
 *      recommends tone adjustments based on what's been working
 *      (or not). Pure.
 *
 * Why a hint (not an overlay mutation): mutating the persona
 * overlay would silently change behavior across every channel the
 * contact uses. A prompt hint is observable, debuggable via grep,
 * and lets the LLM decide whether the recommendation applies in
 * the specific moment. We choose to nudge, not steer.
 */
#ifndef HU_PERSONA_STYLE_ADAPTER_H
#define HU_PERSONA_STYLE_ADAPTER_H

#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hu_personal_model;

typedef enum hu_style_warmth {
    HU_STYLE_WARMTH_UNKNOWN = 0,   /* insufficient data */
    HU_STYLE_WARMTH_NEGATIVE,      /* >50% negative reactions in window */
    HU_STYLE_WARMTH_NEUTRAL,       /* mixed or mostly neutral */
    HU_STYLE_WARMTH_POSITIVE,      /* >50% positive reactions in window */
    HU_STYLE_WARMTH_VERY_POSITIVE, /* >80% positive AND >=5 total */
} hu_style_warmth_t;

/* Minimum reactions before we report anything stronger than UNKNOWN.
 * Below this, the signal is too noisy to act on. */
#define HU_STYLE_ADAPTER_MIN_REACTIONS 3

/* Pure: classify warmth from causal_attribution counts for a contact.
 * Returns UNKNOWN when total_reactions < MIN_REACTIONS. */
hu_style_warmth_t hu_style_adapter_warmth(const struct hu_personal_model *model,
                                          const char *contact_handle);

/* Human-readable label. Returns a borrowed string; never NULL. */
const char *hu_style_adapter_warmth_label(hu_style_warmth_t warmth);

/* Pure: render the "STYLE HINT:" prompt block for a contact. Returns
 * bytes written. Returns 0 for UNKNOWN warmth (nothing to surface yet).
 *
 * Output shape:
 *   "STYLE HINT: alice — recent replies landed well; keep current tone."
 *   "STYLE HINT: alice — recent replies landed flat; try shorter / warmer."
 */
size_t hu_style_adapter_render_hint(const struct hu_personal_model *model,
                                    const char *contact_handle, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* HU_PERSONA_STYLE_ADAPTER_H */
