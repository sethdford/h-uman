/* include/human/tts/voice_reply.h
 *
 * One builder for everything a voice reply sends to Cartesia: the
 * SSML-annotated transcript (hu_transcript_prep) plus the request-level
 * generation config. The daemon (src/daemon/daemon_voice_reply.c) and the
 * `human voice preview` CLI both go through hu_voice_reply_build_request, so
 * what you preview is what the daemon sends.
 *
 * Why: until 2026-09-20 hu_transcript_prep had no production caller — the
 * daemon used a 15% coin-flip nonverbal injector and one emotion for the whole
 * clip, which is why voice replies sounded rushed and flat. */
#ifndef HU_TTS_VOICE_REPLY_H
#define HU_TTS_VOICE_REPLY_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tts/cartesia.h"
#include "human/tts/transcript_prep.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hu_persona_voice_config;

#define HU_VOICE_REPLY_DEFAULT_MODEL "sonic-3.6"
#define HU_VOICE_REPLY_DEFAULT_SPEED 0.95f

/* Self-contained request. `tts.model_id`, `tts.emotion` point INTO this struct
 * and `tts.voice_id` into the persona, so do not copy it by value. */
typedef struct hu_voice_reply_request {
    char transcript[HU_PREP_MAX_OUTPUT];
    size_t transcript_len;
    size_t sentence_count;
    char emotion[32];
    char model[64];
    hu_cartesia_tts_config_t tts;
} hu_voice_reply_request_t;

/* Build the request for one reply. `incoming` (may be NULL) is the message
 * being replied to; it steers per-sentence emotion. `hour_local` drives the
 * late-night slowdown; `seed` picks nonverbals / thinking sounds. */
hu_error_t hu_voice_reply_build_request(const struct hu_persona_voice_config *voice,
                                        const char *response, size_t response_len,
                                        const char *incoming, size_t incoming_len, int hour_local,
                                        uint32_t seed, hu_voice_reply_request_t *out);

/* Write synthesized audio to a temp file in the container the channel wants
 * (CAF via afconvert for iMessage, otherwise mp3/wav). Caller must
 * hu_audio_cleanup_temp(out_path). `channel_name` may be NULL. */
hu_error_t hu_voice_reply_audio_to_temp(hu_allocator_t *alloc, const char *channel_name,
                                        const unsigned char *bytes, size_t len, char *out_path,
                                        size_t out_cap);

#endif /* HU_TTS_VOICE_REPLY_H */
