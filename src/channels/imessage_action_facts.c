#include "human/channels/imessage_action_facts.h"
#include <ctype.h>
#include <string.h>

/* Pure helper: convert formality string to [0..1] float.
 * Maps the persona's overlay->formality string to a scalar for the predicate.
 * Used when the dispatcher (F2) builds hu_conversation_snapshot_t from chat.db +
 * the current persona. */
static float formality_string_to_float(const char *formality) {
    /* Default if missing or empty. */
    if (!formality || !*formality)
        return 0.5f; /* neutral */

    /* Word-boundary matching: avoid "informal" matching "formal" etc.
     * See rules/substring-classifier-pitfalls.md. */
    bool has_formal = false, has_casual = false;

    /* Check for formal-leaning keywords */
    for (size_t i = 0; formality[i]; i++) {
        if (strncasecmp(&formality[i], "formal", 6) == 0) {
            int left_ok = (i == 0) || !isalnum((unsigned char)formality[i - 1]);
            int right_ok = (formality[i + 6] == '\0') || !isalnum((unsigned char)formality[i + 6]);
            if (left_ok && right_ok) {
                has_formal = true;
                break;
            }
        }
    }

    /* Check for casual-leaning keywords */
    if (!has_formal) {
        const char *casual_keywords[] = {"casual", "informal", NULL};
        for (size_t k = 0; casual_keywords[k]; k++) {
            const char *needle = casual_keywords[k];
            size_t nlen = strlen(needle);
            for (size_t i = 0; formality[i]; i++) {
                if (strncasecmp(&formality[i], needle, nlen) == 0) {
                    int left_ok = (i == 0) || !isalnum((unsigned char)formality[i - 1]);
                    int right_ok = (formality[i + nlen] == '\0') ||
                                   !isalnum((unsigned char)formality[i + nlen]);
                    if (left_ok && right_ok) {
                        has_casual = true;
                        break;
                    }
                }
            }
            if (has_casual)
                break;
        }
    }

    if (has_formal)
        return 0.8f;
    if (has_casual)
        return 0.2f;
    return 0.5f; /* neutral if neither formal nor casual */
}

void hu_imessage_build_reply_facts(const hu_conversation_snapshot_t *snapshot,
                                   const hu_persona_t *persona, hu_reply_style_facts_t *facts_out) {
    if (!facts_out)
        return;

    memset(facts_out, 0, sizeof(*facts_out));

    if (snapshot) {
        facts_out->seconds_since_parent = snapshot->parent_seconds_ago;
        facts_out->parent_position_from_bottom = snapshot->parent_position_from_bottom;
        facts_out->parent_was_a_question = snapshot->parent_is_question;
        facts_out->parent_emotional_intensity = snapshot->parent_emotional_intensity;
        facts_out->pending_questions_in_window = snapshot->pending_questions_in_window;
        facts_out->other_threaded_replies_recent = snapshot->other_threaded_replies_recent;
        facts_out->our_threaded_replies_recent = snapshot->our_threaded_replies_recent;
        facts_out->conv_density_msgs_per_min = snapshot->conv_density_msgs_per_min;
    }

    if (persona) {
        /* Find the iMessage overlay to get the persona's formality for this channel.
         * If no overlay exists, use defaults. */
        const hu_persona_overlay_t *overlay = NULL;
        for (size_t i = 0; i < persona->overlays_count; i++) {
            if (persona->overlays[i].channel &&
                strcasecmp(persona->overlays[i].channel, "imessage") == 0) {
                overlay = &persona->overlays[i];
                break;
            }
        }

        if (overlay && overlay->formality) {
            facts_out->persona_formality = formality_string_to_float(overlay->formality);
        } else {
            facts_out->persona_formality = 0.5f; /* neutral default */
        }

        /* persona_thread_affinity defaults to 0.3 for now. Future: read from
         * config via action_surface_v2.thread_affinity_default (A5). */
        facts_out->persona_thread_affinity = 0.3f;
    } else {
        facts_out->persona_formality = 0.5f;
        facts_out->persona_thread_affinity = 0.3f;
    }
}
