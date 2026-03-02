#ifndef SC_VOICE_H
#define SC_VOICE_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

typedef struct sc_voice_config {
    const char *api_key;
    size_t api_key_len;
    const char *stt_endpoint;   /* NULL = default (Groq) */
    const char *tts_endpoint;   /* NULL = default (OpenAI) */
    const char *stt_model;      /* NULL = "whisper-large-v3" */
    const char *tts_model;       /* NULL = "tts-1" */
    const char *tts_voice;      /* NULL = "alloy" */
    const char *language;       /* NULL = auto-detect */
} sc_voice_config_t;

sc_error_t sc_voice_stt_file(sc_allocator_t *alloc, const sc_voice_config_t *config,
    const char *file_path, char **out_text, size_t *out_len);

sc_error_t sc_voice_stt(sc_allocator_t *alloc, const sc_voice_config_t *config,
    const void *audio_data, size_t audio_len,
    char **out_text, size_t *out_len);

sc_error_t sc_voice_tts(sc_allocator_t *alloc, const sc_voice_config_t *config,
    const char *text, size_t text_len,
    void **out_audio, size_t *out_audio_len);

#endif /* SC_VOICE_H */
