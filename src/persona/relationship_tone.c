/* src/persona/relationship_tone.c — relationship-based tone note predicate.
 *
 * See include/human/persona/relationship_tone.h for the contract and the
 * 2026-07-11 measurement that motivated the warmth-vocabulary branch.
 */
#include "human/persona/relationship_tone.h"

#include "human/core/string.h"

#include <string.h>

const char *hu_persona_relationship_tone_note(const hu_contact_profile_t *cp,
                                              bool warmth_vocab_enabled) {
    if (!cp)
        return NULL;

    /* Legacy vocabulary — production semantics, always honored. */
    if (cp->relationship_stage && strstr(cp->relationship_stage, "deep"))
        return "\n\n[Relationship: deep — be genuinely present, "
               "anticipate needs, use shared references freely.]";
    if (cp->relationship_stage && strstr(cp->relationship_stage, "trusted"))
        return "\n\n[Relationship: trusted — be candid and proactive. "
               "Share insights freely and be direct.]";
    if (cp->relationship_stage && strstr(cp->relationship_stage, "familiar"))
        return "\n\n[Relationship: familiar — reference past conversations "
               "when relevant. Be warmer than default.]";
    if (cp->warmth_level && strstr(cp->warmth_level, "intimate"))
        return "\n\n[Relationship: intimate — respond with genuine warmth "
               "and personal connection. Use inside references.]";

    if (!warmth_vocab_enabled)
        return NULL;

    /* Persona's real warmth vocabulary {high/warm/close, moderate, low}.
     * Word-boundary matched so "lukewarm"/"unfriendly"-shaped values never
     * misroute (substring-classifier-pitfalls). */
    if (cp->warmth_level && *cp->warmth_level) {
        if (hu_str_contains_word_ci(cp->warmth_level, "high") ||
            hu_str_contains_word_ci(cp->warmth_level, "warm") ||
            hu_str_contains_word_ci(cp->warmth_level, "close"))
            return "\n\n[Relationship: warm — this is someone you're close to. "
                   "Respond with genuine warmth and ease; inside references and "
                   "affection are welcome.]";
        if (hu_str_contains_word_ci(cp->warmth_level, "moderate"))
            return "\n\n[Relationship: friendly — comfortable and familiar, "
                   "warmer than a stranger but not effusive.]";
        /* low / cold → no note: keep the default register. */
    }
    return NULL;
}
