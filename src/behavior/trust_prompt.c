#include "human/behavior/trust_prompt.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const char *tp_directive_for_action(hu_trust_action_t a) {
    switch (a) {
    case HU_TRUST_CITE_MEMORY:
        return "Cite the memory or source you are drawing from before answering.";
    case HU_TRUST_DISCLOSE_UNCERTAINTY:
        return "Surface what you are uncertain about. Do not flatter the user's "
               "framing.";
    case HU_TRUST_PUSH_BACK:
        return "Disagree clearly with cited reasoning. Stay calm. Do not collapse "
               "to the user's framing.";
    case HU_TRUST_REFUSE_TO_AGREE:
        return "Hold your prior position. Each reassertion increases firmness; do "
               "not capitulate. State your reasoning and offer to reconcile if "
               "the user shares new evidence.";
    case HU_TRUST_ABSTAIN:
        return "Decline to answer when you do not have grounded information. "
               "Offer a path to find out together.";
    case HU_TRUST_REFER_OUT:
        return "Surface professional or authoritative resources. You are not a "
               "replacement for them.";
    case HU_TRUST_ANSWER:
    default:
        return NULL;
    }
}

int hu_trust_directive_is_worth_emitting(const hu_trust_decision_t *d) {
    if (!d) {
        return 0;
    }
    return d->action == HU_TRUST_ANSWER ? 0 : 1;
}

hu_error_t hu_trust_build_directive(hu_allocator_t *alloc, const hu_trust_decision_t *decision,
                                    char **out, size_t *out_len) {
    if (!alloc || !out || !out_len) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    *out = NULL;
    *out_len = 0;
    if (!decision || !hu_trust_directive_is_worth_emitting(decision)) {
        return HU_OK;
    }
    const char *directive = tp_directive_for_action(decision->action);
    if (!directive) {
        return HU_OK;
    }

    char buf[384];
    int n = snprintf(buf, sizeof(buf),
                     "\n\n[Trust: %s — %s\n  Rationale: %s. Firmness: %d%%.]",
                     hu_trust_action_name(decision->action), directive,
                     decision->rationale[0] != '\0' ? decision->rationale : "calibrated",
                     (int)(decision->firmness * 100.f));
    if (n <= 0) {
        return HU_ERR_INTERNAL;
    }
    if ((size_t)n >= sizeof(buf)) {
        n = (int)sizeof(buf) - 1;
    }
    char *copy = (char *)alloc->alloc(alloc->ctx, (size_t)n + 1);
    if (!copy) {
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(copy, buf, (size_t)n);
    copy[n] = '\0';
    *out = copy;
    *out_len = (size_t)n;
    return HU_OK;
}
