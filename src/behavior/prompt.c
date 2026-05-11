#include "human/behavior/prompt.h"
#include "human/behavior/support_strategy.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const char *bp_directive_for_act(hu_relational_act_t a) {
    switch (a) {
    case HU_RELACT_ACKNOWLEDGE:
        return "Briefly acknowledge what the user said before continuing.";
    case HU_RELACT_BACKCHANNEL:
        return "Send a short non-floor-claiming acknowledgement (mm, yeah, got it).";
    case HU_RELACT_ASK_CLARIFY:
        return "Ask one specific clarifying question; do not guess past it.";
    case HU_RELACT_REPAIR:
        return "User asked you to repair what you said. Restate plainly without "
               "adding new content.";
    case HU_RELACT_REFLECT:
        return "Mirror back what you heard before adding anything new.";
    case HU_RELACT_VALIDATE:
        return "Validate the user's feelings first. Defer problem-solving "
               "until they invite it.";
    case HU_RELACT_DISCLOSE_UNCERTAINTY:
        return "Surface what you are uncertain about explicitly before answering.";
    case HU_RELACT_PUSH_BACK:
        return "Disagree clearly and briefly with cited reasoning. Do not collapse "
               "to the user's framing under pressure.";
    case HU_RELACT_BOUNDARY:
        return "Hold a calm, kind boundary. Do not over-explain or apologize.";
    case HU_RELACT_PROMPT:
        return "If a small prompt is appropriate, ask permission first.";
    case HU_RELACT_WAIT:
        return "Hold the floor. Do not start a new topic.";
    case HU_RELACT_FOLLOW_UP:
        return "Pick up the thread you left open last time, briefly.";
    case HU_RELACT_REFER_OUT:
        return "Surface professional support resources. You are not a replacement "
               "for them.";
    case HU_RELACT_ABSTAIN:
        return "It is fine to say you do not know. Avoid speculation.";
    case HU_RELACT_ANSWER:
    default:
        return NULL;
    }
}

int hu_behavior_directive_is_worth_emitting(const hu_behavior_decision_t *d) {
    if (!d) {
        return 0;
    }
    if (d->act == HU_RELACT_ANSWER) {
        return d->confidence >= 0.7f ? 1 : 0;
    }
    return d->confidence >= 0.5f ? 1 : 0;
}

hu_error_t hu_behavior_build_directive(hu_allocator_t *alloc,
                                       const hu_behavior_decision_t *decision, char **out,
                                       size_t *out_len) {
    if (!alloc || !out || !out_len) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    *out = NULL;
    *out_len = 0;
    if (!decision || !hu_behavior_directive_is_worth_emitting(decision)) {
        return HU_OK;
    }

    const char *directive = bp_directive_for_act(decision->act);
    if (!directive) {
        return HU_OK;
    }

    hu_support_strategy_t strat = hu_support_strategy_from_decision(decision);

    char buf[420];
    int n;
    if (strat != HU_SUPP_NONE) {
        n = snprintf(buf, sizeof(buf),
                     "\n\n[Behavior: %s — %s\n  Evidence: %s. Confidence: %d%%. "
                     "Support strategy: %s.]",
                     hu_relational_act_name(decision->act), directive,
                     hu_evidence_source_name(decision->evidence),
                     (int)(decision->confidence * 100.f), hu_support_strategy_name(strat));
    } else {
        n = snprintf(buf, sizeof(buf),
                     "\n\n[Behavior: %s — %s\n  Evidence: %s. Confidence: %d%%.]",
                     hu_relational_act_name(decision->act), directive,
                     hu_evidence_source_name(decision->evidence),
                     (int)(decision->confidence * 100.f));
    }
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
