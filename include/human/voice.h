#ifndef HU_VOICE_H
#define HU_VOICE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct hu_voice_config {
    const char *api_key;
    size_t api_key_len;
    const char *stt_endpoint;       /* NULL = default (Groq) */
    const char *tts_endpoint;       /* NULL = default (OpenAI) */
    const char *stt_model;          /* NULL = "whisper-large-v3" */
    const char *tts_model;          /* NULL = "tts-1" */
    const char *tts_voice;          /* NULL = "alloy" */
    const char *language;           /* NULL = auto-detect */
    const char *local_stt_endpoint; /* NULL = skip; if set, tried before cloud STT */
    const char *local_tts_endpoint; /* NULL = skip; if set, tried before cloud TTS */
    const char *stt_provider;       /* NULL = auto; "cartesia", "groq", "gemini", "local" */
    const char *tts_provider;       /* NULL = auto; "cartesia", "openai", "local" */
    const char *cartesia_api_key;   /* NULL = use CARTESIA_API_KEY env; for Cartesia STT/TTS */
    size_t cartesia_api_key_len;
    const char
        *openai_api_key; /* OpenAI key when voice.mode is "realtime" (secondary to api_key) */
    size_t openai_api_key_len;
    bool privacy_mode; /* true = on-device only; never contact Cartesia/cloud (ADR 2026-05-31) */
} hu_voice_config_t;

/*
 * Voice provider tiering (ADR 2026-05-31: Cartesia default TTS, local privacy mode).
 * privacy_mode is the cloud-egress kill-switch: when true, STT and TTS use ONLY the
 * local on-device path and never contact Cartesia or the cloud default.
 */
typedef enum hu_voice_tts_backend {
    HU_VOICE_TTS_BACKEND_LOCAL = 0,    /* on-device (Kokoro / local OpenAI-compatible server) */
    HU_VOICE_TTS_BACKEND_CARTESIA,     /* Cartesia cloud (cloned + expressive) */
    HU_VOICE_TTS_BACKEND_CLOUD_DEFAULT /* OpenAI-compatible cloud default */
} hu_voice_tts_backend_t;

/* True when non-local (cloud) STT/TTS egress is permitted. Equivalent to
 * !privacy_mode — centralized so STT and TTS share one privacy predicate. */
bool hu_voice_cloud_egress_allowed(bool privacy_mode);

/*
 * Preferred TTS backend given privacy mode + availability. Pure; performs no I/O.
 *   privacy_mode                -> LOCAL (cloud forbidden)
 *   cartesia_selected_and_keyed -> CARTESIA (the default cloud tier)
 *   have_local_endpoint         -> LOCAL (fallback)
 *   else                        -> CLOUD_DEFAULT
 * Extracted as a pure predicate so the egress/precedence decision is unit-testable
 * without a network call — see .claude/rules/security-predicate-extraction.md.
 */
hu_voice_tts_backend_t hu_voice_preferred_tts_backend(bool privacy_mode,
                                                      bool cartesia_selected_and_keyed,
                                                      bool have_local_endpoint);

/* Static, human-readable privacy disclosure for the active backend (ADR mitigation M4). */
const char *hu_voice_privacy_disclosure(hu_voice_tts_backend_t backend);

struct hu_config;
/* Build hu_voice_config_t from config settings + provider keys.
 * Does not allocate — all pointers reference config-owned strings.
 * config may be NULL (returns zeroed struct). */
hu_error_t hu_voice_config_from_settings(const struct hu_config *config, hu_voice_config_t *out);

hu_error_t hu_voice_stt_file(hu_allocator_t *alloc, const hu_voice_config_t *config,
                             const char *file_path, char **out_text, size_t *out_len);

hu_error_t hu_voice_stt(hu_allocator_t *alloc, const hu_voice_config_t *config,
                        const void *audio_data, size_t audio_len, char **out_text, size_t *out_len);

hu_error_t hu_voice_tts(hu_allocator_t *alloc, const hu_voice_config_t *config, const char *text,
                        size_t text_len, void **out_audio, size_t *out_audio_len);

hu_error_t hu_voice_play(hu_allocator_t *alloc, const void *audio_data, size_t audio_len);

/*
 * Gemini STT — transcribe audio via Gemini generateContent with inline_data.
 * Takes base64-encoded payload directly (no decode needed).
 * When mime_type begins with "video/", uses a video-description prompt instead of transcription.
 * config->api_key is used as the Gemini API key.
 * config->stt_model defaults to "gemini-3.1-flash-lite" if NULL.
 * config->stt_endpoint defaults to Gemini API URL if NULL.
 */
hu_error_t hu_voice_stt_gemini(hu_allocator_t *alloc, const hu_voice_config_t *config,
                               const char *audio_base64, size_t audio_base64_len,
                               const char *mime_type, char **out_text, size_t *out_len);

#endif /* HU_VOICE_H */
