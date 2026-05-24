/* src/persona/style_adapter.c
 *
 * Per-contact style hint derived from causal_attribution counts.
 * Sprint B B-loop (2026-05-24). */

#include "human/persona/style_adapter.h"

#include "human/memory/causal_attribution.h"
#include "human/memory/personal_model.h"

#include <stdio.h>
#include <string.h>

hu_style_warmth_t hu_style_adapter_warmth(const struct hu_personal_model *model,
                                          const char *contact_handle) {
    if (!model || !contact_handle || !*contact_handle)
        return HU_STYLE_WARMTH_UNKNOWN;
    hu_causal_attribution_summary_t s;
    hu_causal_attribution_summarize(model, contact_handle, &s);
    if (s.total_reactions < HU_STYLE_ADAPTER_MIN_REACTIONS)
        return HU_STYLE_WARMTH_UNKNOWN;
    int pos = s.positive_count;
    int neg = s.negative_count;
    int total = s.total_reactions;
    /* Integer-percent thresholds — avoid floats for determinism. */
    int pos_pct = (pos * 100) / total;
    int neg_pct = (neg * 100) / total;
    if (pos_pct > 80 && total >= 5)
        return HU_STYLE_WARMTH_VERY_POSITIVE;
    if (pos_pct > 50)
        return HU_STYLE_WARMTH_POSITIVE;
    if (neg_pct > 50)
        return HU_STYLE_WARMTH_NEGATIVE;
    return HU_STYLE_WARMTH_NEUTRAL;
}

const char *hu_style_adapter_warmth_label(hu_style_warmth_t warmth) {
    switch (warmth) {
    case HU_STYLE_WARMTH_NEGATIVE:
        return "negative";
    case HU_STYLE_WARMTH_NEUTRAL:
        return "neutral";
    case HU_STYLE_WARMTH_POSITIVE:
        return "positive";
    case HU_STYLE_WARMTH_VERY_POSITIVE:
        return "very_positive";
    case HU_STYLE_WARMTH_UNKNOWN:
    default:
        return "unknown";
    }
}

size_t hu_style_adapter_render_hint(const struct hu_personal_model *model,
                                    const char *contact_handle, char *out, size_t cap) {
    if (!out || cap < 16)
        return 0;
    out[0] = '\0';
    hu_style_warmth_t w = hu_style_adapter_warmth(model, contact_handle);
    if (w == HU_STYLE_WARMTH_UNKNOWN)
        return 0;

    const char *guidance = NULL;
    switch (w) {
    case HU_STYLE_WARMTH_VERY_POSITIVE:
        guidance = "recent replies landed very well; keep current tone and pacing";
        break;
    case HU_STYLE_WARMTH_POSITIVE:
        guidance = "recent replies landed well; keep current tone";
        break;
    case HU_STYLE_WARMTH_NEUTRAL:
        guidance = "recent replies landed flat; try a slightly warmer or shorter message";
        break;
    case HU_STYLE_WARMTH_NEGATIVE:
        guidance = "recent replies landed poorly; pull back, ask a question, give space";
        break;
    default:
        return 0;
    }
    int n = snprintf(out, cap, "STYLE HINT: %s — %s.", contact_handle, guidance);
    if (n < 0)
        return 0;
    return (size_t)((size_t)n < cap ? (size_t)n : cap - 1);
}
