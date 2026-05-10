#ifndef HU_BEHAVIOR_DIALOG_ACT_H
#define HU_BEHAVIOR_DIALOG_ACT_H

#include <stdbool.h>
#include <stddef.h>

/* B2: Dialogue acts and other-initiated repair detection.
 *
 * Heuristic, lexicon + punctuation based. Coexists with the voice-side
 * `hu_turn_signal_t` (include/human/voice/duplex.h): voice handles real-time
 * floor management; this module classifies semantic acts on text turns.
 */

typedef enum hu_dialog_act {
    HU_DACT_UNKNOWN = 0,
    HU_DACT_BACKCHANNEL,        /* mm, yeah, oh, right */
    HU_DACT_ACKNOWLEDGE,        /* got it, ok, sure */
    HU_DACT_ANSWER,
    HU_DACT_QUESTION,
    HU_DACT_CLARIFY_QUESTION,   /* what do you mean / can you clarify */
    HU_DACT_REPAIR_INITIATE,    /* huh? what? wait, you mean… */
    HU_DACT_REPAIR_ANSWER,
    HU_DACT_REFLECTION,
    HU_DACT_VALIDATION,
    HU_DACT_ADVICE,
    HU_DACT_REMINDER,
    HU_DACT_DISAGREEMENT,
    HU_DACT_BOUNDARY,
    HU_DACT_ABSTENTION,
    HU_DACT_GREETING,
    HU_DACT_FAREWELL,
    HU_DACT_COUNT
} hu_dialog_act_t;

const char *hu_dialog_act_name(hu_dialog_act_t act);

/* Classify a single user/assistant utterance. NULL or empty returns UNKNOWN. */
hu_dialog_act_t hu_dialog_act_classify(const char *text, size_t len);

/* True if the text looks like an other-initiated repair request:
 * "huh?", "what?", "wait,", "i don't follow", "say that again",
 * "you mean…?", a bare "?" after a short utterance.
 *
 * False-positive filter: longer narrative sentences that happen to mention
 * "what" or "wait" do not trigger.
 */
bool hu_dialog_act_is_repair_initiation(const char *text, size_t len);

#endif /* HU_BEHAVIOR_DIALOG_ACT_H */
