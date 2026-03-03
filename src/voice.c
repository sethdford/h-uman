/*
 * Voice STT (Whisper) and TTS (OpenAI) via HTTP APIs.
 * In SC_IS_TEST mode, returns mock data without spawning curl.
 */
#include "seaclaw/voice.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/process_util.h"
#include "seaclaw/core/string.h"
#include "seaclaw/platform.h"
#include <stdio.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

#define SC_VOICE_STT_DEFAULT_ENDPOINT "https://api.groq.com/openai/v1/audio/transcriptions"
#define SC_VOICE_STT_DEFAULT_MODEL    "whisper-large-v3"
#define SC_VOICE_TTS_DEFAULT_ENDPOINT "https://api.openai.com/v1/audio/speech"
#define SC_VOICE_TTS_DEFAULT_MODEL    "tts-1"
#define SC_VOICE_TTS_DEFAULT_VOICE    "alloy"

#if !SC_IS_TEST
static int get_pid(void) {
#if defined(__linux__)
    return (int)getpid();
#elif defined(__APPLE__) || defined(__FreeBSD__)
    return (int)getpid();
#else
    return 0;
#endif
}
#endif /* !SC_IS_TEST */

sc_error_t sc_voice_stt_file(sc_allocator_t *alloc, const sc_voice_config_t *config,
                             const char *file_path, char **out_text, size_t *out_len) {
    if (!alloc || !config || !out_text || !out_len)
        return SC_ERR_INVALID_ARGUMENT;
    *out_text = NULL;
    *out_len = 0;

    if (!config->api_key || config->api_key_len == 0)
        return SC_ERR_PROVIDER_AUTH;

#if SC_IS_TEST
    {
        (void)file_path;
        const char *prefix = "This is a mock transcription of ";
        size_t plen = strlen(prefix);
        size_t fplen = file_path ? strlen(file_path) : 0;
        char *mock = (char *)alloc->alloc(alloc->ctx, plen + fplen + 1);
        if (!mock)
            return SC_ERR_OUT_OF_MEMORY;
        memcpy(mock, prefix, plen);
        if (fplen > 0)
            memcpy(mock + plen, file_path, fplen);
        mock[plen + fplen] = '\0';
        *out_text = mock;
        *out_len = plen + fplen;
        return SC_OK;
    }
#else
    const char *endpoint = config->stt_endpoint && config->stt_endpoint[0]
                               ? config->stt_endpoint
                               : SC_VOICE_STT_DEFAULT_ENDPOINT;
    const char *model =
        config->stt_model && config->stt_model[0] ? config->stt_model : SC_VOICE_STT_DEFAULT_MODEL;

    /* Build Authorization header: "Authorization: Bearer " + api_key */
    size_t auth_prefix = 22; /* "Authorization: Bearer " */
    char *auth_hdr = (char *)alloc->alloc(alloc->ctx, auth_prefix + config->api_key_len + 1);
    if (!auth_hdr)
        return SC_ERR_OUT_OF_MEMORY;
    memcpy(auth_hdr, "Authorization: Bearer ", auth_prefix);
    memcpy(auth_hdr + auth_prefix, config->api_key, config->api_key_len);
    auth_hdr[auth_prefix + config->api_key_len] = '\0';

    /* Build file=@path and model=X */
    size_t file_arg_cap = 128 + (file_path ? strlen(file_path) : 0);
    char *file_arg = (char *)alloc->alloc(alloc->ctx, file_arg_cap);
    if (!file_arg) {
        alloc->free(alloc->ctx, auth_hdr, auth_prefix + config->api_key_len + 1);
        return SC_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(file_arg, file_arg_cap, "file=@%s", file_path ? file_path : "");
    if (n < 0 || (size_t)n >= file_arg_cap) {
        alloc->free(alloc->ctx, auth_hdr, auth_prefix + config->api_key_len + 1);
        alloc->free(alloc->ctx, file_arg, file_arg_cap);
        return SC_ERR_INVALID_ARGUMENT;
    }

    size_t model_arg_cap = 32 + strlen(model);
    char *model_arg = (char *)alloc->alloc(alloc->ctx, model_arg_cap);
    if (!model_arg) {
        alloc->free(alloc->ctx, auth_hdr, auth_prefix + config->api_key_len + 1);
        alloc->free(alloc->ctx, file_arg, file_arg_cap);
        return SC_ERR_OUT_OF_MEMORY;
    }
    n = snprintf(model_arg, model_arg_cap, "model=%s", model);
    if (n < 0 || (size_t)n >= model_arg_cap) {
        alloc->free(alloc->ctx, auth_hdr, auth_prefix + config->api_key_len + 1);
        alloc->free(alloc->ctx, file_arg, file_arg_cap);
        alloc->free(alloc->ctx, model_arg, model_arg_cap);
        return SC_ERR_INVALID_ARGUMENT;
    }

    char *lang_arg = NULL;
    size_t lang_arg_cap = 0;
    if (config->language && config->language[0]) {
        lang_arg_cap = 32 + strlen(config->language);
        lang_arg = (char *)alloc->alloc(alloc->ctx, lang_arg_cap);
        if (!lang_arg) {
            alloc->free(alloc->ctx, auth_hdr, auth_prefix + config->api_key_len + 1);
            alloc->free(alloc->ctx, file_arg, file_arg_cap);
            alloc->free(alloc->ctx, model_arg, model_arg_cap);
            return SC_ERR_OUT_OF_MEMORY;
        }
        n = snprintf(lang_arg, lang_arg_cap, "language=%s", config->language);
        if (n < 0 || (size_t)n >= lang_arg_cap) {
            alloc->free(alloc->ctx, lang_arg, lang_arg_cap);
            lang_arg = NULL;
        }
    }

    /* argv: curl -s -X POST -H "..." -F "file=@path" -F "model=..." [-F "language=..."] url */
    const char *argv[16];
    size_t argc = 0;
    argv[argc++] = "curl";
    argv[argc++] = "-s";
    argv[argc++] = "-X";
    argv[argc++] = "POST";
    argv[argc++] = "-H";
    argv[argc++] = auth_hdr;
    argv[argc++] = "-F";
    argv[argc++] = file_arg;
    argv[argc++] = "-F";
    argv[argc++] = model_arg;
    if (lang_arg) {
        argv[argc++] = "-F";
        argv[argc++] = lang_arg;
    }
    argv[argc++] = endpoint;
    argv[argc] = NULL;

    if (argc >= 16) {
        alloc->free(alloc->ctx, auth_hdr, auth_prefix + config->api_key_len + 1);
        alloc->free(alloc->ctx, file_arg, file_arg_cap);
        alloc->free(alloc->ctx, model_arg, model_arg_cap);
        if (lang_arg)
            alloc->free(alloc->ctx, lang_arg, lang_arg_cap);
        return SC_ERR_INVALID_ARGUMENT;
    }

    sc_run_result_t result = {0};
    sc_error_t err = sc_process_run(alloc, argv, NULL, 4 * 1024 * 1024, &result);
    alloc->free(alloc->ctx, auth_hdr, auth_prefix + config->api_key_len + 1);
    alloc->free(alloc->ctx, file_arg, file_arg_cap);
    alloc->free(alloc->ctx, model_arg, model_arg_cap);
    if (lang_arg)
        alloc->free(alloc->ctx, lang_arg, lang_arg_cap);

    if (err != SC_OK) {
        sc_run_result_free(alloc, &result);
        return err;
    }
    if (!result.success || !result.stdout_buf) {
        sc_run_result_free(alloc, &result);
        return SC_ERR_PROVIDER_RESPONSE;
    }

    /* Parse {"text":"..."} */
    sc_json_value_t *parsed = NULL;
    err = sc_json_parse(alloc, result.stdout_buf, result.stdout_len, &parsed);
    sc_run_result_free(alloc, &result);
    if (err != SC_OK)
        return SC_ERR_PARSE;

    sc_json_value_t *text_val = sc_json_object_get(parsed, "text");
    if (!text_val || text_val->type != SC_JSON_STRING) {
        sc_json_free(alloc, parsed);
        return SC_ERR_PARSE;
    }

    char *text = sc_strndup(alloc, text_val->data.string.ptr, text_val->data.string.len);
    sc_json_free(alloc, parsed);
    if (!text)
        return SC_ERR_OUT_OF_MEMORY;

    *out_text = text;
    *out_len = text_val->data.string.len;
    return SC_OK;
#endif
}

sc_error_t sc_voice_stt(sc_allocator_t *alloc, const sc_voice_config_t *config,
                        const void *audio_data, size_t audio_len, char **out_text,
                        size_t *out_len) {
    if (!alloc || !config || !out_text || !out_len)
        return SC_ERR_INVALID_ARGUMENT;
    if (!audio_data && audio_len > 0)
        return SC_ERR_INVALID_ARGUMENT;
    *out_text = NULL;
    *out_len = 0;

#if SC_IS_TEST
    {
        (void)audio_data;
        (void)audio_len;
        const char *mock = "Mock transcription";
        char *dup = sc_strndup(alloc, mock, strlen(mock));
        if (!dup)
            return SC_ERR_OUT_OF_MEMORY;
        *out_text = dup;
        *out_len = strlen(mock);
        return SC_OK;
    }
#else
    if (!audio_data && audio_len > 0)
        return SC_ERR_INVALID_ARGUMENT;

    char *tmp_dir = sc_platform_get_temp_dir(alloc);
    if (!tmp_dir)
        return SC_ERR_IO;

    char tmp_path[256];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s/seaclaw_voice_%d.bin", tmp_dir, get_pid());
    alloc->free(alloc->ctx, tmp_dir, strlen(tmp_dir) + 1);
    if (n < 0 || (size_t)n >= sizeof(tmp_path))
        return SC_ERR_IO;

    FILE *f = fopen(tmp_path, "wb");
    if (!f)
        return SC_ERR_IO;
    if (audio_len > 0 && fwrite(audio_data, 1, audio_len, f) != audio_len) {
        fclose(f);
        unlink(tmp_path);
        return SC_ERR_IO;
    }
    fclose(f);

    sc_error_t err = sc_voice_stt_file(alloc, config, tmp_path, out_text, out_len);
    unlink(tmp_path);
    return err;
#endif
}

sc_error_t sc_voice_tts(sc_allocator_t *alloc, const sc_voice_config_t *config, const char *text,
                        size_t text_len, void **out_audio, size_t *out_audio_len) {
    if (!alloc || !config || !out_audio || !out_audio_len)
        return SC_ERR_INVALID_ARGUMENT;
    *out_audio = NULL;
    *out_audio_len = 0;

    if (!config->api_key || config->api_key_len == 0)
        return SC_ERR_PROVIDER_AUTH;

#if SC_IS_TEST
    {
        (void)text;
        (void)text_len;
        static const unsigned char mock_audio[4] = {0x00, 0x01, 0x02, 0x03};
        void *buf = alloc->alloc(alloc->ctx, 4);
        if (!buf)
            return SC_ERR_OUT_OF_MEMORY;
        memcpy(buf, mock_audio, 4);
        *out_audio = buf;
        *out_audio_len = 4;
        return SC_OK;
    }
#else
    const char *endpoint = config->tts_endpoint && config->tts_endpoint[0]
                               ? config->tts_endpoint
                               : SC_VOICE_TTS_DEFAULT_ENDPOINT;
    const char *model =
        config->tts_model && config->tts_model[0] ? config->tts_model : SC_VOICE_TTS_DEFAULT_MODEL;
    const char *voice =
        config->tts_voice && config->tts_voice[0] ? config->tts_voice : SC_VOICE_TTS_DEFAULT_VOICE;

    /* Build JSON body: {"model":"tts-1","input":"<escaped>","voice":"alloy"} */
    sc_json_buf_t body_buf = {0};
    if (sc_json_buf_init(&body_buf, alloc) != SC_OK)
        return SC_ERR_OUT_OF_MEMORY;
    if (sc_json_buf_append_raw(&body_buf, "{\"model\":\"", 9) != SC_OK)
        goto tts_fail;
    if (sc_json_append_string(&body_buf, model, strlen(model)) != SC_OK)
        goto tts_fail;
    if (sc_json_buf_append_raw(&body_buf, "\",\"input\":", 10) != SC_OK)
        goto tts_fail;
    if (sc_json_append_string(&body_buf, text ? text : "", text_len) != SC_OK)
        goto tts_fail;
    if (sc_json_buf_append_raw(&body_buf, "\",\"voice\":\"", 10) != SC_OK)
        goto tts_fail;
    if (sc_json_append_string(&body_buf, voice, strlen(voice)) != SC_OK)
        goto tts_fail;
    if (sc_json_buf_append_raw(&body_buf, "\"}", 2) != SC_OK)
        goto tts_fail;

    size_t json_len = body_buf.len;
    char *json_body = (char *)alloc->alloc(alloc->ctx, json_len + 1);
    if (!json_body) {
        sc_json_buf_free(&body_buf);
        return SC_ERR_OUT_OF_MEMORY;
    }
    memcpy(json_body, body_buf.ptr, json_len);
    json_body[json_len] = '\0';
    sc_json_buf_free(&body_buf);

    /* Auth header */
    size_t auth_prefix = 22;
    char *auth_hdr = (char *)alloc->alloc(alloc->ctx, auth_prefix + config->api_key_len + 1);
    if (!auth_hdr) {
        alloc->free(alloc->ctx, json_body, json_len + 1);
        return SC_ERR_OUT_OF_MEMORY;
    }
    memcpy(auth_hdr, "Authorization: Bearer ", auth_prefix);
    memcpy(auth_hdr + auth_prefix, config->api_key, config->api_key_len);
    auth_hdr[auth_prefix + config->api_key_len] = '\0';

    /* argv: curl -s -X POST -H "Authorization: Bearer ..." -H "Content-Type: application/json" -d
     * '...' url */
    const char *argv[12];
    size_t argc = 0;
    argv[argc++] = "curl";
    argv[argc++] = "-s";
    argv[argc++] = "-X";
    argv[argc++] = "POST";
    argv[argc++] = "-H";
    argv[argc++] = auth_hdr;
    argv[argc++] = "-H";
    argv[argc++] = "Content-Type: application/json";
    argv[argc++] = "-d";
    argv[argc++] = json_body;
    argv[argc++] = endpoint;
    argv[argc] = NULL;

    sc_run_result_t result = {0};
    sc_error_t err = sc_process_run(alloc, argv, NULL, 4 * 1024 * 1024, &result);

    alloc->free(alloc->ctx, auth_hdr, auth_prefix + config->api_key_len + 1);
    alloc->free(alloc->ctx, json_body, json_len + 1);

    if (err != SC_OK) {
        sc_run_result_free(alloc, &result);
        return err;
    }
    if (!result.success || !result.stdout_buf) {
        sc_run_result_free(alloc, &result);
        return SC_ERR_PROVIDER_RESPONSE;
    }

    /* Response is raw audio bytes */
    if (result.stdout_len == 0) {
        sc_run_result_free(alloc, &result);
        return SC_ERR_PROVIDER_RESPONSE;
    }

    void *audio = alloc->alloc(alloc->ctx, result.stdout_len);
    if (!audio) {
        sc_run_result_free(alloc, &result);
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t audio_len = result.stdout_len;
    memcpy(audio, result.stdout_buf, audio_len);
    sc_run_result_free(alloc, &result);

    *out_audio = audio;
    *out_audio_len = audio_len;
    return SC_OK;

tts_fail:
    sc_json_buf_free(&body_buf);
    return SC_ERR_OUT_OF_MEMORY;
#endif
}

sc_error_t sc_voice_play(sc_allocator_t *alloc, const void *audio_data, size_t audio_len) {
    if (!alloc)
        return SC_ERR_INVALID_ARGUMENT;
    if (!audio_data && audio_len > 0)
        return SC_ERR_INVALID_ARGUMENT;

#if SC_IS_TEST
    (void)audio_data;
    (void)audio_len;
    return SC_OK;
#else
    char *tmp_dir = sc_platform_get_temp_dir(alloc);
    if (!tmp_dir)
        return SC_ERR_IO;

    char tmp_path[256];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s/seaclaw_tts_%d.mp3", tmp_dir, get_pid());
    alloc->free(alloc->ctx, tmp_dir, strlen(tmp_dir) + 1);
    if (n < 0 || (size_t)n >= sizeof(tmp_path))
        return SC_ERR_IO;

    FILE *f = fopen(tmp_path, "wb");
    if (!f)
        return SC_ERR_IO;
    if (audio_len > 0 && fwrite(audio_data, 1, audio_len, f) != audio_len) {
        fclose(f);
        unlink(tmp_path);
        return SC_ERR_IO;
    }
    fclose(f);

#if defined(__APPLE__)
    const char *argv[] = {"afplay", tmp_path, NULL};
    sc_run_result_t result = {0};
    sc_error_t err = sc_process_run(alloc, argv, NULL, 64 * 1024, &result);
    sc_run_result_free(alloc, &result);
#elif defined(__linux__)
    const char *argv_pa[] = {"paplay", tmp_path, NULL};
    sc_run_result_t result = {0};
    sc_error_t err = sc_process_run(alloc, argv_pa, NULL, 64 * 1024, &result);
    if (err != SC_OK || !result.success) {
        sc_run_result_free(alloc, &result);
        const char *argv_a[] = {"aplay", tmp_path, NULL};
        err = sc_process_run(alloc, argv_a, NULL, 64 * 1024, &result);
    }
    sc_run_result_free(alloc, &result);
#else
    unlink(tmp_path);
    return SC_ERR_NOT_SUPPORTED;
#endif

    unlink(tmp_path);
    return err;
#endif
}
