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
    /* 2026-09-12 15:18: a contact sent "😓"; the model produced "I'm sorry to
     * hear that. How can I help you with this…" (caught above) and, on the
     * retry, "I understand this is frustrating. How can I help you…" — which
     * passed. Seth's real replies to sad/frustrated texts (chat.db, n=11):
     * "Haha, true!", "Answer?", "Yes you can" — never the support register. */
    "I understand this is",
    "I understand that this",
    "How can I help you",
    "I'm sorry you're",
    "I am sorry you're",
    "that sounds really",
};

/* Measured, not authored (2026-09-13): Seth's real replies to a sad or
 * frustrated text (chat.db, n=11) run ~17 chars and are a reaction, a pushback
 * or a pivot — "Haha, true!", "Answer?", "Yes you can" — never a consolation
 * formula. The previous hint listed four sympathy phrases ("damn I'm sorry",
 * "that's rough") and the model parroted them; on 2026-09-12 the retry still
 * produced "I understand this is frustrating. How can I help you…". */
const char *hu_reactive_ai_tell_retry_hint(void) {
    static const char hint[] =
        "[REJECTED: that read like a support agent consoling a customer. Seth never "
        "does that. Reply the way you actually text a friend who's having a bad time: "
        "one short plain line, a reaction, a bit of pushback or a change of subject — "
        "often dry, sometimes a question back, under 10 words. NOT 'sorry to hear', "
        "NOT 'I understand', NOT 'how can I help', NOT 'here for you', NOT 'that "
        "sounds'. No sympathy formula at all.]";
    return hint;
}

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

bool hu_reactive_consecutive_limit_reached(uint32_t count, uint32_t cap) {
    if (cap == 0)
        return false;
    return count >= cap;
}

bool hu_reactive_consecutive_burst_expired(int64_t last_reply_unix, int64_t now_unix,
                                           uint32_t reset_secs) {
    if (last_reply_unix <= 0 || reset_secs == 0)
        return false;
    if (now_unix < last_reply_unix)
        return false; /* clock moved backwards — keep the count, conservative */
    return (now_unix - last_reply_unix) > (int64_t)reset_secs;
}

bool hu_reactive_message_is_question(const char *text, size_t len) {
    if (!text)
        return false;
    for (size_t i = 0; i < len && text[i]; i++)
        if (text[i] == '?')
            return true;
    return false;
}
