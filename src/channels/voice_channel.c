#include "seaclaw/channels/voice_channel.h"
#include <stdlib.h>
#include <string.h>

#ifdef SC_HAS_SONATA
/* FFI declarations for Sonata Rust pipeline */
extern int32_t sonata_pipeline_init(const char *config_json, size_t config_len);
extern int32_t sonata_stt(const float *audio, size_t samples,
                          char *text, size_t *text_len);
extern int32_t sonata_tts(const char *text, size_t text_len,
                          const char *speaker_id, float emotion_exag,
                          float *audio, size_t *audio_len);
extern int32_t sonata_speaker_encode(const float *audio, size_t samples,
                                     float *embedding);
extern void sonata_pipeline_deinit(void);
#endif

typedef struct sc_voice_ctx {
    sc_allocator_t *alloc;
    sc_channel_voice_config_t config;
    bool initialized;
    bool running;
} sc_voice_ctx_t;

static sc_error_t voice_start(void *ctx)
{
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

static void voice_stop(void *ctx)
{
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

static sc_error_t voice_send(void *ctx,
    const char *target, size_t target_len,
    const char *message, size_t message_len,
    const char *const *media, size_t media_count)
{
    sc_voice_ctx_t *v = (sc_voice_ctx_t *)ctx;
    if (!v || !message)
        return SC_ERR_INVALID_ARGUMENT;

    (void)target;
    (void)target_len;
    (void)media;
    (void)media_count;

#ifdef SC_HAS_SONATA
    /* Convert text to speech via Sonata TTS */
    float audio_buf[24000 * 30]; /* up to 30s at 24kHz */
    size_t audio_len = sizeof(audio_buf) / sizeof(float);

    int32_t err = sonata_tts(message, message_len,
        v->config.speaker_id, v->config.emotion_exaggeration,
        audio_buf, &audio_len);
    if (err != 0)
        return SC_ERR_CHANNEL_SEND;

    /* In production: play audio via platform audio API */
#endif

    return SC_OK;
}

static const char *voice_name(void *ctx)
{
    (void)ctx;
    return "voice";
}

static bool voice_health_check(void *ctx)
{
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

sc_error_t sc_channel_voice_create(sc_allocator_t *alloc,
    const sc_channel_voice_config_t *config,
    sc_channel_t *out)
{
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;

    sc_voice_ctx_t *v = (sc_voice_ctx_t *)calloc(1, sizeof(*v));
    if (!v)
        return SC_ERR_OUT_OF_MEMORY;

    v->alloc = alloc;
    v->initialized = false;
    v->running = false;

    if (config) {
        v->config = *config;
    }

    /* Apply defaults */
    if (v->config.sample_rate == 0)
        v->config.sample_rate = 24000;
    if (v->config.emotion_exaggeration == 0.0f)
        v->config.emotion_exaggeration = 1.0f;

    out->ctx = v;
    out->vtable = &voice_vtable;
    return SC_OK;
}

void sc_channel_voice_destroy(sc_channel_t *ch)
{
    if (ch && ch->ctx) {
        sc_voice_ctx_t *v = (sc_voice_ctx_t *)ch->ctx;
        if (v->running)
            voice_stop(v);
        free(v);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
