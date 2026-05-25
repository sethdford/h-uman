/* src/memory/audio_emotion.c
 *
 * Heuristic stub for voice-message tone classification. See header
 * for rationale on why this is a stub and what real audio infra
 * would add. Sprint B Story 5. */

#include "human/memory/audio_emotion.h"

#include <stdio.h>

/* Threshold tuning — based on phonetics literature on conversational
 * English (typical 120-150 wpm; rapid speech 180+; deliberate <100). */
#define HU_AUDIO_WPM_FAST 180.0
#define HU_AUDIO_WPM_SLOW 100.0
/* Hesitant: a long recording with very few words (lots of pauses). */
#define HU_AUDIO_HESITANT_WPM   50.0
#define HU_AUDIO_HESITANT_MIN_S 5.0

hu_audio_tone_t hu_audio_tone_classify(double duration_seconds, int word_count) {
    if (duration_seconds <= 0.0 || word_count <= 0)
        return HU_AUDIO_TONE_UNKNOWN;
    double wpm = ((double)word_count / duration_seconds) * 60.0;

    /* Hesitant first: long recording with very low rate trumps "slow"
     * classification because the signal is dominantly silence, not
     * deliberate speech. */
    if (duration_seconds >= HU_AUDIO_HESITANT_MIN_S && wpm < HU_AUDIO_HESITANT_WPM)
        return HU_AUDIO_TONE_HESITANT;

    if (wpm < HU_AUDIO_WPM_SLOW)
        return HU_AUDIO_TONE_DELIBERATE;
    if (wpm > HU_AUDIO_WPM_FAST)
        return HU_AUDIO_TONE_ENERGETIC;
    return HU_AUDIO_TONE_NEUTRAL;
}

const char *hu_audio_tone_label(hu_audio_tone_t tone) {
    switch (tone) {
    case HU_AUDIO_TONE_DELIBERATE:
        return "deliberate";
    case HU_AUDIO_TONE_NEUTRAL:
        return "neutral";
    case HU_AUDIO_TONE_ENERGETIC:
        return "energetic";
    case HU_AUDIO_TONE_HESITANT:
        return "hesitant";
    case HU_AUDIO_TONE_UNKNOWN:
    default:
        return "unknown";
    }
}

size_t hu_audio_tone_render(const char *contact_handle, hu_audio_tone_t tone, char *out,
                            size_t cap) {
    if (!contact_handle || !*contact_handle || !out || cap < 16)
        return 0;
    out[0] = '\0';
    if (tone == HU_AUDIO_TONE_UNKNOWN)
        return 0;
    int n = snprintf(out, cap, "VOICE TONE: %s's last voice message sounded %s.", contact_handle,
                     hu_audio_tone_label(tone));
    if (n < 0)
        return 0;
    return (size_t)((size_t)n < cap ? (size_t)n : cap - 1);
}

size_t hu_audio_tone_format_fact(const char *contact_handle, hu_audio_tone_t tone, char *out,
                                 size_t cap) {
    if (!contact_handle || !*contact_handle || !out || cap < 16)
        return 0;
    out[0] = '\0';
    if (tone == HU_AUDIO_TONE_UNKNOWN)
        return 0;
    /* No prompt prefix — this is a canonical-English FACT that the
     * personal-model fact-extractor will parse. Subject is the contact,
     * predicate is "sounded <label>", object is empty. */
    int n = snprintf(out, cap, "%s's voice message sounded %s.", contact_handle,
                     hu_audio_tone_label(tone));
    if (n < 0)
        return 0;
    return (size_t)((size_t)n < cap ? (size_t)n : cap - 1);
}
