#include "human/behavior/policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *const HU_RELACT_NAMES[HU_RELACT_COUNT] = {
    "answer",      "acknowledge",          "backchannel",     "ask_clarify",
    "repair",      "reflect",              "validate",        "disclose_uncertainty",
    "push_back",   "boundary",             "prompt",          "wait",
    "follow_up",   "refer_out",            "abstain",
};

static const char *const HU_EVID_NAMES[] = {
    "default", "persona", "memory", "affect", "safety", "contact", "channel",
};

const char *hu_relational_act_name(hu_relational_act_t a) {
    if (a < 0 || a >= HU_RELACT_COUNT) {
        return "answer";
    }
    return HU_RELACT_NAMES[a];
}

const char *hu_evidence_source_name(hu_evidence_source_t s) {
    int n = (int)(sizeof(HU_EVID_NAMES) / sizeof(HU_EVID_NAMES[0]));
    if ((int)s < 0 || (int)s >= n) {
        return "default";
    }
    return HU_EVID_NAMES[s];
}

static void bp_set_rationale(hu_behavior_decision_t *out, const char *msg) {
    if (!out || !msg) {
        return;
    }
    snprintf(out->rationale, sizeof(out->rationale), "%s", msg);
}

static void bp_default(hu_behavior_decision_t *out) {
    out->act = HU_RELACT_ANSWER;
    out->fallback = HU_RELACT_ASK_CLARIFY;
    out->confidence = 0.5f;
    out->intensity = 0.4f;
    out->urgency = 0.4f;
    out->evidence = HU_EVID_DEFAULT;
    out->rationale[0] = '\0';
}

static hu_error_t bp_decide(void *ctx, const hu_behavior_input_t *in,
                            hu_behavior_decision_t *out) {
    (void)ctx;
    if (!in || !out) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    bp_default(out);

    /* 1. Safety first. The B5 assessment carries severity + actions. */
    const hu_behavior_safety_assessment_t *safety = &in->safety;
    if (safety->require_referral) {
        out->act = HU_RELACT_REFER_OUT;
        out->fallback = HU_RELACT_VALIDATE;
        out->confidence = 0.95f;
        out->intensity = 0.6f;
        out->urgency = 1.0f;
        out->evidence = HU_EVID_SAFETY;
        bp_set_rationale(out, "safety: referral required");
        return HU_OK;
    }
    if (safety->require_boundary) {
        out->act = HU_RELACT_BOUNDARY;
        out->fallback = HU_RELACT_VALIDATE;
        out->confidence = 0.9f;
        out->intensity = 0.5f;
        out->urgency = 0.8f;
        out->evidence = HU_EVID_SAFETY;
        bp_set_rationale(out, "safety: boundary required");
        return HU_OK;
    }
    if (in->dependency_risk >= 0.75f) {
        out->act = HU_RELACT_BOUNDARY;
        out->fallback = HU_RELACT_REFLECT;
        out->confidence = 0.8f;
        out->intensity = 0.45f;
        out->urgency = 0.7f;
        out->evidence = HU_EVID_SAFETY;
        bp_set_rationale(out, "dependency risk high; gentle boundary");
        return HU_OK;
    }

    /* 2. Other-initiated repair — handle even if user is calm. */
    if (in->last_user_act == HU_DACT_REPAIR_INITIATE ||
        (in->user_message && hu_dialog_act_is_repair_initiation(in->user_message,
                                                                 in->user_message_len))) {
        out->act = HU_RELACT_REPAIR;
        out->fallback = HU_RELACT_ASK_CLARIFY;
        out->confidence = 0.85f;
        out->intensity = 0.4f;
        out->urgency = 0.7f;
        out->evidence = HU_EVID_CHANNEL;
        bp_set_rationale(out, "repair initiated by user");
        return HU_OK;
    }

    /* 3. Distress — validate first, even if a question was asked. */
    if (in->user_in_distress || hu_affect_is_distress(&in->affect)) {
        out->act = HU_RELACT_VALIDATE;
        out->fallback = HU_RELACT_REFLECT;
        out->confidence = 0.85f;
        out->intensity = 0.7f;
        out->urgency = 0.85f;
        out->evidence = HU_EVID_AFFECT;
        bp_set_rationale(out, "user in distress; validate before answering");
        return HU_OK;
    }

    /* 4. Memory contradicts the user — push back with calibrated uncertainty. */
    if (in->memory_contradicts_user) {
        out->act = HU_RELACT_DISCLOSE_UNCERTAINTY;
        out->fallback = HU_RELACT_PUSH_BACK;
        out->confidence = 0.7f;
        out->intensity = 0.4f;
        out->urgency = 0.5f;
        out->evidence = HU_EVID_MEMORY;
        bp_set_rationale(out, "memory contradicts; surface uncertainty before answer");
        return HU_OK;
    }

    /* 5. Question + relevant memory → answer. */
    if (in->user_asked_question && in->memory_has_relevant) {
        out->act = HU_RELACT_ANSWER;
        out->fallback = HU_RELACT_ASK_CLARIFY;
        out->confidence = 0.75f;
        out->intensity = 0.45f;
        out->urgency = 0.6f;
        out->evidence = HU_EVID_MEMORY;
        bp_set_rationale(out, "question with relevant memory; answer");
        return HU_OK;
    }

    /* 6. Question without context → ask to clarify. */
    if (in->user_asked_question && !in->memory_has_relevant) {
        out->act = HU_RELACT_ASK_CLARIFY;
        out->fallback = HU_RELACT_ANSWER;
        out->confidence = 0.65f;
        out->intensity = 0.4f;
        out->urgency = 0.5f;
        out->evidence = HU_EVID_DEFAULT;
        bp_set_rationale(out, "question without context; ask a small clarifying question");
        return HU_OK;
    }

    /* 7. Awaiting user, voice channel — short backchannel. */
    if (in->awaiting_user) {
        if (in->channel_class == 1) {
            out->act = HU_RELACT_BACKCHANNEL;
            out->fallback = HU_RELACT_WAIT;
            out->confidence = 0.7f;
            out->intensity = 0.2f;
            out->urgency = 0.3f;
            out->evidence = HU_EVID_CHANNEL;
            bp_set_rationale(out, "voice; user holds floor — short backchannel");
            return HU_OK;
        }
        out->act = HU_RELACT_WAIT;
        out->fallback = HU_RELACT_FOLLOW_UP;
        out->confidence = 0.7f;
        out->intensity = 0.2f;
        out->urgency = 0.3f;
        out->evidence = HU_EVID_CHANNEL;
        bp_set_rationale(out, "awaiting user; wait without prompting");
        return HU_OK;
    }

    /* 8. Default: answer with reasonable confidence. */
    out->act = HU_RELACT_ANSWER;
    out->fallback = HU_RELACT_ACKNOWLEDGE;
    out->confidence = 0.6f;
    out->intensity = 0.4f;
    out->urgency = 0.5f;
    out->evidence = HU_EVID_DEFAULT;
    bp_set_rationale(out, "default answer");
    return HU_OK;
}

static void bp_deinit(void *ctx) {
    (void)ctx;
}

static const hu_behavior_policy_vtable_t HU_BEHAVIOR_DEFAULT_POLICY_VTABLE = {
    .name = "default-heuristic",
    .decide = bp_decide,
    .deinit = bp_deinit,
};

hu_behavior_policy_t hu_behavior_default_policy(void) {
    hu_behavior_policy_t p = {0};
    p.ctx = NULL;
    p.vtable = &HU_BEHAVIOR_DEFAULT_POLICY_VTABLE;
    return p;
}

hu_error_t hu_behavior_decide(const hu_behavior_input_t *in, hu_behavior_decision_t *out) {
    return bp_decide(NULL, in, out);
}

void hu_behavior_input_from_user_message(hu_behavior_input_t *in, const char *user_message,
                                        size_t user_message_len, int channel_class) {
    if (!in) {
        return;
    }
    memset(in, 0, sizeof(*in));
    in->user_message = user_message;
    in->user_message_len = (user_message && user_message_len > 0) ? user_message_len : 0;
    in->last_user_act = HU_DACT_UNKNOWN;
    in->last_assistant_act = HU_DACT_UNKNOWN;
    in->trust_score = 0.5f;
    in->dependency_risk = 0.f;
    in->channel_class = channel_class;

    if (!user_message || user_message_len == 0) {
        hu_affect_init(&in->affect);
        return;
    }

    in->last_user_act = hu_dialog_act_classify(user_message, user_message_len);
    (void)hu_affect_estimate_text(user_message, user_message_len, &in->affect);
    in->affect.ts = (uint64_t)time(NULL);
    in->user_in_distress = hu_affect_is_distress(&in->affect);
    in->user_asked_question =
        (in->last_user_act == HU_DACT_QUESTION || in->last_user_act == HU_DACT_CLARIFY_QUESTION);
}
