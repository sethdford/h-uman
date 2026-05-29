/*
 * warm_response.c — warm-response directive builder (B2/B4/B5). B0-gated, like
 * the celebration builder. See include/human/persona/warm_response.h.
 */

#include "human/persona/warm_response.h"

#include "human/behavior/prosocial.h"
#include "human/core/string.h"

#include <string.h>

char *hu_warm_response_build_directive(hu_allocator_t *alloc, hu_pmoment_kind_t kind,
                                       hu_behavior_risk_t dependency_risk, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!alloc || kind == HU_PMOMENT_NONE)
        return NULL;

    /* B0 gate (load-bearing): the directive is honest + grounded by
     * construction; a dependency/attachment risk SUPPRESSes warmth. */
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
    case HU_PMOMENT_ENCOURAGE:
        body = "The user is working toward something or having a hard time. Offer "
               "grounded, specific encouragement — name the real effort you can see, "
               "no empty \"you've got this\", no claimed feelings. Be a steady voice, "
               "not a cheerleader.";
        break;
    case HU_PMOMENT_AFFIRM:
        body = "The user showed genuine effort or character. Affirm it specifically — "
               "name what they actually did and why it counts. Not generic praise, not "
               "flattery, never a claimed feeling.";
        break;
    case HU_PMOMENT_SAVOR:
        body = "The user shared a good moment. Help them savor it — reflect the specific "
               "thing back so it lands, don't rush past it to the next task. Warm and "
               "unhurried, not effusive.";
        break;
    case HU_PMOMENT_GRATITUDE:
        body = "The user expressed appreciation. Receive it warmly and honestly — "
               "acknowledge that the work mattered. Don't deflect it, and do NOT claim "
               "to feel grateful; you can be glad it helped without pretending to feel.";
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
