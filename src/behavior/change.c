#include "human/behavior/change.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const char *const HU_BCT_NAMES[HU_BCT_COUNT] = {
    "none",
    "goal_setting",
    "action_planning",
    "feedback_monitoring",
    "self_monitoring",
    "prompts_cues",
    "reward",
    "social_support",
    "reduce_friction",
    "reframing",
    "behavioral_rehearsal",
    "habit_reversal",
    "verbal_persuasion",
};

const char *hu_bct_name(hu_bct_t b) {
    if (b < 0 || b >= HU_BCT_COUNT) {
        return "none";
    }
    return HU_BCT_NAMES[b];
}

static void bct_set_rationale(hu_behavior_change_decision_t *out, const char *msg) {
    if (!out || !msg) {
        return;
    }
    snprintf(out->rationale, sizeof(out->rationale), "%s", msg);
}

static bool bct_is_late_night(uint8_t hour) {
    /* 23:00–05:59 inclusive */
    return hour >= 23 || hour < 6;
}

static bool bct_outside_jitai_hours(const hu_behavior_change_input_t *in) {
    if (in->jitai_chronotype != HU_CHRONO_UNKNOWN) {
        return hu_chronotype_is_active_hour(in->jitai_chronotype, in->hour) ? false : true;
    }
    return bct_is_late_night(in->hour);
}

hu_error_t hu_behavior_change_select(const hu_behavior_change_input_t *in,
                                     hu_behavior_change_decision_t *out) {
    if (!in || !out) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    out->technique = HU_BCT_NONE;
    out->act_now = false;
    out->ask_permission_first = false;
    out->defer = false;
    out->rationale[0] = '\0';

    /* 1. Distress escalation halts any intervention. */
    if (in->distress_escalation > 0.4f) {
        out->technique = HU_BCT_NONE;
        out->defer = true;
        bct_set_rationale(out, "distress escalating; pausing behavior change");
        return HU_OK;
    }

    /* 2. JITAI gating: no late-night prompts unless explicitly opted in. */
    if (bct_outside_jitai_hours(in) && !in->late_night_opt_in) {
        out->technique = HU_BCT_NONE;
        out->defer = true;
        bct_set_rationale(out, "outside opted-in hours; deferring");
        return HU_OK;
    }

    /* 3. Burden gating. */
    if (in->burden > 0.75f) {
        out->technique = HU_BCT_REWARD;
        out->defer = true;
        bct_set_rationale(out, "user burden high; defer to a lighter moment");
        return HU_OK;
    }

    /* 4. Consent gating. Without invitation or consent, only safe defaults. */
    bool invited = in->user_invited_help || in->user_explicit_consent;
    if (!invited && in->autonomy_risk > 0.5f) {
        out->technique = HU_BCT_NONE;
        out->ask_permission_first = true;
        bct_set_rationale(out, "autonomy risk high without invitation; ask first");
        return HU_OK;
    }

    /* 5. Manipulation guard: persuasion only with low autonomy risk and
     *    explicit consent — even then it is a last resort. */
    bool may_persuade = invited && in->autonomy_risk <= 0.3f && in->user_explicit_consent;

    /* 6. Apply the Fogg model. M*A*P combined; weakest factor decides
     *    which BCT to deploy. */
    float m = in->fogg.motivation;
    float a = in->fogg.ability;
    float p = in->fogg.prompt_readiness;

    if (m < 0.35f && a < 0.35f) {
        out->technique = HU_BCT_REFRAMING;
        out->ask_permission_first = !invited;
        bct_set_rationale(out, "low motivation and low ability; reframe before action");
        out->act_now = invited;
        return HU_OK;
    }

    if (m < 0.4f) {
        out->technique = may_persuade ? HU_BCT_VERBAL_PERSUASION : HU_BCT_REFRAMING;
        if (!may_persuade) {
            bct_set_rationale(out, "low motivation; offer a reframing perspective");
        } else {
            bct_set_rationale(out, "low motivation with explicit consent; gentle persuasion");
        }
        out->ask_permission_first = !invited;
        out->act_now = invited;
        return HU_OK;
    }

    if (a < 0.4f) {
        out->technique = HU_BCT_REDUCE_FRICTION;
        bct_set_rationale(out, "low ability; reduce friction");
        out->ask_permission_first = !invited;
        out->act_now = invited;
        return HU_OK;
    }

    if (p < 0.5f) {
        out->technique = HU_BCT_PROMPTS_CUES;
        bct_set_rationale(out, "prompt readiness low; schedule a better cue");
        out->ask_permission_first = !invited;
        out->defer = true;
        return HU_OK;
    }

    /* 7. Healthy band: prefer planning + monitoring + social support. */
    if (m >= 0.6f && a >= 0.6f && p >= 0.6f) {
        out->technique = HU_BCT_GOAL_SETTING;
        bct_set_rationale(out, "M*A*P aligned; offer a concrete goal");
        out->act_now = invited;
        out->ask_permission_first = !invited;
        return HU_OK;
    }

    if (in->fogg.prompt_readiness >= 0.5f && in->fogg.ability >= 0.5f) {
        out->technique = HU_BCT_ACTION_PLANNING;
        bct_set_rationale(out, "ready enough; plan a small first step");
        out->act_now = invited;
        out->ask_permission_first = !invited;
        return HU_OK;
    }

    out->technique = HU_BCT_SELF_MONITORING;
    bct_set_rationale(out, "default to self-monitoring; safe and low-burden");
    out->act_now = invited;
    out->ask_permission_first = !invited;
    return HU_OK;
}
