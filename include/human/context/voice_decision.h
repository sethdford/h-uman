#ifndef HU_CONTEXT_VOICE_DECISION_H
#define HU_CONTEXT_VOICE_DECISION_H

#include "human/persona.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_voice_decision {
    HU_VOICE_SEND_TEXT,
    HU_VOICE_SEND_VOICE,
} hu_voice_decision_t;

/* Classify when to send voice vs text based on response, incoming message, config, and context. */
hu_voice_decision_t hu_voice_decision_classify(const char *response_text, size_t response_len,
                                               const char *incoming_msg, size_t incoming_len,
                                               const hu_voice_messages_config_t *voice_msg_config,
                                               bool has_voice_id, int hour_local, uint32_t seed);

/* Same classification, plus WHY: one of "no_voice_id", "disabled",
 * "incoming_question", "response_short", "logistics", "too_long",
 * "no_prefer_boost", "roll_miss", "voice". Static strings, never NULL when
 * out_reason is non-NULL. Logged by the daemon into proactive_decisions
 * (trigger='voice_reply') so voice timing gets the same When2Speak
 * measurement as proactive sends. */
hu_voice_decision_t
hu_voice_decision_classify_ex(const char *response_text, size_t response_len,
                              const char *incoming_msg, size_t incoming_len,
                              const hu_voice_messages_config_t *voice_msg_config, bool has_voice_id,
                              int hour_local, uint32_t seed, const char **out_reason);

#endif /* HU_CONTEXT_VOICE_DECISION_H */
