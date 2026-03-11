#ifndef HU_CONTEXT_VOICE_DECISION_H
#define HU_CONTEXT_VOICE_DECISION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_voice_decision {
    HU_VOICE_SEND_TEXT,
    HU_VOICE_SEND_VOICE,
} hu_voice_decision_t;

/* Classify when to send voice vs text based on response, incoming message, config, and context. */
hu_voice_decision_t hu_voice_decision_classify(
    const char *response_text, size_t response_len,
    const char *incoming_msg, size_t incoming_len,
    bool voice_enabled, const char *frequency,
    uint8_t hour_local, uint32_t seed);

#endif /* HU_CONTEXT_VOICE_DECISION_H */
