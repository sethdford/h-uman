/* Calibrated self-uncertainty directive. See include/human/agent/self_uncertainty.h.
 * Pure assessment (no LLM, no alloc) + a terse directive builder. */
#include "human/agent/self_uncertainty.h"
#include <math.h>
#include <string.h>

void hu_self_uncertainty_assess(float trajectory_confidence, hu_self_uncertainty_t *out) {
    if (!out) {
        return;
    }
    float c = trajectory_confidence;
    if (isnan(c) || c < 0.0f) {
        c = 0.0f;
    } else if (c > 1.0f) {
        c = 1.0f;
    }
    out->confidence = c;
    out->hedge = (c < HU_SELF_UNCERTAINTY_THRESHOLD);
}

static const char SELF_UNCERTAINTY_DIRECTIVE[] =
    "\n[self-awareness] Your recent answers have run uncertain. Express appropriate doubt — hedge, "
    "and say \"i'm not sure\" or \"i think\" rather than overclaiming. Don't fake confidence.";

hu_error_t hu_self_uncertainty_build_directive(const hu_allocator_t *alloc,
                                               const hu_self_uncertainty_t *a, char **dir,
                                               size_t *dir_len) {
    if (!alloc || !a || !dir || !dir_len) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    *dir = NULL;
    *dir_len = 0;
    if (!a->hedge) {
        return HU_OK; /* confident enough — inject nothing */
    }
    size_t len = sizeof(SELF_UNCERTAINTY_DIRECTIVE) - 1;
    char *buf = (char *)alloc->realloc(alloc->ctx, NULL, 0, len + 1);
    if (!buf) {
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(buf, SELF_UNCERTAINTY_DIRECTIVE, len);
    buf[len] = '\0';
    *dir = buf;
    *dir_len = len;
    return HU_OK;
}
