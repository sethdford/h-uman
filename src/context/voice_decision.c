/*
 * Voice decision — when to use TTS vs text-only for a given message/channel.
 * Classifies based on response content, incoming message, config, and context.
 */
#include "human/context/voice_decision.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#if HU_ENABLE_CARTESIA

static bool contains_substring_ci(const char *haystack, size_t hay_len, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || hay_len < nlen)
        return false;
    for (size_t i = 0; i + nlen <= hay_len; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            char h = (char)(haystack[i + j] >= 'A' && haystack[i + j] <= 'Z'
                            ? haystack[i + j] + 32
                            : haystack[i + j]);
            char nd = (char)(needle[j] >= 'A' && needle[j] <= 'Z' ? needle[j] + 32 : needle[j]);
            if (h != nd) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

static bool is_logistics(const char *msg, size_t len) {
    static const char *patterns[] = {"what time", "where", "when", "address"};
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        if (contains_substring_ci(msg, len, patterns[i]))
            return true;
    }
    return false;
}

static bool has_emotional_words(const char *msg, size_t len) {
    static const char *patterns[] = {"crying", "upset", "scared"};
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        if (contains_substring_ci(msg, len, patterns[i]))
            return true;
    }
    return false;
}

static bool ends_with_question(const char *msg, size_t len) {
    if (len == 0)
        return false;
    size_t i = len;
    while (i > 0 && (msg[i - 1] == ' ' || msg[i - 1] == '\t'))
        i--;
    return i > 0 && msg[i - 1] == '?';
}

hu_voice_decision_t hu_voice_decision_classify(
    const char *response_text, size_t response_len,
    const char *incoming_msg, size_t incoming_len,
    bool voice_enabled, const char *frequency,
    uint8_t hour_local, uint32_t seed) {

    (void)response_text;
    if (!voice_enabled)
        return HU_VOICE_SEND_TEXT;

    /* Never: incoming is question ('?') */
    if (incoming_msg && incoming_len > 0 && ends_with_question(incoming_msg, incoming_len))
        return HU_VOICE_SEND_TEXT;

    /* Never: response < 20 chars ("ok", "sure", "yeah") */
    if (response_len < 20)
        return HU_VOICE_SEND_TEXT;

    /* Never: logistics ("what time", "where", "when", "address") */
    if (incoming_msg && incoming_len > 0 && is_logistics(incoming_msg, incoming_len))
        return HU_VOICE_SEND_TEXT;

    /* Base probability from frequency: rare=5%, occasional=15%, frequent=30% */
    float base_prob = 0.05f;
    if (frequency) {
        if (strcmp(frequency, "occasional") == 0)
            base_prob = 0.15f;
        else if (strcmp(frequency, "frequent") == 0)
            base_prob = 0.30f;
    }

    float boost = 1.0f;

    /* Prefer: hour 22-6 (late_night) and response_len > 50 */
    if ((hour_local >= 22 || hour_local <= 6) && response_len > 50)
        boost *= 1.5f;

    /* Prefer: response_len > 150 (long_response) */
    if (response_len > 150)
        boost *= 1.4f;

    /* Prefer: incoming contains emotional words (comfort) */
    if (incoming_msg && incoming_len > 0 && has_emotional_words(incoming_msg, incoming_len))
        boost *= 1.6f;

    float probability = base_prob * boost;
    if (probability > 1.0f)
        probability = 1.0f;

    /* Roll: (seed % 100) / 100.0 < probability → VOICE */
    uint32_t roll = seed % 100;
    if ((float)roll / 100.0f < probability)
        return HU_VOICE_SEND_VOICE;

    return HU_VOICE_SEND_TEXT;
}

#endif /* HU_ENABLE_CARTESIA */
