/*
 * Emotion map — maps conversation context to Cartesia emotion strings.
 * Keyword-based heuristics. Prefers primary emotions (content, excited, sad, calm, etc.).
 */
#include "human/tts/emotion_map.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#if defined(HU_ENABLE_CARTESIA)

#if defined(_WIN32) || defined(_WIN64)
#define hu_strncasecmp _strnicmp
#else
#include <strings.h>
#define hu_strncasecmp strncasecmp
#endif

static bool contains_substring_ci(const char *haystack, size_t hay_len, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || hay_len < nlen)
        return false;
    for (size_t i = 0; i + nlen <= hay_len; i++) {
        if (hu_strncasecmp(haystack + i, needle, nlen) == 0)
            return true;
    }
    return false;
}

const char *hu_cartesia_emotion_from_context(
    const char *incoming_msg, size_t msg_len,
    const char *response, size_t resp_len,
    uint8_t hour_local) {

    /* Comforting: sad keywords in incoming → sympathetic */
    if (incoming_msg && msg_len > 0) {
        static const char *sad[] = {"sad", "upset", "crying", "devastated", "heartbroken",
                                    "depressed", "miserable", "lonely", "grief"};
        for (size_t i = 0; i < sizeof(sad) / sizeof(sad[0]); i++) {
            if (contains_substring_ci(incoming_msg, msg_len, sad[i]))
                return "sympathetic";
        }
    }

    /* Congratulating: "congrats", "awesome" in response → excited */
    if (response && resp_len > 0) {
        static const char *congrats[] = {"congrats", "congratulations", "awesome", "amazing"};
        for (size_t i = 0; i < sizeof(congrats) / sizeof(congrats[0]); i++) {
            if (contains_substring_ci(response, resp_len, congrats[i]))
                return "excited";
        }
    }

    /* Late night casual (22–6) → calm */
    if (hour_local >= 22 || hour_local <= 6)
        return "calm";

    /* Playful: "lol", "haha", teasing in incoming or response → joking/comedic */
    if ((incoming_msg && msg_len > 0) || (response && resp_len > 0)) {
        static const char *playful[] = {"lol", "haha", "hahaha", "lmao", "😂"};
        const char *text = incoming_msg ? incoming_msg : response;
        size_t text_len = incoming_msg ? msg_len : resp_len;
        if (text) {
            for (size_t i = 0; i < sizeof(playful) / sizeof(playful[0]); i++) {
                if (contains_substring_ci(text, text_len, playful[i]))
                    return "joking/comedic";
            }
        }
    }

    /* Serious/heavy topic: "death", "funeral", "cancer" in incoming or response → contemplative */
    if ((incoming_msg && msg_len > 0) || (response && resp_len > 0)) {
        static const char *serious[] = {"death", "funeral", "cancer", "terminal", "died"};
        const char *text = incoming_msg ? incoming_msg : response;
        size_t text_len = incoming_msg ? msg_len : resp_len;
        if (text) {
            for (size_t i = 0; i < sizeof(serious) / sizeof(serious[0]); i++) {
                if (contains_substring_ci(text, text_len, serious[i]))
                    return "contemplative";
            }
        }
    }

    /* Anxious/worried incoming → calm */
    if (incoming_msg && msg_len > 0) {
        static const char *anxious[] = {"anxious", "worried", "nervous", "scared", "afraid"};
        for (size_t i = 0; i < sizeof(anxious) / sizeof(anxious[0]); i++) {
            if (contains_substring_ci(incoming_msg, msg_len, anxious[i]))
                return "calm";
        }
    }

    return "content";
}

#else

const char *hu_cartesia_emotion_from_context(
    const char *incoming_msg, size_t msg_len,
    const char *response, size_t resp_len,
    uint8_t hour_local) {
    (void)incoming_msg;
    (void)msg_len;
    (void)response;
    (void)resp_len;
    (void)hour_local;
    return "content";
}

#endif /* HU_ENABLE_CARTESIA */
