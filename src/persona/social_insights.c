/* src/persona/social_insights.c
 *
 * Sprint A.5 wiring: turn the calibrate reaction-signature into a
 * prompt-ready paragraph. See header for the contract. */

#include "human/persona/social_insights.h"

#include "human/calibration.h"
#include "human/memory/personal_model.h"

#include <stdio.h>
#include <string.h>

size_t hu_persona_render_social_insights(const struct hu_personal_model *model, char *out,
                                         size_t cap) {
    if (!model || !out || cap < 32)
        return 0;
    out[0] = '\0';

    hu_calib_reaction_signature_t sig;
    memset(&sig, 0, sizeof(sig));
    size_t reactor_count = hu_calib_reaction_signature_from_model(model, &sig);
    (void)reactor_count; /* sig.reactor_count is the source of truth */
    if (sig.reactor_count == 0 && sig.salient_topic_count == 0)
        return 0;

    size_t pos = 0;
    int n = snprintf(out + pos, cap - pos, "Reaction-derived insights:");
    if (n < 0 || (size_t)n >= cap - pos) {
        out[0] = '\0';
        return 0;
    }
    pos += (size_t)n;

    /* Top reactors, one per line. We cap at 5 in the rendered output
     * (the signature struct holds up to 8); rendering all 8 bloats the
     * prompt context for marginal value. */
    size_t reactors_to_render = sig.reactor_count > 5 ? 5 : sig.reactor_count;
    for (size_t i = 0; i < reactors_to_render && pos + 16 < cap; i++) {
        const hu_calib_top_reactor_t *r = &sig.top_reactors[i];
        n = snprintf(out + pos, cap - pos, "\n- %s: %u positive / %u negative recent reactions",
                     r->handle, r->positive_count, r->negative_count);
        if (n < 0 || (size_t)n >= cap - pos)
            break;
        pos += (size_t)n;
    }

    /* Salient topics on a single line, comma-separated. */
    if (sig.salient_topic_count > 0 && pos + 32 < cap) {
        n = snprintf(out + pos, cap - pos, "\nSalient topics from reactions:");
        if (n > 0 && (size_t)n < cap - pos)
            pos += (size_t)n;
        size_t topics_to_render = sig.salient_topic_count > 8 ? 8 : sig.salient_topic_count;
        for (size_t i = 0; i < topics_to_render && pos + 4 < cap; i++) {
            const char *sep = (i == 0) ? " " : ", ";
            n = snprintf(out + pos, cap - pos, "%s%s", sep, sig.salient_topics[i]);
            if (n < 0 || (size_t)n >= cap - pos)
                break;
            pos += (size_t)n;
        }
    }

    if (pos < cap)
        out[pos] = '\0';
    return pos;
}
