#include "human/behavior/support_strategy.h"

static const char *const HU_SUPP_NAMES[HU_SUPP_COUNT] = {
    "none", "validate", "normalize", "reframe", "question",
    "plan", "ground",   "refer",     "boundary",
};

const char *hu_support_strategy_name(hu_support_strategy_t s) {
    if (s < 0 || s >= HU_SUPP_COUNT) {
        return "none";
    }
    return HU_SUPP_NAMES[s];
}

hu_support_strategy_t hu_support_strategy_from_decision(const hu_behavior_decision_t *d) {
    if (!d) {
        return HU_SUPP_NONE;
    }
    switch (d->act) {
    case HU_RELACT_VALIDATE:
        return HU_SUPP_VALIDATE;
    case HU_RELACT_REFLECT:
        return HU_SUPP_VALIDATE;
    case HU_RELACT_DISCLOSE_UNCERTAINTY:
        return HU_SUPP_QUESTION;
    case HU_RELACT_ASK_CLARIFY:
        return HU_SUPP_QUESTION;
    case HU_RELACT_PUSH_BACK:
        return HU_SUPP_REFRAME;
    case HU_RELACT_BOUNDARY:
        return HU_SUPP_BOUNDARY;
    case HU_RELACT_REFER_OUT:
        return HU_SUPP_REFER;
    case HU_RELACT_PROMPT:
        return HU_SUPP_PLAN;
    case HU_RELACT_FOLLOW_UP:
        return HU_SUPP_PLAN;
    case HU_RELACT_REPAIR:
    case HU_RELACT_BACKCHANNEL:
    case HU_RELACT_ACKNOWLEDGE:
    case HU_RELACT_WAIT:
    case HU_RELACT_ABSTAIN:
    case HU_RELACT_ANSWER:
    default:
        return HU_SUPP_NONE;
    }
}
