/*
 * celebration.c — celebration directive builder (B1b). B0's first consumer:
 * every directive is gated by hu_prosocial_gate, which makes the prosocial
 * integrity guardrail load-bearing. See include/human/persona/celebration.h.
 */

#include "human/persona/celebration.h"

#include "human/behavior/prosocial.h"
#include "human/core/string.h"

#include <string.h>

char *hu_celebration_build_directive(hu_allocator_t *alloc, hu_win_kind_t kind,
                                     hu_behavior_risk_t dependency_risk, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!alloc || kind == HU_WIN_NONE)
        return NULL;

    /* B0 gate (load-bearing). The directive we author is honest by construction
     * (claims_feeling=false), grounded/specific (praise_grounded=true), and does
     * not override a need. The one thing that can still SUPPRESS is a dependency
     * /attachment risk — in which case we do NOT celebrate. */
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
    case HU_WIN_ACHIEVEMENT:
        body = "The user just shared a real accomplishment. Acknowledge it "
               "specifically and warmly — name what it actually took. No generic "
               "\"congrats\", no claimed feelings, no flattery: grounded, earned warmth.";
        break;
    case HU_WIN_MILESTONE:
        body = "The user reached a meaningful milestone. Mark it warmly and "
               "specifically — reflect why it matters to them. Honest warmth, "
               "not a greeting-card line, and never a claimed feeling.";
        break;
    case HU_WIN_GOOD_NEWS:
        body = "The user shared good news. Meet it with genuine, grounded warmth "
               "— celebrate the specific thing, don't flatten it into \"that's "
               "nice.\" Be real, not effusive; never claim to feel an emotion.";
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
