#ifndef SC_CHANNELS_VOICE_CHANNEL_H
#define SC_CHANNELS_VOICE_CHANNEL_H

#include "seaclaw/channel.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * Sonata Voice Channel — native on-device STT/TTS via Sonata pipeline.
 *
 * When SC_HAS_SONATA is defined, this channel uses the Rust Sonata pipeline
 * for native speech-to-text and text-to-speech. Otherwise, it falls back
 * to the cloud-based sc_voice_stt/tts API.
 */

typedef struct sc_channel_voice_config {
    const char *codec_model_path;       /* Path to codec weights */
    const char *stt_model_path;         /* Path to STT weights */
    const char *tts_model_path;         /* Path to TTS weights */
    const char *cam_model_path;         /* Path to CAM++ weights */
    const char *cfm_model_path;         /* Path to CFM weights */
    const char *speaker_id;             /* Speaker identity for TTS */
    float emotion_exaggeration;         /* 0.0 - 2.0, default 1.0 */
    uint32_t sample_rate;               /* Default: 24000 */
    bool enable_full_duplex;            /* Enable overlapping speech */
    bool enable_backchanneling;         /* Enable hmm/right/oh responses */
} sc_channel_voice_config_t;

/**
 * Create a voice channel.
 *
 * @param alloc  Allocator for memory management
 * @param config Voice channel configuration (NULL for defaults)
 * @param out    Output channel handle
 * @return SC_OK on success
 */
sc_error_t sc_channel_voice_create(sc_allocator_t *alloc,
    const sc_channel_voice_config_t *config,
    sc_channel_t *out);

/**
 * Destroy a voice channel and release resources.
 */
void sc_channel_voice_destroy(sc_channel_t *ch);

#endif /* SC_CHANNELS_VOICE_CHANNEL_H */
