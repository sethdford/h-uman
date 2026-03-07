#include "seaclaw/channels/voice_channel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SC_HAS_SONATA
/* FFI declarations for Sonata Rust pipeline.
 * Rust FFI uses *const u8 / *mut u8 for byte buffers, which maps to uint8_t
 * in C (not char, which may be signed on some platforms). */
extern int32_t sonata_pipeline_init(const uint8_t *config_json, size_t config_len);
extern int32_t sonata_stt(const float *audio, size_t samples, uint8_t *text, size_t *text_len);
extern int32_t sonata_tts(const uint8_t *text, size_t text_len, const uint8_t *speaker_id,
                          float emotion_exag, float *audio, size_t *audio_len);
extern int32_t sonata_speaker_encode(const float *audio, size_t samples, float *embedding);
extern void sonata_pipeline_deinit(void);
#endif

#define VOICE_MAX_SAMPLES (24000u * 10u) /* 10s at 24kHz */

typedef struct sc_voice_ctx {
    sc_allocator_t *alloc;
    sc_channel_voice_config_t config;
    bool initialized;
    bool running;
} sc_voice_ctx_t;

static sc_error_t voice_start(void *ctx) {
    sc_voice_ctx_t *v = (sc_voice_ctx_t *)ctx;
    if (!v)
        return SC_ERR_INVALID_ARGUMENT;

#ifdef SC_HAS_SONATA
    if (!v->initialized) {
        int32_t err = sonata_pipeline_init(NULL, 0);
        if (err != 0)
            return SC_ERR_CHANNEL_START;
        v->initialized = true;
    }
#endif

    v->running = true;
    return SC_OK;
}

static void voice_stop(void *ctx) {
    sc_voice_ctx_t *v = (sc_voice_ctx_t *)ctx;
    if (!v)
        return;
    v->running = false;

#ifdef SC_HAS_SONATA
    if (v->initialized) {
        sonata_pipeline_deinit();
        v->initialized = false;
    }
#endif
}

static sc_error_t voice_send(void *ctx, const char *target, size_t target_len, const char *message,
                             size_t message_len, const char *const *media, size_t media_count) {
    sc_voice_ctx_t *v = (sc_voice_ctx_t *)ctx;
    if (!v || !message)
        return SC_ERR_INVALID_ARGUMENT;

    (void)target;
    (void)target_len;
    (void)media;
    (void)media_count;
    (void)message_len;

#ifdef SC_HAS_SONATA
    const size_t buf_samples = VOICE_MAX_SAMPLES;
    const size_t buf_bytes = buf_samples * sizeof(float);
    float *audio_buf = (float *)v->alloc->alloc(v->alloc->ctx, buf_bytes);
    if (!audio_buf) {
#if !SC_IS_TEST
        fprintf(stderr, "voice_channel: failed to allocate audio buffer (%zu bytes)\n", buf_bytes);
#endif
        return SC_ERR_OUT_OF_MEMORY;
    }

    size_t audio_len = buf_samples;
    float exag = v->config.emotion_exaggeration_set ? v->config.emotion_exaggeration : 1.0f;

    int32_t err = sonata_tts((const uint8_t *)message, message_len,
                             (const uint8_t *)v->config.speaker_id, exag, audio_buf, &audio_len);
    if (err != 0) {
#if !SC_IS_TEST
        fprintf(stderr, "voice_channel: sonata_tts failed (err=%d)\n", err);
#endif
        v->alloc->free(v->alloc->ctx, audio_buf, buf_bytes);
        return SC_ERR_CHANNEL_SEND;
    }

    if (v->config.on_audio_ready) {
        v->config.on_audio_ready(audio_buf, audio_len, v->config.callback_user_data);
    }

    v->alloc->free(v->alloc->ctx, audio_buf, buf_bytes);
#endif

    return SC_OK;
}

static const char *voice_name(void *ctx) {
    (void)ctx;
    return "voice";
}

static bool voice_health_check(void *ctx) {
    sc_voice_ctx_t *v = (sc_voice_ctx_t *)ctx;
    return v && v->running;
}

static const sc_channel_vtable_t voice_vtable = {
    .start = voice_start,
    .stop = voice_stop,
    .send = voice_send,
    .name = voice_name,
    .health_check = voice_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

sc_error_t sc_channel_voice_create(sc_allocator_t *alloc, const sc_channel_voice_config_t *config,
                                   sc_channel_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;

    sc_voice_ctx_t *v = (sc_voice_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*v));
    if (!v)
        return SC_ERR_OUT_OF_MEMORY;
    memset(v, 0, sizeof(*v));

    v->alloc = alloc;

    if (config) {
        v->config = *config;
    }

    /* Apply defaults */
    if (v->config.sample_rate == 0)
        v->config.sample_rate = 24000;

    out->ctx = v;
    out->vtable = &voice_vtable;
    return SC_OK;
}

void sc_channel_voice_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_voice_ctx_t *v = (sc_voice_ctx_t *)ch->ctx;
        sc_allocator_t *a = v->alloc;
        if (v->running)
            voice_stop(v);
        a->free(a->ctx, v, sizeof(*v));
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
