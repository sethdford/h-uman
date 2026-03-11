/*
 * Voice decision — when to use TTS vs text-only for a given message/channel.
 * Classifies based on response content, incoming message, config, and context.
 */
#include "human/context/voice_decision.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#if defined(HU_ENABLE_CARTESIA)

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

static bool ends_with_question(const char *msg, size_t len) {
    if (len == 0)
        return false;
    size_t i = len;
    while (i > 0 && (msg[i - 1] == ' ' || msg[i - 1] == '\t'))
        i--;
    return i > 0 && msg[i - 1] == '?';
}

static bool has_prefer_for_boost(const char *response_text, size_t response_len,
                                 const char *incoming_msg, size_t incoming_len,
                                 const hu_voice_messages_config_t *cfg, int hour_local) {
    (void)incoming_msg;
    (void)incoming_len;
    if (!cfg)
        return false;
    for (size_t i = 0; i < cfg->prefer_for_count; i++) {
        const char *p = cfg->prefer_for[i];
        if (!p || !p[0])
            continue;
        if (strcmp(p, "late_night") == 0) {
            if (hour_local >= 22 || hour_local <= 6)
                return true;
        } else if (strcmp(p, "long_response") == 0) {
            if (response_len > 150)
                return true;
        } else if (strcmp(p, "emotional") == 0) {
            static const char *emotional[] = {"love", "miss", "proud", "sorry", "worried"};
            for (size_t j = 0; j < sizeof(emotional) / sizeof(emotional[0]); j++) {
                if (contains_substring_ci(response_text, response_len, emotional[j]))
                    return true;
            }
        } else if (strcmp(p, "comfort") == 0) {
            /* Incoming must have sad keywords AND response must be comforting */
            static const char *sad[] = {"sad", "upset", "crying", "devastated", "heartbroken",
                                       "depressed", "miserable", "lonely"};
            static const char *comfort[] = {"it'll be ok", "I'm here", "you've got this"};
            bool incoming_sad = false;
            if (incoming_msg && incoming_len > 0) {
                for (size_t j = 0; j < sizeof(sad) / sizeof(sad[0]); j++) {
                    if (contains_substring_ci(incoming_msg, incoming_len, sad[j])) {
                        incoming_sad = true;
                        break;
                    }
                }
            }
            if (incoming_sad) {
                for (size_t j = 0; j < sizeof(comfort) / sizeof(comfort[0]); j++) {
                    if (contains_substring_ci(response_text, response_len, comfort[j]))
                        return true;
                }
            }
        }
    }
    return false;
}

hu_voice_decision_t hu_voice_decision_classify(
    const char *response_text, size_t response_len,
    const char *incoming_msg, size_t incoming_len,
    const hu_voice_messages_config_t *voice_msg_config,
    bool has_voice_id,
    int hour_local,
    uint32_t seed) {

    (void)seed;

    if (!has_voice_id)
        return HU_VOICE_SEND_TEXT;
    if (!voice_msg_config || !voice_msg_config->enabled)
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

    /* Never: response would exceed max_duration_sec (~5 chars/sec speaking rate) */
    if (voice_msg_config->max_duration_sec > 0) {
        uint32_t est_sec = (uint32_t)(response_len / 5);
        if (est_sec > voice_msg_config->max_duration_sec)
            return HU_VOICE_SEND_TEXT;
    }

    /* Prefer for: need at least one boost to consider voice. Otherwise TEXT. */
    if (!has_prefer_for_boost(response_text, response_len, incoming_msg, incoming_len,
                              voice_msg_config, hour_local))
        return HU_VOICE_SEND_TEXT;

    /* Frequency roll: rare=5%, occasional=15%, frequent=30% */
    float base_prob = 0.05f;
    if (voice_msg_config->frequency[0]) {
        if (strcmp(voice_msg_config->frequency, "occasional") == 0)
            base_prob = 0.15f;
        else if (strcmp(voice_msg_config->frequency, "frequent") == 0)
            base_prob = 0.30f;
    }

    uint32_t roll = seed % 100;
    if ((float)roll / 100.0f < base_prob)
        return HU_VOICE_SEND_VOICE;

    return HU_VOICE_SEND_TEXT;
}

#else

hu_voice_decision_t hu_voice_decision_classify(
    const char *response_text, size_t response_len,
    const char *incoming_msg, size_t incoming_len,
    const hu_voice_messages_config_t *voice_msg_config,
    bool has_voice_id,
    int hour_local,
    uint32_t seed) {
    (void)response_text;
    (void)response_len;
    (void)incoming_msg;
    (void)incoming_len;
    (void)voice_msg_config;
    (void)has_voice_id;
    (void)hour_local;
    (void)seed;
    return HU_VOICE_SEND_TEXT;
}

#endif /* HU_ENABLE_CARTESIA */
