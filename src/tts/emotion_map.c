/*
 * Emotion map — maps conversation emotion/energy to Cartesia emotion strings.
 */
#include "human/tts/cartesia.h"
#include <stddef.h>
#include <string.h>

#if defined(HU_ENABLE_CARTESIA)

const char *hu_cartesia_emotion_from_context(const char *detected_emotion, int energy_level) {
    if (!detected_emotion || !detected_emotion[0])
        return "content";

    if (strcmp(detected_emotion, "excited") == 0 || strcmp(detected_emotion, "happy") == 0)
        return "excited";
    if (strcmp(detected_emotion, "sad") == 0 || strcmp(detected_emotion, "melancholic") == 0)
        return "sympathetic";
    if (strcmp(detected_emotion, "stressed") == 0 || strcmp(detected_emotion, "anxious") == 0)
        return "calm";
    if (strcmp(detected_emotion, "angry") == 0 || strcmp(detected_emotion, "frustrated") == 0)
        return "calm";
    if (strcmp(detected_emotion, "nostalgic") == 0)
        return "nostalgic";
    if (strcmp(detected_emotion, "playful") == 0)
        return "joking/comedic";

    /* Energy-based fallback */
    if (energy_level == 2)
        return "excited";
    if (energy_level == 0)
        return "calm";

    return "content";
}

#endif /* HU_ENABLE_CARTESIA */
