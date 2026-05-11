#include "human/intelligence/trust.h"
#include <stdio.h>
#include <string.h>

/* Trust modifiers per event type */
static const double TRUST_DELTAS[] = {
    [HU_TRUST_ACCURATE_RECALL] = +0.05,
    [HU_TRUST_HELPFUL_ACTION] = +0.03,
    [HU_TRUST_CORRECTION] = -0.08,
    [HU_TRUST_FABRICATION] = -0.15,
    [HU_TRUST_ERROR] = -0.05,
    [HU_TRUST_VERIFIED_CLAIM] = +0.07,
};

void hu_trust_init(hu_trust_state_t *state) {
    if (!state)
        return;
    memset(state, 0, sizeof(*state));
    state->trust_level = HU_TRUST_DEFAULT_LEVEL;
}

hu_error_t hu_trust_update(hu_trust_state_t *state, hu_trust_event_t event, int64_t now_ts) {
    if (!state)
        return HU_ERR_INVALID_ARGUMENT;
    if ((unsigned)event > HU_TRUST_VERIFIED_CLAIM)
        return HU_ERR_INVALID_ARGUMENT;

    double delta = TRUST_DELTAS[event];
    state->trust_level += delta;

    /* Clamp to [0.0, 1.0] */
    if (state->trust_level < 0.0)
        state->trust_level = 0.0;
    if (state->trust_level > 1.0)
        state->trust_level = 1.0;

    /* Update counters */
    switch (event) {
    case HU_TRUST_ACCURATE_RECALL:
        state->accurate_recalls++;
        break;
    case HU_TRUST_HELPFUL_ACTION:
        state->helpful_actions++;
        break;
    case HU_TRUST_CORRECTION:
        state->corrections_count++;
        state->last_error_at = now_ts;
        break;
    case HU_TRUST_FABRICATION:
        state->fabrications_count++;
        state->last_error_at = now_ts;
        break;
    case HU_TRUST_ERROR:
        state->last_error_at = now_ts;
        break;
    case HU_TRUST_VERIFIED_CLAIM:
        state->accurate_recalls++;
        break;
    }

    /* Update erosion rate: negative events increase erosion, positive decrease */
    if (delta < 0.0) {
        state->erosion_rate += (-delta) * 0.5;
        if (state->erosion_rate > 1.0)
            state->erosion_rate = 1.0;
    } else {
        state->erosion_rate *= 0.9; /* slow recovery */
    }

    state->last_updated_at = now_ts;
    return HU_OK;
}

bool hu_trust_detect_erosion(const hu_trust_state_t *state) {
    if (!state)
        return false;
    return state->trust_level < HU_TRUST_EROSION_THRESHOLD;
}

hu_uncertainty_level_t hu_trust_calibrate_language(const hu_trust_state_t *state,
                                                    double internal_confidence) {
    if (!state)
        return HU_UNCERTAINTY_THINK; /* safe default */

    /* Adjust confidence downward when trust is low */
    double adjusted = internal_confidence;
    if (state->trust_level < 0.5) {
        /* Scale down: at trust=0.0, halve the confidence */
        double scale = 0.5 + state->trust_level;
        adjusted *= scale;
    }

    /* Map adjusted confidence to 7-level uncertainty scale */
    if (adjusted >= 0.95)
        return HU_UNCERTAINTY_CERTAIN;
    if (adjusted >= 0.85)
        return HU_UNCERTAINTY_CONFIDENT;
    if (adjusted >= 0.70)
        return HU_UNCERTAINTY_FAIRLY_SURE;
    if (adjusted >= 0.55)
        return HU_UNCERTAINTY_THINK;
    if (adjusted >= 0.40)
        return HU_UNCERTAINTY_BELIEVE;
    if (adjusted >= 0.25)
        return HU_UNCERTAINTY_NOT_SURE;
    return HU_UNCERTAINTY_SPECULATING;
}

const char *hu_uncertainty_prefix(hu_uncertainty_level_t level) {
    switch (level) {
    case HU_UNCERTAINTY_CERTAIN:
        return "I know ";
    case HU_UNCERTAINTY_CONFIDENT:
        return "I'm confident that ";
    case HU_UNCERTAINTY_FAIRLY_SURE:
        return "I'm fairly sure ";
    case HU_UNCERTAINTY_THINK:
        return "I think ";
    case HU_UNCERTAINTY_BELIEVE:
        return "I believe ";
    case HU_UNCERTAINTY_NOT_SURE:
        return "I'm not sure, but ";
    case HU_UNCERTAINTY_SPECULATING:
        return "I may be wrong, but ";
    default:
        return "I think ";
    }
}

hu_error_t hu_trust_state_build_directive(hu_allocator_t *alloc, const hu_trust_state_t *state,
                                          char **out, size_t *out_len) {
    if (!alloc || !state || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_len = 0;

    /* Only emit a directive when trust is below neutral */
    if (state->trust_level >= HU_TRUST_DEFAULT_LEVEL)
        return HU_OK;

    char buf[512];
    int n;

    if (state->trust_level < HU_TRUST_EROSION_THRESHOLD) {
        n = snprintf(buf, sizeof(buf),
                     "[TRUST: LOW (%.0f%%)] Be extra careful with claims. "
                     "Preface uncertain statements with hedging language. "
                     "Verify facts before stating them. "
                     "Corrections received: %u, fabrications detected: %u.",
                     state->trust_level * 100.0, state->corrections_count,
                     state->fabrications_count);
    } else {
        n = snprintf(buf, sizeof(buf),
                     "[TRUST: MODERATE (%.0f%%)] Use appropriate uncertainty markers. "
                     "Double-check claims before making them.",
                     state->trust_level * 100.0);
    }

    if (n <= 0 || (size_t)n >= sizeof(buf))
        return HU_ERR_IO;

    size_t len = (size_t)n;
    char *result = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;

    memcpy(result, buf, len);
    result[len] = '\0';
    *out = result;
    *out_len = len;
    return HU_OK;
}
