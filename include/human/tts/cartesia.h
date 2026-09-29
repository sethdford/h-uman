#ifndef HU_TTS_CARTESIA_H
#define HU_TTS_CARTESIA_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Map conversation context to Cartesia emotion (declared in emotion_map.h, implemented in
 * emotion_map.c) */
const char *hu_cartesia_emotion_from_context(const char *incoming_msg, size_t msg_len,
                                             const char *response, size_t resp_len,
                                             uint8_t hour_local);

/* Target container for TTS artifacts. String values are passed to
 * hu_cartesia_tts_synthesize as output_format (lowercase). */
typedef enum hu_tts_output_format {
    HU_TTS_OUTPUT_MP3 = 0,
    HU_TTS_OUTPUT_WAV,
    HU_TTS_OUTPUT_OGG,
    HU_TTS_OUTPUT_CAF,
} hu_tts_output_format_t;

/* Map channel vtable name -> desired format: "caf" (iMessage), "ogg" (telegram/discord —
 * WAV from API until Opus conversion exists), "mp3" (default). */
const char *hu_tts_format_for_channel(const char *channel_name);

typedef struct hu_cartesia_tts_config {
    const char *model_id; /* "sonic-3-2026-01-12" */
    const char *voice_id; /* cloned voice UUID */
    const char *emotion;  /* "content", "excited", etc. */
    float speed;          /* 0.6–1.5; default 0.95 */
    float volume;         /* 0.5–2.0; default 1.0 */
    bool nonverbals;
} hu_cartesia_tts_config_t;

/* output_format: "mp3", "wav", "ogg", or "caf". NULL or "" -> "mp3".
 * "caf" uses MP3 on the wire (converted to .caf in the audio pipeline).
 * "ogg" requests WAV from Cartesia (no native OGG); caller should write .wav until conversion
 * exists. */
/* Build the /tts/bytes JSON body (exposed so tests can parse what the daemon
 * would send). On success `jbuf` is initialized and owned by the caller. */
hu_error_t hu_cartesia_build_tts_body(hu_allocator_t *alloc, const char *transcript,
                                      size_t transcript_len, const hu_cartesia_tts_config_t *config,
                                      const char *output_format, struct hu_json_buf *jbuf);

hu_error_t hu_cartesia_tts_synthesize(hu_allocator_t *alloc, const char *api_key,
                                      size_t api_key_len, const char *transcript,
                                      size_t transcript_len, const hu_cartesia_tts_config_t *config,
                                      const char *output_format, unsigned char **out_bytes,
                                      size_t *out_len);

#if defined(HU_IS_TEST) && HU_IS_TEST
/* Test-only: what the mock synthesizer was last handed. */
const char *hu_cartesia_test_last_transcript(void);
const hu_cartesia_tts_config_t *hu_cartesia_test_last_config(void);
#endif

void hu_cartesia_tts_free_bytes(hu_allocator_t *alloc, unsigned char *bytes, size_t len);

/* --- Cartesia STT (Ink Whisper) --- */

typedef struct hu_cartesia_stt_config {
    const char *model;    /* "ink-whisper" (default) */
    const char *language; /* ISO-639-1, e.g. "en"; NULL = auto */
} hu_cartesia_stt_config_t;

/* Transcribe an audio file via Cartesia's /stt endpoint.
 * Auth: X-API-Key header. Response: {"text":"..."}.
 * Caller must free *out_text via alloc->free(ctx, ptr, *out_len + 1). */
hu_error_t hu_cartesia_stt_transcribe(hu_allocator_t *alloc, const char *api_key,
                                      size_t api_key_len, const char *audio_path,
                                      const hu_cartesia_stt_config_t *config, char **out_text,
                                      size_t *out_len);

#endif
