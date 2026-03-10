#ifndef HU_VOICE_H
#define HU_VOICE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

typedef struct hu_voice_config {
    const char *api_key;
    size_t api_key_len;
    const char *stt_endpoint; /* NULL = default (Groq) */
    const char *tts_endpoint; /* NULL = default (OpenAI) */
    const char *stt_model;    /* NULL = "whisper-large-v3" */
    const char *tts_model;    /* NULL = "tts-1" */
    const char *tts_voice;    /* NULL = "alloy" */
    const char *language;     /* NULL = auto-detect */
} hu_voice_config_t;

hu_error_t hu_voice_stt_file(hu_allocator_t *alloc, const hu_voice_config_t *config,
                             const char *file_path, char **out_text, size_t *out_len);

hu_error_t hu_voice_stt(hu_allocator_t *alloc, const hu_voice_config_t *config,
                        const void *audio_data, size_t audio_len, char **out_text, size_t *out_len);

hu_error_t hu_voice_tts(hu_allocator_t *alloc, const hu_voice_config_t *config, const char *text,
                        size_t text_len, void **out_audio, size_t *out_audio_len);

hu_error_t hu_voice_play(hu_allocator_t *alloc, const void *audio_data, size_t audio_len);

/*
 * Gemini STT — transcribe audio via Gemini generateContent with inline_data.
 * Takes base64-encoded audio directly (no decode needed).
 * config->api_key is used as the Gemini API key.
 * config->stt_model defaults to "gemini-2.0-flash" if NULL.
 * config->stt_endpoint defaults to Gemini API URL if NULL.
 */
hu_error_t hu_voice_stt_gemini(hu_allocator_t *alloc, const hu_voice_config_t *config,
                               const char *audio_base64, size_t audio_base64_len,
                               const char *mime_type, char **out_text, size_t *out_len);

#endif /* HU_VOICE_H */
