/*
 * prosocial_routine.c — scheduled prosocial routines (C-series). Pure scheduler
 * + B0-gated prompt builder. See include/human/agent/prosocial_routine.h and
 * docs/plans/2026-05-29-prosocial-uplift/.
 *
 * Every routine share is routed by the daemon through init_proposer (the
 * silence-biased gate), so this module only DECIDES + PHRASES; it never sends.
 */

#include "human/agent/prosocial_routine.h"

#include "human/behavior/prosocial.h"
#include "human/core/string.h"

#include <string.h>

static bool in_window(int hour, int start, int end) {
    return hour >= start && hour < end;
}

hu_routine_kind_t hu_routine_due(const hu_routine_facts_t *f) {
    if (!f)
        return HU_ROUTINE_NONE;
    if (f->user_active)
        return HU_ROUTINE_NONE; /* hard preemption — never interrupt a conversation */

    /* Weekly check-in has highest precedence (most meaningful, rarest). */
    if (f->day_of_week == HU_ROUTINE_WEEKLY_DOW &&
        in_window(f->local_hour, HU_ROUTINE_WEEKLY_START, HU_ROUTINE_WEEKLY_END) &&
        f->secs_since_weekly >= HU_ROUTINE_WEEKLY_MIN_GAP_SECS)
        return HU_ROUTINE_WEEKLY_CHECKIN;

    if (in_window(f->local_hour, HU_ROUTINE_MORNING_START, HU_ROUTINE_MORNING_END) &&
        f->secs_since_morning >= HU_ROUTINE_DAILY_MIN_GAP_SECS)
        return HU_ROUTINE_MORNING_INTENTION;

    if (in_window(f->local_hour, HU_ROUTINE_EVENING_START, HU_ROUTINE_EVENING_END) &&
        f->secs_since_evening >= HU_ROUTINE_DAILY_MIN_GAP_SECS)
        return HU_ROUTINE_EVENING_REFLECTION;

    /* Thinking-of-you: daytime, only after a long quiet gap. */
    if (in_window(f->local_hour, 10, 20) &&
        f->secs_since_thinking >= HU_ROUTINE_THINKING_MIN_GAP_SECS)
        return HU_ROUTINE_THINKING_OF_YOU;

    return HU_ROUTINE_NONE;
}

char *hu_routine_build_prompt(hu_allocator_t *alloc, hu_routine_kind_t kind,
                              hu_behavior_risk_t dependency_risk, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!alloc || kind == HU_ROUTINE_NONE)
        return NULL;

    /* B0 gate (load-bearing): honest by construction; dependency risk SUPPRESSes
     * a proactive reach-out entirely. */
    hu_prosocial_input_t pin = {
        .claims_feeling = false,
        .praise_grounded = true,
        .overrides_user_need = false,
        .dependency_risk = dependency_risk,
    };
    if (hu_prosocial_gate(&pin, NULL) == HU_PROSOCIAL_SUPPRESS)
        return NULL;

    const char *body;
    switch (kind) {
    case HU_ROUTINE_MORNING_INTENTION:
        body = "It's morning and the user has been quiet. Consider a light, optional "
               "opener inviting them to name one intention for the day. Brief, warm, "
               "zero pressure — easy to ignore. No claimed feelings.";
        break;
    case HU_ROUTINE_EVENING_REFLECTION:
        body = "It's evening. Consider a gentle invitation to reflect on one thing that "
               "went well today. Short and warm, never an obligation. No claimed feelings.";
        break;
    case HU_ROUTINE_WEEKLY_CHECKIN:
        body = "It's the weekly check-in moment. Consider asking how they're really doing "
               "— a genuine, unhurried opener, not a status request. Make it easy to skip. "
               "No claimed feelings, no pressure.";
        break;
    case HU_ROUTINE_THINKING_OF_YOU:
        body = "It's been a while since you talked. Consider a brief, low-key note that "
               "references something specific they mentioned before — a real \"thinking of "
               "you\", grounded, never performative, never a claimed feeling.";
        break;
    default:
        return NULL;
    }

    size_t len = strlen(body);
    char *out = hu_strndup(alloc, body, len);
    if (out && out_len)
        *out_len = len;
    return out;
}
