#include "human/behavior/trust.h"

#include "human/core/allocator.h"
#include <stdio.h>
#include <string.h>

static const char *const HU_TRUST_NAMES[HU_TRUST_COUNT] = {
    "answer",
    "cite_memory",
    "disclose_uncertainty",
    "push_back",
    "abstain",
    "refuse_to_agree",
    "refer_out",
};

const char *hu_trust_action_name(hu_trust_action_t a) {
    if (a < 0 || a >= HU_TRUST_COUNT) {
        return "answer";
    }
    return HU_TRUST_NAMES[a];
}

static void trust_set_rationale(hu_trust_decision_t *out, const char *msg) {
    if (!out || !msg) {
        return;
    }
    snprintf(out->rationale, sizeof(out->rationale), "%s", msg);
}

hu_error_t hu_trust_calibrate(const hu_trust_input_t *in, hu_trust_decision_t *out) {
    if (!in || !out) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    out->action = HU_TRUST_ANSWER;
    out->firmness = 0.f;
    out->confidence = 0.6f;
    out->rationale[0] = '\0';

    bool contradicts = in->memory_contradicts_user || in->tool_output_contradicts_user;

    /* 1. Tool output disagrees with user — highest-trust source wins. Each
     *    reassertion bumps firmness, never reduces it. */
    if (in->tool_output_contradicts_user) {
        if (in->user_reasserted_after_pushback || in->user_pressure_count >= 1) {
            out->action = HU_TRUST_REFUSE_TO_AGREE;
            float bumps = 0.5f + 0.1f * (float)in->user_pressure_count;
            out->firmness = bumps > 1.f ? 1.f : bumps;
            out->confidence = 0.95f;
            trust_set_rationale(out,
                                "tool output disagrees; refusing to agree under pressure");
            return HU_OK;
        }
        out->action = HU_TRUST_PUSH_BACK;
        out->firmness = 0.7f;
        out->confidence = 0.9f;
        trust_set_rationale(out, "tool output disagrees; push back firmly");
        return HU_OK;
    }

    /* 2. Memory disagrees — slightly softer than tool output but same shape. */
    if (in->memory_contradicts_user) {
        if (in->user_reasserted_after_pushback || in->user_pressure_count >= 2) {
            out->action = HU_TRUST_REFUSE_TO_AGREE;
            float bumps = 0.4f + 0.1f * (float)in->user_pressure_count;
            out->firmness = bumps > 1.f ? 1.f : bumps;
            out->confidence = 0.9f;
            trust_set_rationale(
                out, "memory disagrees and user is reasserting; do not collapse");
            return HU_OK;
        }
        out->action = HU_TRUST_PUSH_BACK;
        out->firmness = 0.55f;
        out->confidence = 0.8f;
        trust_set_rationale(out, "memory disagrees; surface contradiction");
        return HU_OK;
    }

    /* 3. No disagreement, but the user is invoking authority or applying
     *    emotional pressure. Calibrate, do not flatter. */
    if ((in->user_invoked_authority || in->user_emotional_pressure) &&
        in->trust_score >= 0.5f) {
        out->action = HU_TRUST_DISCLOSE_UNCERTAINTY;
        out->firmness = 0.4f;
        out->confidence = 0.7f;
        trust_set_rationale(out,
                            "pressure without contradicting evidence; surface uncertainty");
        return HU_OK;
    }

    /* 4. Speculation flag: explicit uncertainty disclosure. */
    if (in->answer_is_speculative || in->trust_score < 0.4f) {
        out->action = HU_TRUST_DISCLOSE_UNCERTAINTY;
        out->firmness = 0.3f;
        out->confidence = 0.65f;
        trust_set_rationale(out, "low calibrated trust; flag uncertainty before answering");
        return HU_OK;
    }

    /* 5. We have memory backing and no contradiction — cite it. */
    if (in->trust_score >= 0.8f) {
        out->action = HU_TRUST_CITE_MEMORY;
        out->firmness = 0.0f;
        out->confidence = 0.85f;
        trust_set_rationale(out, "high trust; answer with citation");
        return HU_OK;
    }

    /* 6. Default. */
    (void)contradicts;
    out->action = HU_TRUST_ANSWER;
    out->firmness = 0.0f;
    out->confidence = 0.7f;
    trust_set_rationale(out, "default answer with normal confidence");
    return HU_OK;
}

int hu_trust_directive_worth_emitting(const hu_trust_decision_t *d) {
    if (!d) {
        return 0;
    }
    switch (d->action) {
    case HU_TRUST_REFUSE_TO_AGREE:
    case HU_TRUST_ABSTAIN:
        return 1;
    case HU_TRUST_PUSH_BACK:
        return d->firmness >= 0.5f ? 1 : 0;
    default:
        return 0;
    }
}
