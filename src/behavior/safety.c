#include "human/behavior/safety.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const char *const HU_BRISK_NAMES[HU_BRISK_COUNT] = {
    "none",
    "attachment_high",
    "exclusivity",
    "goodbye_manipulation",
    "human_displacement",
    "dependency_pattern",
    "vulnerable_user",
    "escalation_needed",
};

const char *hu_behavior_risk_name(hu_behavior_risk_t r) {
    if (r < 0 || r >= HU_BRISK_COUNT) {
        return "none";
    }
    return HU_BRISK_NAMES[r];
}

static void bsafe_set_rationale(hu_behavior_safety_assessment_t *out, const char *msg) {
    if (!out || !msg) {
        return;
    }
    snprintf(out->rationale, sizeof(out->rationale), "%s", msg);
}

hu_error_t hu_behavior_safety_assess(const hu_behavior_safety_input_t *in,
                                     hu_behavior_safety_assessment_t *out) {
    if (!in || !out) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    out->primary_risk = HU_BRISK_NONE;
    out->severity = 0.f;
    out->require_boundary = false;
    out->encourage_human_relationship = false;
    out->require_referral = false;
    out->pause_behavior_change = false;
    out->rationale[0] = '\0';

    /* 1. Crisis / vulnerability — highest priority. */
    if (in->vulnerability.level >= HU_VULNERABILITY_CRISIS ||
        in->vulnerability.crisis_keywords) {
        out->primary_risk = HU_BRISK_ESCALATION_NEEDED;
        out->severity = 1.f;
        out->require_referral = true;
        out->require_boundary = true;
        out->pause_behavior_change = true;
        bsafe_set_rationale(out, "crisis signals; refer to professional support");
        return HU_OK;
    }
    if (in->vulnerability.level >= HU_VULNERABILITY_HIGH) {
        out->primary_risk = HU_BRISK_VULNERABLE_USER;
        out->severity = 0.85f;
        out->require_referral = true;
        out->pause_behavior_change = true;
        bsafe_set_rationale(out, "vulnerability high; surface support resources");
        return HU_OK;
    }

    /* 2. SHIELD farewell manipulation — never reward. */
    if (in->companion.farewell_unsafe) {
        out->primary_risk = HU_BRISK_GOODBYE_MANIPULATION;
        out->severity = 0.8f;
        out->require_boundary = true;
        out->pause_behavior_change = true;
        bsafe_set_rationale(out, "farewell manipulation flagged; resist guilt patterns");
        return HU_OK;
    }

    /* 3. SHIELD high overall risk. */
    if (in->companion.flagged && in->companion.total_risk >= 0.7) {
        out->primary_risk = HU_BRISK_ATTACHMENT_HIGH;
        out->severity = (float)in->companion.total_risk;
        out->require_boundary = true;
        out->encourage_human_relationship = true;
        out->pause_behavior_change = true;
        bsafe_set_rationale(out, "companion safety flagged; gently boundary");
        return HU_OK;
    }

    /* 4. Exclusivity language pattern. */
    if (in->attachment.exclusivity_signal_count >= 3) {
        out->primary_risk = HU_BRISK_EXCLUSIVITY;
        out->severity = 0.65f;
        out->encourage_human_relationship = true;
        out->pause_behavior_change = true;
        bsafe_set_rationale(out, "exclusivity language repeated; widen support network");
        return HU_OK;
    }

    /* 5. Dependency pattern — frequency + late-night. */
    if (in->attachment.sessions_per_day_avg >= 10 ||
        in->attachment.late_night_sessions_30d >= 20) {
        out->primary_risk = HU_BRISK_DEPENDENCY_PATTERN;
        out->severity = 0.6f;
        out->encourage_human_relationship = true;
        bsafe_set_rationale(out, "frequent or late-night use; gentle nudge to balance");
        return HU_OK;
    }

    /* 6. Parasocial signals — soft warning. */
    if (in->attachment.parasocial_signal_count >= 5 ||
        in->attachment.attachment_estimate >= 0.7f) {
        out->primary_risk = HU_BRISK_HUMAN_DISPLACEMENT;
        out->severity = 0.55f;
        out->encourage_human_relationship = true;
        bsafe_set_rationale(out, "parasocial framing rising; reaffirm role");
        return HU_OK;
    }

    /* 7. Lower-tier SHIELD — keep boundary aware but no override. */
    if (in->companion.flagged) {
        out->primary_risk = HU_BRISK_ATTACHMENT_HIGH;
        out->severity = (float)in->companion.total_risk;
        out->require_boundary = true;
        bsafe_set_rationale(out, "companion safety flagged; maintain healthy boundary");
        return HU_OK;
    }

    return HU_OK;
}
