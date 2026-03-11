#ifndef HU_TTS_EMOTION_MAP_H
#define HU_TTS_EMOTION_MAP_H

#include <stddef.h>
#include <stdint.h>

/* Map conversation context (incoming + response + time) to Cartesia emotion.
 * Keyword-based heuristics. Returns pointer to static string. Never NULL (defaults to "content"). */
const char *hu_cartesia_emotion_from_context(
    const char *incoming_msg, size_t msg_len,
    const char *response, size_t resp_len,
    uint8_t hour_local);

#endif /* HU_TTS_EMOTION_MAP_H */
