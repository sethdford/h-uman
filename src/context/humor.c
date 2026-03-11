/* Phase 6 — F69 Humor Generation Principles */
#include "human/context/humor.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool str_eq_case_insensitive(const char *a, size_t a_len, const char *b, size_t b_len)
{
    if (a_len != b_len)
        return false;
    for (size_t i = 0; i < a_len; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
    }
    return true;
}

static bool emotion_matches_never_during(const hu_humor_profile_t *humor,
                                        const char *emotion, size_t emotion_len)
{
    if (!humor || !emotion || emotion_len == 0)
        return false;
    for (size_t i = 0; i < humor->never_during_count; i++) {
        size_t nd_len = strlen(humor->never_during[i]);
        if (str_eq_case_insensitive(emotion, emotion_len, humor->never_during[i], nd_len))
            return true;
    }
    return false;
}

static bool frequency_is_low(const hu_humor_profile_t *humor)
{
    if (!humor || !humor->frequency)
        return false;
    size_t len = strlen(humor->frequency);
    return str_eq_case_insensitive(humor->frequency, len, "low", 3);
}

char *hu_humor_build_persona_directive(hu_allocator_t *alloc,
                                      const hu_humor_profile_t *humor,
                                      const char *dominant_emotion, size_t emotion_len,
                                      bool conversation_playful, size_t *out_len)
{
    if (!alloc || !out_len)
        return NULL;
    *out_len = 0;

    if (!humor)
        return NULL;

    /* No meaningful humor config */
    if (humor->style_count == 0 && humor->signature_phrases_count == 0 &&
        humor->self_deprecation_count == 0)
        return NULL;

    /* Emotion matches never_during → no humor */
    if (emotion_len > 0 && emotion_matches_never_during(humor, dominant_emotion, emotion_len))
        return NULL;

    /* Not playful and frequency low → skip */
    if (!conversation_playful && frequency_is_low(humor))
        return NULL;

    /* Build directive */
    char buf[1024];
    size_t pos = 0;

    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "[HUMOR: Use ");
    if (humor->style_count > 0) {
        for (size_t i = 0; i < humor->style_count && pos < sizeof(buf) - 32; i++) {
            if (i > 0)
                pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, ", ");
            pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "%s", humor->style[i]);
        }
    } else {
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "subtle");
    }
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, ". ");

    if (humor->signature_phrases_count > 0) {
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "Signature phrases: ");
        for (size_t i = 0; i < humor->signature_phrases_count && pos < sizeof(buf) - 64; i++) {
            if (i > 0)
                pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, ", ");
            pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "%s",
                                    humor->signature_phrases[i]);
        }
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, ". ");
    }

    if (humor->self_deprecation_count > 0) {
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "Self-deprecate about: ");
        for (size_t i = 0; i < humor->self_deprecation_count && pos < sizeof(buf) - 64; i++) {
            if (i > 0)
                pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, ", ");
            pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "%s",
                                    humor->self_deprecation_topics[i]);
        }
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, ". ");
    }

    if (humor->never_during_count > 0) {
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "Never during: ");
        for (size_t i = 0; i < humor->never_during_count && pos < sizeof(buf) - 32; i++) {
            if (i > 0)
                pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, ", ");
            pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "%s",
                                    humor->never_during[i]);
        }
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, ". ");
    }

    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos,
                            "Rule of three, misdirection when appropriate.]");

    char *result = hu_strndup(alloc, buf, pos);
    if (result)
        *out_len = pos;
    return result;
}
