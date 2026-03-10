#ifndef HU_CHANNELS_VOICE_CHANNEL_H
#define HU_CHANNELS_VOICE_CHANNEL_H

#include "human/channel.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * Sonata Voice Channel — native on-device STT/TTS via Sonata pipeline.
 *
 * When HU_HAS_SONATA is defined, this channel uses the Rust Sonata pipeline
 * for native speech-to-text and text-to-speech. Otherwise, it falls back
 * to the cloud-based hu_voice_stt/tts API.
 */

/** Callback invoked when TTS audio is ready for playback. */
typedef void (*hu_voice_audio_callback_t)(const float *audio, size_t samples, void *user_data);

typedef struct hu_channel_voice_config {
    const char *codec_model_path;             /* Path to codec weights */
    const char *stt_model_path;               /* Path to STT weights */
    const char *tts_model_path;               /* Path to TTS weights */
    const char *cam_model_path;               /* Path to CAM++ weights */
    const char *cfm_model_path;               /* Path to CFM weights */
    const char *speaker_id;                   /* Speaker identity for TTS */
    float emotion_exaggeration;               /* 0.0 - 2.0, default 1.0 */
    bool emotion_exaggeration_set;            /* true if explicitly configured */
    uint32_t sample_rate;                     /* Default: 24000 */
    bool enable_full_duplex;                  /* Enable overlapping speech */
    bool enable_backchanneling;               /* Enable hmm/right/oh responses */
    hu_voice_audio_callback_t on_audio_ready; /* Callback for generated audio */
    void *callback_user_data;                 /* User data for callback */
} hu_channel_voice_config_t;

/**
 * Create a voice channel.
 *
 * @param alloc  Allocator for memory management
 * @param config Voice channel configuration (NULL for defaults)
 * @param out    Output channel handle
 * @return HU_OK on success
 */
hu_error_t hu_channel_voice_create(hu_allocator_t *alloc, const hu_channel_voice_config_t *config,
                                   hu_channel_t *out);

/**
 * Destroy a voice channel and release resources.
 */
void hu_channel_voice_destroy(hu_channel_t *ch);

#endif /* HU_CHANNELS_VOICE_CHANNEL_H */
