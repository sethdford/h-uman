#include "human/tts/cartesia.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include <stdio.h>
#include <string.h>

#if defined(HU_ENABLE_CARTESIA)

#define CARTESIA_TTS_URL "https://api.cartesia.ai/tts/bytes"
#define CARTESIA_VERSION "2024-06-10"
#define DEFAULT_MODEL "sonic-3-2026-01-12"
#define DEFAULT_EMOTION "content"
#define DEFAULT_SPEED 0.95f
#define DEFAULT_VOLUME 1.0f
#define MOCK_MP3_HEADER 0xFF
#define MOCK_MP3_HEADER_LEN 4
#define MOCK_MP3_REPEAT 100

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

hu_error_t hu_cartesia_tts_synthesize(hu_allocator_t *alloc,
    const char *api_key, size_t api_key_len,
    const char *transcript, size_t transcript_len,
    const hu_cartesia_tts_config_t *config,
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
    /* Mock: return fake MP3 header bytes (0xFF 0xFB 0x90 0x00 repeated 100 times) */
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
    err = hu_json_buf_append_raw(&jbuf, "\"},\"output_format\":{\"container\":\"mp3\","
        "\"encoding\":\"mp3\",\"sample_rate\":44100},\"generation_config\":{", 68);
    if (err)
        goto fail;

    char num_buf[32];
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
    unsigned char **out_bytes, size_t *out_len) {
    (void)alloc;
    (void)api_key;
    (void)api_key_len;
    (void)transcript;
    (void)transcript_len;
    (void)config;
    (void)out_bytes;
    (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
}

void hu_cartesia_tts_free_bytes(hu_allocator_t *alloc, unsigned char *bytes, size_t len) {
    (void)alloc;
    (void)bytes;
    (void)len;
}

#endif
