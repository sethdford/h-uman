#include "human/tts/cartesia.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/process_util.h"
#include <stdio.h>
#include <string.h>

const char *hu_tts_format_for_channel(const char *channel_name) {
    if (!channel_name)
        return "mp3";
    if (strcmp(channel_name, "imessage") == 0)
        return "caf";
    if (strcmp(channel_name, "telegram") == 0 || strcmp(channel_name, "discord") == 0)
        return "ogg";
    return "mp3";
}

#if defined(HU_ENABLE_CARTESIA)

#define CARTESIA_TTS_URL "https://api.cartesia.ai/tts/bytes"
#define CARTESIA_VERSION "2026-03-01"
#define DEFAULT_MODEL "sonic-3-2026-01-12"
#define DEFAULT_EMOTION "content"
#define DEFAULT_SPEED 0.95f
#define DEFAULT_VOLUME 1.0f
#define MOCK_MP3_HEADER 0xFF
#define MOCK_MP3_HEADER_LEN 4
#define MOCK_MP3_REPEAT 100

#if !HU_IS_TEST && defined(HU_HTTP_CURL)
/* Cartesia /tts/bytes supports mp3 and wav containers (not ogg). */
typedef enum {
    CARTESIA_API_MP3,
    CARTESIA_API_WAV,
} cartesia_api_container_t;

static cartesia_api_container_t api_container_for_output_format(const char *output_format) {
    const char *f = output_format;
    if (!f || !f[0])
        f = "mp3";
    if (strcmp(f, "caf") == 0 || strcmp(f, "mp3") == 0)
        return CARTESIA_API_MP3;
    if (strcmp(f, "wav") == 0 || strcmp(f, "ogg") == 0)
        return CARTESIA_API_WAV;
    return CARTESIA_API_MP3;
}

static hu_error_t append_output_format_json(hu_json_buf_t *jbuf, cartesia_api_container_t api) {
    if (api == CARTESIA_API_WAV)
        return hu_json_buf_append_raw(jbuf,
            "\"output_format\":{\"container\":\"wav\",\"encoding\":\"pcm_s16le\","
            "\"sample_rate\":44100},\"generation_config\":{", 100);
    return hu_json_buf_append_raw(jbuf,
        "\"output_format\":{\"container\":\"mp3\",\"sample_rate\":44100,\"bit_rate\":128000},"
        "\"generation_config\":{", 95);
}

static void apply_defaults(const hu_cartesia_tts_config_t *config,
    const char **model_id, const char **voice_id, const char **emotion,
    float *speed, float *volume, bool *nonverbals) {
    if (config) {
        *model_id = config->model_id && config->model_id[0] ? config->model_id : DEFAULT_MODEL;
        *voice_id = config->voice_id ? config->voice_id : "";
        *emotion = config->emotion && config->emotion[0] ? config->emotion : DEFAULT_EMOTION;
        *speed = config->speed > 0.f ? config->speed : DEFAULT_SPEED;
        *volume = config->volume > 0.f ? config->volume : DEFAULT_VOLUME;
        *nonverbals = config->nonverbals;
    } else {
        *model_id = DEFAULT_MODEL;
        *voice_id = "";
        *emotion = DEFAULT_EMOTION;
        *speed = DEFAULT_SPEED;
        *volume = DEFAULT_VOLUME;
        *nonverbals = false;
    }
}
#endif /* !HU_IS_TEST && HU_HTTP_CURL */

hu_error_t hu_cartesia_tts_synthesize(hu_allocator_t *alloc,
    const char *api_key, size_t api_key_len,
    const char *transcript, size_t transcript_len,
    const hu_cartesia_tts_config_t *config,
    const char *output_format,
    unsigned char **out_bytes, size_t *out_len) {
    if (!alloc || !out_bytes || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_bytes = NULL;
    *out_len = 0;

    if (!api_key || api_key_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (!transcript || transcript_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

#if HU_IS_TEST
    (void)config;
    (void)output_format;
    /* Mock audio bytes for any output_format (no network). */
    static const unsigned char mock_header[MOCK_MP3_HEADER_LEN] = {0xFF, 0xFB, 0x90, 0x00};
    size_t mock_len = MOCK_MP3_HEADER_LEN * MOCK_MP3_REPEAT;
    unsigned char *mock = (unsigned char *)alloc->alloc(alloc->ctx, mock_len);
    if (!mock)
        return HU_ERR_OUT_OF_MEMORY;
    for (size_t i = 0; i < MOCK_MP3_REPEAT; i++)
        memcpy(mock + i * MOCK_MP3_HEADER_LEN, mock_header, MOCK_MP3_HEADER_LEN);
    *out_bytes = mock;
    *out_len = mock_len;
    return HU_OK;
#elif defined(HU_HTTP_CURL)
    const char *model_id, *voice_id, *emotion;
    float speed, volume;
    bool nonverbals;
    apply_defaults(config, &model_id, &voice_id, &emotion, &speed, &volume, &nonverbals);
    cartesia_api_container_t api_ct = api_container_for_output_format(output_format);

    hu_json_buf_t jbuf;
    hu_error_t err = hu_json_buf_init(&jbuf, alloc);
    if (err)
        return err;

    err = hu_json_buf_append_raw(&jbuf, "{\"model_id\":\"", 12);
    if (err)
        goto fail;
    err = hu_json_buf_append_raw(&jbuf, model_id, strlen(model_id));
    if (err)
        goto fail;
    err = hu_json_buf_append_raw(&jbuf, "\",\"transcript\":\"", 16);
    if (err)
        goto fail;
    err = hu_json_append_string(&jbuf, transcript, transcript_len);
    if (err)
        goto fail;
    err = hu_json_buf_append_raw(&jbuf, "\",\"voice\":{\"mode\":\"id\",\"id\":\"", 30);
    if (err)
        goto fail;
    err = hu_json_append_string(&jbuf, voice_id, strlen(voice_id));
    if (err)
        goto fail;
    err = hu_json_buf_append_raw(&jbuf, "\"},", 3);
    if (err)
        goto fail;
    err = append_output_format_json(&jbuf, api_ct);
    if (err)
        goto fail;

    char num_buf[64];
    int n = snprintf(num_buf, sizeof(num_buf), "\"speed\":%.2f", (double)speed);
    if (n <= 0 || (size_t)n >= sizeof(num_buf)) {
        err = HU_ERR_INTERNAL;
        goto fail;
    }
    err = hu_json_buf_append_raw(&jbuf, num_buf, (size_t)n);
    if (err)
        goto fail;
    err = hu_json_buf_append_raw(&jbuf, ",\"emotion\":\"", 11);
    if (err)
        goto fail;
    err = hu_json_append_string(&jbuf, emotion, strlen(emotion));
    if (err)
        goto fail;
    n = snprintf(num_buf, sizeof(num_buf), ",\"volume\":%.2f,\"nonverbals\":%s}",
        (double)volume, nonverbals ? "true" : "false");
    if (n <= 0 || (size_t)n >= sizeof(num_buf)) {
        err = HU_ERR_INTERNAL;
        goto fail;
    }
    err = hu_json_buf_append_raw(&jbuf, num_buf, (size_t)n);
    if (err)
        goto fail;
    err = hu_json_buf_append_raw(&jbuf, "}}", 2);
    if (err)
        goto fail;

    char headers_buf[768];
    n = snprintf(headers_buf, sizeof(headers_buf),
        "X-API-Key: %.*s\nCartesia-Version: %s\nContent-Type: application/json",
        (int)api_key_len, api_key, CARTESIA_VERSION);
    if (n <= 0 || (size_t)n >= sizeof(headers_buf)) {
        err = HU_ERR_INVALID_ARGUMENT;
        goto fail;
    }

    hu_http_response_t resp = {0};
    err = hu_http_request(alloc, CARTESIA_TTS_URL, "POST", headers_buf,
        jbuf.ptr, jbuf.len, &resp);
    hu_json_buf_free(&jbuf);
    if (err != HU_OK)
        return err;

    long status = resp.status_code;
    if (status < 200 || status >= 300) {
        hu_http_response_free(alloc, &resp);
        if (status == 401)
            return HU_ERR_PROVIDER_AUTH;
        if (status == 429)
            return HU_ERR_PROVIDER_RATE_LIMITED;
        return HU_ERR_PROVIDER_RESPONSE;
    }

    if (!resp.body || resp.body_len == 0) {
        hu_http_response_free(alloc, &resp);
        return HU_ERR_PROVIDER_RESPONSE;
    }

    unsigned char *out = (unsigned char *)alloc->alloc(alloc->ctx, resp.body_len);
    if (!out) {
        hu_http_response_free(alloc, &resp);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(out, resp.body, resp.body_len);
    hu_http_response_free(alloc, &resp);
    *out_bytes = out;
    *out_len = resp.body_len;
    return HU_OK;

fail:
    hu_json_buf_free(&jbuf);
    return err;
#else
    /* HU_ENABLE_CARTESIA but no HU_HTTP_CURL (e.g. test lib without curl) */
    (void)config;
    (void)output_format;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

void hu_cartesia_tts_free_bytes(hu_allocator_t *alloc, unsigned char *bytes, size_t len) {
    if (alloc && bytes)
        alloc->free(alloc->ctx, bytes, len);
}

#else

hu_error_t hu_cartesia_tts_synthesize(hu_allocator_t *alloc,
    const char *api_key, size_t api_key_len,
    const char *transcript, size_t transcript_len,
    const hu_cartesia_tts_config_t *config,
    const char *output_format,
    unsigned char **out_bytes, size_t *out_len) {
#if HU_IS_TEST
    (void)config;
    (void)output_format;
    if (!alloc || !out_bytes || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (!api_key || api_key_len == 0 || !transcript || transcript_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    *out_bytes = NULL;
    *out_len = 0;
    static const unsigned char mock_header[4] = {0xFF, 0xFB, 0x90, 0x00};
    size_t mock_len = 4 * 100;
    unsigned char *mock = (unsigned char *)alloc->alloc(alloc->ctx, mock_len);
    if (!mock)
        return HU_ERR_OUT_OF_MEMORY;
    for (size_t i = 0; i < 100; i++)
        memcpy(mock + i * 4, mock_header, 4);
    *out_bytes = mock;
    *out_len = mock_len;
    return HU_OK;
#else
    (void)alloc;
    (void)api_key;
    (void)api_key_len;
    (void)transcript;
    (void)transcript_len;
    (void)config;
    (void)output_format;
    (void)out_bytes;
    (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

void hu_cartesia_tts_free_bytes(hu_allocator_t *alloc, unsigned char *bytes, size_t len) {
#if HU_IS_TEST
    /* The HU_IS_TEST branch of hu_cartesia_tts_synthesize above allocates
     * a 400-byte mock buffer through `alloc->alloc` regardless of whether
     * HU_ENABLE_CARTESIA is on or off. When CARTESIA is off the stub free
     * used to be a (void) no-op, leaking the entire mock buffer on every
     * test invocation (test_voice_message_integration leaked 400 b on PR55
     * ASan). Mirror the allocation: free through the same allocator. */
    if (alloc && bytes)
        alloc->free(alloc->ctx, bytes, len);
#else
    (void)alloc;
    (void)bytes;
    (void)len;
#endif
}

#endif

/* --- Cartesia STT (always compiled; real HTTP path requires HU_HTTP_CURL) --- */

#define CARTESIA_STT_URL "https://api.cartesia.ai/stt"
#define CARTESIA_STT_DEFAULT_MODEL "ink-whisper"
#ifndef CARTESIA_VERSION
#define CARTESIA_VERSION "2026-03-01"
#endif

hu_error_t hu_cartesia_stt_transcribe(hu_allocator_t *alloc, const char *api_key,
                                      size_t api_key_len, const char *audio_path,
                                      const hu_cartesia_stt_config_t *config, char **out_text,
                                      size_t *out_len) {
    if (!alloc || !out_text || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_text = NULL;
    *out_len = 0;
    if (!api_key || api_key_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (!audio_path || audio_path[0] == '\0')
        return HU_ERR_INVALID_ARGUMENT;

#if HU_IS_TEST
    (void)api_key;
    (void)api_key_len;
    (void)audio_path;
    (void)config;
    const char *mock = "Cartesia mock transcription";
    size_t mlen = strlen(mock);
    char *dup = (char *)alloc->alloc(alloc->ctx, mlen + 1);
    if (!dup)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(dup, mock, mlen);
    dup[mlen] = '\0';
    *out_text = dup;
    *out_len = mlen;
    return HU_OK;
#elif defined(HU_HTTP_CURL)
    const char *model =
        (config && config->model && config->model[0]) ? config->model : CARTESIA_STT_DEFAULT_MODEL;

    char hdr_buf[512];
    int n = snprintf(hdr_buf, sizeof(hdr_buf), "X-API-Key: %.*s\nCartesia-Version: %s",
                     (int)api_key_len, api_key, CARTESIA_VERSION);
    if (n < 0 || (size_t)n >= sizeof(hdr_buf))
        return HU_ERR_INVALID_ARGUMENT;

    size_t file_cap = 64 + strlen(audio_path);
    char *file_arg = (char *)alloc->alloc(alloc->ctx, file_cap);
    if (!file_arg)
        return HU_ERR_OUT_OF_MEMORY;
    n = snprintf(file_arg, file_cap, "file=@%s", audio_path);
    if (n < 0 || (size_t)n >= file_cap) {
        alloc->free(alloc->ctx, file_arg, file_cap);
        return HU_ERR_INVALID_ARGUMENT;
    }

    size_t model_cap = 32 + strlen(model);
    char *model_arg = (char *)alloc->alloc(alloc->ctx, model_cap);
    if (!model_arg) {
        alloc->free(alloc->ctx, file_arg, file_cap);
        return HU_ERR_OUT_OF_MEMORY;
    }
    n = snprintf(model_arg, model_cap, "model=%s", model);
    if (n < 0 || (size_t)n >= model_cap) {
        alloc->free(alloc->ctx, file_arg, file_cap);
        alloc->free(alloc->ctx, model_arg, model_cap);
        return HU_ERR_INVALID_ARGUMENT;
    }

    char *lang_arg = NULL;
    size_t lang_cap = 0;
    if (config && config->language && config->language[0]) {
        lang_cap = 32 + strlen(config->language);
        lang_arg = (char *)alloc->alloc(alloc->ctx, lang_cap);
        if (!lang_arg) {
            alloc->free(alloc->ctx, file_arg, file_cap);
            alloc->free(alloc->ctx, model_arg, model_cap);
            return HU_ERR_OUT_OF_MEMORY;
        }
        n = snprintf(lang_arg, lang_cap, "language=%s", config->language);
        if (n < 0 || (size_t)n >= lang_cap) {
            alloc->free(alloc->ctx, file_arg, file_cap);
            alloc->free(alloc->ctx, model_arg, model_cap);
            alloc->free(alloc->ctx, lang_arg, lang_cap);
            return HU_ERR_INVALID_ARGUMENT;
        }
    }

    const char *argv[18];
    size_t argc = 0;
    argv[argc++] = "curl";
    argv[argc++] = "-s";
    argv[argc++] = "-X";
    argv[argc++] = "POST";
    argv[argc++] = "-H";
    argv[argc++] = hdr_buf;
    argv[argc++] = "-F";
    argv[argc++] = file_arg;
    argv[argc++] = "-F";
    argv[argc++] = model_arg;
    if (lang_arg) {
        argv[argc++] = "-F";
        argv[argc++] = lang_arg;
    }
    argv[argc++] = CARTESIA_STT_URL;
    argv[argc] = NULL;

    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run(alloc, argv, NULL, 4 * 1024 * 1024, &result);
    alloc->free(alloc->ctx, file_arg, file_cap);
    alloc->free(alloc->ctx, model_arg, model_cap);
    if (lang_arg)
        alloc->free(alloc->ctx, lang_arg, lang_cap);

    if (err != HU_OK) {
        hu_run_result_free(alloc, &result);
        return err;
    }
    if (!result.success || !result.stdout_buf || result.stdout_len == 0) {
        hu_run_result_free(alloc, &result);
        return HU_ERR_PROVIDER_RESPONSE;
    }

    hu_json_value_t *parsed = NULL;
    err = hu_json_parse(alloc, result.stdout_buf, result.stdout_len, &parsed);
    hu_run_result_free(alloc, &result);
    if (err != HU_OK)
        return HU_ERR_PARSE;

    hu_json_value_t *text_val = hu_json_object_get(parsed, "text");
    if (!text_val || text_val->type != HU_JSON_STRING) {
        hu_json_free(alloc, parsed);
        return HU_ERR_PARSE;
    }
    size_t text_len = text_val->data.string.len;
    char *text = (char *)alloc->alloc(alloc->ctx, text_len + 1);
    if (!text) {
        hu_json_free(alloc, parsed);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(text, text_val->data.string.ptr, text_len);
    text[text_len] = '\0';
    hu_json_free(alloc, parsed);

    *out_text = text;
    *out_len = text_len;
    return HU_OK;
#else
    (void)api_key;
    (void)api_key_len;
    (void)audio_path;
    (void)config;
    return HU_ERR_NOT_SUPPORTED;
#endif
}
