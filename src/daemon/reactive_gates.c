/* llm_decides gate split — see include/human/daemon/reactive_gates.h. */
#include "human/daemon/reactive_gates.h"
#include "human/core/string.h"

#include <stddef.h>

bool hu_reactive_gate_is_safety(hu_reactive_gate_t gate) {
    /* Only the heuristic gates are enumerated: everything else, including an
     * unknown value, is treated as safety (fail closed — an unclassified gate
     * stays on). */
    switch (gate) {
    case HU_REACTIVE_GATE_RESPONSE_MODE:
    case HU_REACTIVE_GATE_DROP_OFF:
    case HU_REACTIVE_GATE_TAPBACK_SKIP:
    case HU_REACTIVE_GATE_LEAVE_ON_READ:
    case HU_REACTIVE_GATE_CONSTITUTIONAL:
        return false;
    default:
        return true;
    }
}

bool hu_reactive_gate_active(hu_reactive_gate_t gate, bool llm_decides) {
    if (hu_reactive_gate_is_safety(gate))
        return true;
    return !llm_decides;
}

/* Assistant-register tells. Substring, case-insensitive. Keep entries specific
 * enough not to catch the persona's own register ("sorry just saw this",
 * "my bad") — "sorry to hear" is a tell, "sorry" alone is not. */
static const char *const k_ai_tells[] = {
    /* legacy list (moved from src/daemon.c) */
    "I understand how you",
    "I am here to support",
    "I am here for you",
    "that must be really",
    "I appreciate you sharing",
    "feel free to",
    "I hear you",
    "I'd be happy to",
    "sorry to hear",
    "going through that",
    "here to support",
    "I can only imagine",
    "According to the available",
    "According to my",
    "significant negative impact",
    "fail to account for",
    /* 2026-09-01 incident: reached real contacts under llm_decides. Qualified
     * forms only — "I apologize for the mistake!" is real Seth text
     * (data/eval_blinded_ab.json). */
    "I apologize for the delay",
    "I apologize for any",
    "I apologize for the confusion",
    "I understand you're",
    "I understand you are",
    "please clarify",
    "delay in responding",
    "experiencing these feelings",
};

hu_ai_tell_action_t hu_reactive_ai_tell_action(const char *ai_tell, bool retried) {
    if (!ai_tell)
        return HU_AI_TELL_SEND;
    return retried ? HU_AI_TELL_DROP : HU_AI_TELL_RETRY;
}

const char *hu_reactive_response_ai_tell(const char *response) {
    if (!response || !response[0])
        return NULL;
    for (size_t i = 0; i < sizeof(k_ai_tells) / sizeof(k_ai_tells[0]); i++) {
        if (hu_strcasestr(response, k_ai_tells[i]))
            return k_ai_tells[i];
    }
    return NULL;
}
