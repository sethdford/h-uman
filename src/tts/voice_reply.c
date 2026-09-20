/* src/tts/voice_reply.c — see include/human/tts/voice_reply.h */
#include "human/tts/voice_reply.h"

#include "human/daemon/voice_facade.h"
#include "human/persona.h"
#include "human/tts/audio_pipeline.h"

#include <stdio.h>
#include <string.h>

static float clamp_volume(float v) {
    if (v < 0.5f)
        return 0.5f;
    if (v > 2.0f)
        return 2.0f;
    return v;
}

hu_error_t hu_voice_reply_build_request(const hu_persona_voice_config_t *voice,
                                        const char *response, size_t response_len,
                                        const char *incoming, size_t incoming_len, int hour_local,
                                        uint32_t seed, hu_voice_reply_request_t *out) {
    if (!voice || !response || response_len == 0 || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    int hour = hour_local < 0 ? 0 : hour_local > 23 ? 23 : hour_local;
    hu_prep_config_t cfg = {
        .incoming_msg = incoming,
        .incoming_msg_len = incoming ? incoming_len : 0,
        .default_emotion = voice->default_emotion[0] ? voice->default_emotion : "content",
        .base_speed =
            voice->default_speed > 0.f ? voice->default_speed : HU_VOICE_REPLY_DEFAULT_SPEED,
        .pause_factor = 1.0f,
        .discourse_rate = 0.3f,
        .nonverbals_enabled = voice->nonverbals,
        .strip_ssml = false,
        .thinking_sounds = true,
        .seed = seed,
        .hour_local = (uint8_t)hour,
    };

    hu_prep_result_t prep;
    hu_error_t err = hu_transcript_prep(response, response_len, &cfg, &prep);
    if (err != HU_OK)
        return err;

    size_t n = prep.output_len < sizeof(out->transcript) - 1 ? prep.output_len
                                                             : sizeof(out->transcript) - 1;
    memcpy(out->transcript, prep.output, n);
    out->transcript[n] = '\0';
    out->transcript_len = n;
    out->sentence_count = prep.sentence_count;

    snprintf(out->emotion, sizeof(out->emotion), "%s",
             prep.dominant_emotion ? prep.dominant_emotion : cfg.default_emotion);
    snprintf(out->model, sizeof(out->model), "%s",
             voice->model[0] ? voice->model : HU_VOICE_REPLY_DEFAULT_MODEL);

    out->tts.model_id = out->model;
    out->tts.voice_id = voice->voice_id;
    out->tts.emotion = out->emotion;
    /* Per-sentence <speed> tags are multipliers on this request-level speed. */
    out->tts.speed = prep.base_speed > 0.f ? prep.base_speed : cfg.base_speed;
    out->tts.volume = clamp_volume(prep.volume > 0.f ? prep.volume : 1.0f);
    out->tts.nonverbals = voice->nonverbals;
    return HU_OK;
}

hu_error_t hu_voice_reply_audio_to_temp(hu_allocator_t *alloc, const char *channel_name,
                                        const unsigned char *bytes, size_t len, char *out_path,
                                        size_t out_cap) {
    if (!alloc || !bytes || len == 0 || !out_path || out_cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    const char *fmt = hu_tts_format_for_channel(channel_name ? channel_name : "");
    if (strcmp(fmt, "caf") == 0)
        return hu_audio_mp3_to_caf(alloc, bytes, len, out_path, out_cap);
    const char *ext = "mp3";
    /* Cartesia has no OGG; WAV on disk until an Opus encoder lands. */
    if (strcmp(fmt, "wav") == 0 || strcmp(fmt, "ogg") == 0)
        ext = "wav";
    return hu_audio_tts_bytes_to_temp(alloc, bytes, len, ext, out_path, out_cap);
}
