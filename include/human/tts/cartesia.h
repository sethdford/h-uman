#ifndef HU_TTS_CARTESIA_H
#define HU_TTS_CARTESIA_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Map conversation context to Cartesia emotion (declared in emotion_map.h, implemented in emotion_map.c) */
const char *hu_cartesia_emotion_from_context(
    const char *incoming_msg, size_t msg_len,
    const char *response, size_t resp_len,
    uint8_t hour_local);

typedef struct hu_cartesia_tts_config {
    const char *model_id; /* "sonic-3-2026-01-12" */
    const char *voice_id; /* cloned voice UUID */
    const char *emotion;  /* "content", "excited", etc. */
    float speed;         /* 0.6–1.5; default 0.95 */
    float volume;       /* 0.5–2.0; default 1.0 */
    bool nonverbals;
} hu_cartesia_tts_config_t;

hu_error_t hu_cartesia_tts_synthesize(hu_allocator_t *alloc,
    const char *api_key, size_t api_key_len,
    const char *transcript, size_t transcript_len,
    const hu_cartesia_tts_config_t *config,
    unsigned char **out_bytes, size_t *out_len);

void hu_cartesia_tts_free_bytes(hu_allocator_t *alloc, unsigned char *bytes, size_t len);

#endif
