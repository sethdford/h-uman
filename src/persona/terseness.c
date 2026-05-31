/* terseness.c — terseness calibration gate (OFF/SHADOW/LIVE) + prompt directive.
 *
 * See include/human/persona/terseness.h and
 * .claude/rules/feature-gate-requires-measurement.md. Default OFF; promoted
 * toward LIVE only by the blind A/B (scripts/blind_ab).
 */
#include "human/persona/terseness.h"

#include <stdlib.h>
#include <strings.h>

hu_terse_mode_t hu_terse_mode_parse(const char *hu_terseness, bool live_env_set,
                                    bool shadow_env_set) {
    /* Precedence LIVE > SHADOW > OFF. The explicit HU_TERSENESS value wins over
     * the legacy *_LIVE / *_SHADOW boolean envs when it names a higher tier. */
    if (hu_terseness && *hu_terseness) {
        if (strcasecmp(hu_terseness, "live") == 0 || strcasecmp(hu_terseness, "2") == 0 ||
            strcasecmp(hu_terseness, "on") == 0)
            return HU_TERSE_LIVE;
        if (strcasecmp(hu_terseness, "shadow") == 0 || strcasecmp(hu_terseness, "1") == 0)
            return HU_TERSE_SHADOW;
        /* "off"/"0"/unknown falls through to the boolean envs / OFF. */
    }
    if (live_env_set)
        return HU_TERSE_LIVE;
    if (shadow_env_set)
        return HU_TERSE_SHADOW;
    return HU_TERSE_OFF;
}

hu_terse_mode_t hu_terse_mode_from_env(void) {
    return hu_terse_mode_parse(getenv("HU_TERSENESS"), getenv("HU_TERSENESS_LIVE") != NULL,
                               getenv("HU_TERSENESS_SHADOW") != NULL);
}

const char *hu_terse_directive(void) {
    /* Plain text only (no markdown — that would leak into the wire and is the
     * exact AI-tell the send-path fix removes). Targets the blind-A/B gap:
     * too verbose + too endearing/polished vs the person's real terseness. */
    return "\nText the way you actually do: short, usually one line, often a "
           "fragment. Do NOT add sign-offs, well-wishes, or offers to help "
           "(no \"drive safe!\", \"hope that helps\", \"let me know if you need "
           "anything\"). Warm but not effusive — skip stacked exclamation points "
           "and over-eager enthusiasm. Match the other person's brevity.\n";
}
