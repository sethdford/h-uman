/*
 * Voice STT (Whisper) and TTS (OpenAI) via HTTP APIs.
 * In HU_IS_TEST mode, returns mock data without spawning curl.
 */
#include "human/voice.h"
#include "human/tts/cartesia.h"
#include "human/voice/local_stt.h"
#include "human/voice/local_tts.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include "human/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

#define HU_VOICE_STT_DEFAULT_ENDPOINT "https://api.groq.com/openai/v1/audio/transcriptions"
#define HU_VOICE_STT_DEFAULT_MODEL    "whisper-large-v3"
#define HU_VOICE_TTS_DEFAULT_ENDPOINT "https://api.openai.com/v1/audio/speech"
#define HU_VOICE_TTS_DEFAULT_MODEL    "tts-1"
#define HU_VOICE_TTS_DEFAULT_VOICE    "alloy"

#if !HU_IS_TEST
static int get_pid(void) {
#if defined(__linux__)
    return (int)getpid();
#elif defined(__APPLE__) || defined(__FreeBSD__)
    return (int)getpid();
#else
    return 0;
#endif
}
#endif /* !HU_IS_TEST */

hu_error_t hu_voice_stt_file(hu_allocator_t *alloc, const hu_voice_config_t *config,
                             const char *file_path, char **out_text, size_t *out_len) {
    if (!alloc || !config || !out_text || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_text = NULL;
    *out_len = 0;

    if (!file_path || file_path[0] == '\0')
        return HU_ERR_INVALID_ARGUMENT;

#if HU_IS_TEST
    if (config->local_stt_endpoint && config->local_stt_endpoint[0]) {
        hu_local_stt_config_t lc = {.endpoint = config->local_stt_endpoint,
                                    .model = config->stt_model,
                                    .language = config->language};
        return hu_local_stt_transcribe(alloc, &lc, file_path, out_text, out_len);
    }
    if (config->stt_provider && strcmp(config->stt_provider, "cartesia") == 0) {
        const char *ckey = config->cartesia_api_key;
        size_t cklen = config->cartesia_api_key_len;
        if (!ckey || cklen == 0) {
            ckey = "test-cartesia-key";
            cklen = 17;
        }
        hu_cartesia_stt_config_t sc = {.model = config->stt_model, .language = config->language};
        return hu_cartesia_stt_transcribe(alloc, ckey, cklen, file_path, &sc, out_text, out_len);
    }
    if (!config->api_key || config->api_key_len == 0)
        return HU_ERR_PROVIDER_AUTH;
    {
        (void)file_path;
        const char *prefix = "This is a mock transcription of ";
        size_t plen = strlen(prefix);
        size_t fplen = file_path ? strlen(file_path) : 0;
        char *mock = (char *)alloc->alloc(alloc->ctx, plen + fplen + 1);
        if (!mock)
            return HU_ERR_OUT_OF_MEMORY;
        memcpy(mock, prefix, plen);
        if (fplen > 0)
            memcpy(mock + plen, file_path, fplen);
        mock[plen + fplen] = '\0';
        *out_text = mock;
        *out_len = plen + fplen;
        return HU_OK;
    }
#else
    if (config->local_stt_endpoint && config->local_stt_endpoint[0]) {
        hu_local_stt_config_t lc = {.endpoint = config->local_stt_endpoint,
                                    .model = config->stt_model,
                                    .language = config->language};
        hu_error_t le = hu_local_stt_transcribe(alloc, &lc, file_path, out_text, out_len);
        if (le == HU_OK && *out_text && *out_len > 0)
            return HU_OK;
        if (*out_text) {
            alloc->free(alloc->ctx, *out_text, *out_len + 1);
            *out_text = NULL;
            *out_len = 0;
        }
    }

    if (config->stt_provider && strcmp(config->stt_provider, "cartesia") == 0) {
        const char *ckey = config->cartesia_api_key;
        size_t cklen = config->cartesia_api_key_len;
        if ((!ckey || cklen == 0)) {
            const char *env = getenv("CARTESIA_API_KEY");
            if (env && env[0]) {
                ckey = env;
                cklen = strlen(env);
            }
        }
        if (!ckey || cklen == 0)
            return HU_ERR_PROVIDER_AUTH;
        hu_cartesia_stt_config_t sc = {.model = config->stt_model, .language = config->language};
        return hu_cartesia_stt_transcribe(alloc, ckey, cklen, file_path, &sc, out_text, out_len);
    }

    if (!config->api_key || config->api_key_len == 0)
        return HU_ERR_PROVIDER_AUTH;

    const char *endpoint = config->stt_endpoint && config->stt_endpoint[0]
                               ? config->stt_endpoint
                               : HU_VOICE_STT_DEFAULT_ENDPOINT;
    const char *model =
        config->stt_model && config->stt_model[0] ? config->stt_model : HU_VOICE_STT_DEFAULT_MODEL;

    /* Build Authorization header: "Authorization: Bearer " + api_key */
    size_t auth_prefix = 22; /* "Authorization: Bearer " */
    char *auth_hdr = (char *)alloc->alloc(alloc->ctx, auth_prefix + config->api_key_len + 1);
    if (!auth_hdr)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(auth_hdr, "Authorization: Bearer ", auth_prefix);
    memcpy(auth_hdr + auth_prefix, config->api_key, config->api_key_len);
    auth_hdr[auth_prefix + config->api_key_len] = '\0';

    /* Build file=@path and model=X */
    size_t file_arg_cap = 128 + (file_path ? strlen(file_path) : 0);
    char *file_arg = (char *)alloc->alloc(alloc->ctx, file_arg_cap);
    if (!file_arg) {
        alloc->free(alloc->ctx, auth_hdr, auth_prefix + config->api_key_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(file_arg, file_arg_cap, "file=@%s", file_path ? file_path : "");
    if (n < 0 || (size_t)n >= file_arg_cap) {
        alloc->free(alloc->ctx, auth_hdr, auth_prefix + config->api_key_len + 1);
        alloc->free(alloc->ctx, file_arg, file_arg_cap);
        return HU_ERR_INVALID_ARGUMENT;
    }

    size_t model_arg_cap = 32 + strlen(model);
    char *model_arg = (char *)alloc->alloc(alloc->ctx, model_arg_cap);
    if (!model_arg) {
        alloc->free(alloc->ctx, auth_hdr, auth_prefix + config->api_key_len + 1);
        alloc->free(alloc->ctx, file_arg, file_arg_cap);
        return HU_ERR_OUT_OF_MEMORY;
    }
    n = snprintf(model_arg, model_arg_cap, "model=%s", model);
    if (n < 0 || (size_t)n >= model_arg_cap) {
        alloc->free(alloc->ctx, auth_hdr, auth_prefix + config->api_key_len + 1);
        alloc->free(alloc->ctx, file_arg, file_arg_cap);
        alloc->free(alloc->ctx, model_arg, model_arg_cap);
        return HU_ERR_INVALID_ARGUMENT;
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
            return HU_ERR_OUT_OF_MEMORY;
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
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run(alloc, argv, NULL, 4 * 1024 * 1024, &result);
    alloc->free(alloc->ctx, auth_hdr, auth_prefix + config->api_key_len + 1);
    alloc->free(alloc->ctx, file_arg, file_arg_cap);
    alloc->free(alloc->ctx, model_arg, model_arg_cap);
    if (lang_arg)
        alloc->free(alloc->ctx, lang_arg, lang_arg_cap);

    if (err != HU_OK) {
        hu_run_result_free(alloc, &result);
        return err;
    }
    if (!result.success || !result.stdout_buf) {
        hu_run_result_free(alloc, &result);
        return HU_ERR_PROVIDER_RESPONSE;
    }

    /* Parse {"text":"..."} */
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
    char *text = hu_strndup(alloc, text_val->data.string.ptr, text_len);
    hu_json_free(alloc, parsed);
    if (!text)
        return HU_ERR_OUT_OF_MEMORY;

    *out_text = text;
    *out_len = text_len;
    return HU_OK;
#endif
}

hu_error_t hu_voice_stt(hu_allocator_t *alloc, const hu_voice_config_t *config,
                        const void *audio_data, size_t audio_len, char **out_text,
                        size_t *out_len) {
    if (!alloc || !config || !out_text || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (!audio_data && audio_len > 0)
        return HU_ERR_INVALID_ARGUMENT;
    *out_text = NULL;
    *out_len = 0;

#if HU_IS_TEST
    {
        (void)audio_data;
        (void)audio_len;
        const char *mock = "Mock transcription";
        char *dup = hu_strndup(alloc, mock, strlen(mock));
        if (!dup)
            return HU_ERR_OUT_OF_MEMORY;
        *out_text = dup;
        *out_len = strlen(mock);
        return HU_OK;
    }
#else
    if (!audio_data && audio_len > 0)
        return HU_ERR_INVALID_ARGUMENT;

    char *tmp_dir = hu_platform_get_temp_dir(alloc);
    if (!tmp_dir)
        return HU_ERR_IO;

    char tmp_path[256];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s/human_voice_%d.bin", tmp_dir, get_pid());
    alloc->free(alloc->ctx, tmp_dir, strlen(tmp_dir) + 1);
    if (n < 0 || (size_t)n >= sizeof(tmp_path))
        return HU_ERR_IO;

    FILE *f = fopen(tmp_path, "wb");
    if (!f)
        return HU_ERR_IO;
    if (audio_len > 0 && fwrite(audio_data, 1, audio_len, f) != audio_len) {
        fclose(f);
        unlink(tmp_path);
        return HU_ERR_IO;
    }
    fclose(f);

    hu_error_t err = hu_voice_stt_file(alloc, config, tmp_path, out_text, out_len);
    unlink(tmp_path);
    return err;
#endif
}

hu_error_t hu_voice_tts(hu_allocator_t *alloc, const hu_voice_config_t *config, const char *text,
                        size_t text_len, void **out_audio, size_t *out_audio_len) {
    if (!alloc || !config || !out_audio || !out_audio_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_audio = NULL;
    *out_audio_len = 0;

#if HU_IS_TEST
    if (config->local_tts_endpoint && config->local_tts_endpoint[0]) {
        hu_local_tts_config_t lc = {.endpoint = config->local_tts_endpoint,
                                    .model = config->tts_model,
                                    .voice = config->tts_voice};
        char *path = NULL;
        hu_error_t le = hu_local_tts_synthesize(alloc, &lc, text, &path);
        if (le != HU_OK) {
            if (path) {
                (void)unlink(path);
                alloc->free(alloc->ctx, path, strlen(path) + 1);
            }
            return le;
        }
        if (path) {
            (void)unlink(path);
            alloc->free(alloc->ctx, path, strlen(path) + 1);
        }
        void *buf = alloc->alloc(alloc->ctx, 1);
        if (!buf)
            return HU_ERR_OUT_OF_MEMORY;
        *out_audio = buf;
        *out_audio_len = 0;
        return HU_OK;
    }
    if (config->tts_provider && strcmp(config->tts_provider, "cartesia") == 0) {
        const char *ckey = config->cartesia_api_key;
        size_t cklen = config->cartesia_api_key_len;
        if (!ckey || cklen == 0) {
            ckey = "test-cartesia-key";
            cklen = 17;
        }
        hu_cartesia_tts_config_t tc = {
            .model_id = config->tts_model, .voice_id = config->tts_voice};
        unsigned char *bytes = NULL;
        size_t blen = 0;
        hu_error_t ce =
            hu_cartesia_tts_synthesize(alloc, ckey, cklen, text, text_len, &tc, "mp3", &bytes, &blen);
        if (ce != HU_OK)
            return ce;
        *out_audio = bytes;
        *out_audio_len = blen;
        return HU_OK;
    }
    if (!config->api_key || config->api_key_len == 0)
        return HU_ERR_PROVIDER_AUTH;
    {
        (void)text;
        (void)text_len;
        static const unsigned char mock_audio[4] = {0x00, 0x01, 0x02, 0x03};
        void *buf = alloc->alloc(alloc->ctx, 4);
        if (!buf)
            return HU_ERR_OUT_OF_MEMORY;
        memcpy(buf, mock_audio, 4);
        *out_audio = buf;
        *out_audio_len = 4;
        return HU_OK;
    }
#else
    if (config->local_tts_endpoint && config->local_tts_endpoint[0]) {
        hu_local_tts_config_t lc = {.endpoint = config->local_tts_endpoint,
                                    .model = config->tts_model,
                                    .voice = config->tts_voice};
        char *path = NULL;
        hu_error_t le = hu_local_tts_synthesize(alloc, &lc, text, &path);
        if (le != HU_OK) {
            if (path) {
                (void)unlink(path);
                alloc->free(alloc->ctx, path, strlen(path) + 1);
            }
            /* fall through to cloud */
        } else if (path) {
            FILE *af = fopen(path, "rb");
            if (!af) {
                (void)unlink(path);
                alloc->free(alloc->ctx, path, strlen(path) + 1);
            } else {
                if (fseek(af, 0, SEEK_END) != 0) {
                    fclose(af);
                    (void)unlink(path);
                    alloc->free(alloc->ctx, path, strlen(path) + 1);
                } else {
                    long sz = ftell(af);
                    if (sz < 0) {
                        fclose(af);
                        (void)unlink(path);
                        alloc->free(alloc->ctx, path, strlen(path) + 1);
                    } else {
                        if (fseek(af, 0, SEEK_SET) != 0) {
                            fclose(af);
                            (void)unlink(path);
                            alloc->free(alloc->ctx, path, strlen(path) + 1);
                        } else {
                            size_t audio_sz = (size_t)sz;
                            size_t raw_cap = audio_sz ? audio_sz : 1;
                            void *raw = alloc->alloc(alloc->ctx, raw_cap);
                            if (!raw) {
                                fclose(af);
                                (void)unlink(path);
                                alloc->free(alloc->ctx, path, strlen(path) + 1);
                            } else {
                                if (audio_sz > 0 &&
                                    fread(raw, 1, audio_sz, af) != audio_sz) {
                                    fclose(af);
                                    (void)unlink(path);
                                    alloc->free(alloc->ctx, path, strlen(path) + 1);
                                    alloc->free(alloc->ctx, raw, raw_cap);
                                } else {
                                    fclose(af);
                                    (void)unlink(path);
                                    alloc->free(alloc->ctx, path, strlen(path) + 1);
                                    *out_audio = raw;
                                    *out_audio_len = audio_sz;
                                    return HU_OK;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (config->tts_provider && strcmp(config->tts_provider, "cartesia") == 0) {
        const char *ckey = config->cartesia_api_key;
        size_t cklen = config->cartesia_api_key_len;
        if ((!ckey || cklen == 0)) {
            const char *env = getenv("CARTESIA_API_KEY");
            if (env && env[0]) {
                ckey = env;
                cklen = strlen(env);
            }
        }
        if (!ckey || cklen == 0)
            return HU_ERR_PROVIDER_AUTH;
        hu_cartesia_tts_config_t tc = {
            .model_id = config->tts_model, .voice_id = config->tts_voice};
        unsigned char *bytes = NULL;
        size_t blen = 0;
        hu_error_t ce =
            hu_cartesia_tts_synthesize(alloc, ckey, cklen, text, text_len, &tc, "mp3", &bytes, &blen);
        if (ce != HU_OK)
            return ce;
        *out_audio = bytes;
        *out_audio_len = blen;
        return HU_OK;
    }

    if (!config->api_key || config->api_key_len == 0)
        return HU_ERR_PROVIDER_AUTH;

    const char *endpoint = config->tts_endpoint && config->tts_endpoint[0]
                               ? config->tts_endpoint
                               : HU_VOICE_TTS_DEFAULT_ENDPOINT;
    const char *model =
        config->tts_model && config->tts_model[0] ? config->tts_model : HU_VOICE_TTS_DEFAULT_MODEL;
    const char *voice =
        config->tts_voice && config->tts_voice[0] ? config->tts_voice : HU_VOICE_TTS_DEFAULT_VOICE;

    /* Build JSON body: {"model":"tts-1","input":"<escaped>","voice":"alloy"} */
    hu_json_buf_t body_buf = {0};
    if (hu_json_buf_init(&body_buf, alloc) != HU_OK)
        return HU_ERR_OUT_OF_MEMORY;
    if (hu_json_buf_append_raw(&body_buf, "{\"model\":\"", 10) != HU_OK)
        goto tts_fail;
    if (hu_json_append_string(&body_buf, model, strlen(model)) != HU_OK)
        goto tts_fail;
    if (hu_json_buf_append_raw(&body_buf, "\",\"input\":", 10) != HU_OK)
        goto tts_fail;
    if (hu_json_append_string(&body_buf, text ? text : "", text_len) != HU_OK)
        goto tts_fail;
    if (hu_json_buf_append_raw(&body_buf, "\",\"voice\":\"", 11) != HU_OK)
        goto tts_fail;
    if (hu_json_append_string(&body_buf, voice, strlen(voice)) != HU_OK)
        goto tts_fail;
    if (hu_json_buf_append_raw(&body_buf, "\"}", 2) != HU_OK)
        goto tts_fail;

    size_t json_len = body_buf.len;
    char *json_body = (char *)alloc->alloc(alloc->ctx, json_len + 1);
    if (!json_body) {
        hu_json_buf_free(&body_buf);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(json_body, body_buf.ptr, json_len);
    json_body[json_len] = '\0';
    hu_json_buf_free(&body_buf);

    /* Auth header */
    size_t auth_prefix = 22;
    char *auth_hdr = (char *)alloc->alloc(alloc->ctx, auth_prefix + config->api_key_len + 1);
    if (!auth_hdr) {
        alloc->free(alloc->ctx, json_body, json_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
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

    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run(alloc, argv, NULL, 4 * 1024 * 1024, &result);

    alloc->free(alloc->ctx, auth_hdr, auth_prefix + config->api_key_len + 1);
    alloc->free(alloc->ctx, json_body, json_len + 1);

    if (err != HU_OK) {
        hu_run_result_free(alloc, &result);
        return err;
    }
    if (!result.success || !result.stdout_buf) {
        hu_run_result_free(alloc, &result);
        return HU_ERR_PROVIDER_RESPONSE;
    }

    /* Response is raw audio bytes */
    if (result.stdout_len == 0) {
        hu_run_result_free(alloc, &result);
        return HU_ERR_PROVIDER_RESPONSE;
    }

    void *audio = alloc->alloc(alloc->ctx, result.stdout_len);
    if (!audio) {
        hu_run_result_free(alloc, &result);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t audio_len = result.stdout_len;
    memcpy(audio, result.stdout_buf, audio_len);
    hu_run_result_free(alloc, &result);

    *out_audio = audio;
    *out_audio_len = audio_len;
    return HU_OK;

tts_fail:
    hu_json_buf_free(&body_buf);
    return HU_ERR_OUT_OF_MEMORY;
#endif
}

#define HU_VOICE_GEMINI_DEFAULT_ENDPOINT "https://generativelanguage.googleapis.com/v1beta/models/"
#define HU_VOICE_GEMINI_DEFAULT_MODEL    "gemini-3.1-flash-lite-preview"
#define HU_VOICE_GEMINI_PROMPT                                \
    "Transcribe the following audio exactly, word for word. " \
    "Output only the spoken words, nothing else. "            \
    "If the audio is silent or unintelligible, output an empty string."
#define HU_VOICE_GEMINI_VIDEO_PROMPT                                                   \
    "Describe this video: summarize visible scenes, actions, people or objects, and "  \
    "any readable text. Be concise but complete."

hu_error_t hu_voice_stt_gemini(hu_allocator_t *alloc, const hu_voice_config_t *config,
                               const char *audio_base64, size_t audio_base64_len,
                               const char *mime_type, char **out_text, size_t *out_len) {
    if (!alloc || !config || !out_text || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_text = NULL;
    *out_len = 0;

    if (!config->api_key || config->api_key_len == 0)
        return HU_ERR_PROVIDER_AUTH;
    if (!audio_base64 || audio_base64_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (!mime_type)
        mime_type = "audio/webm";

#if HU_IS_TEST
    {
        (void)audio_base64;
        (void)audio_base64_len;
        (void)mime_type;
        const char *mock = "Mock Gemini transcription";
        char *dup = hu_strndup(alloc, mock, strlen(mock));
        if (!dup)
            return HU_ERR_OUT_OF_MEMORY;
        *out_text = dup;
        *out_len = strlen(mock);
        return HU_OK;
    }
#else
    const char *model = config->stt_model && config->stt_model[0] ? config->stt_model
                                                                  : HU_VOICE_GEMINI_DEFAULT_MODEL;
    const char *base_url = config->stt_endpoint && config->stt_endpoint[0]
                               ? config->stt_endpoint
                               : HU_VOICE_GEMINI_DEFAULT_ENDPOINT;

    /* Build URL: {base_url}{model}:generateContent?key={api_key} */
    size_t url_cap = strlen(base_url) + strlen(model) + 64 + config->api_key_len;
    char *url = (char *)alloc->alloc(alloc->ctx, url_cap);
    if (!url)
        return HU_ERR_OUT_OF_MEMORY;
    int n = snprintf(url, url_cap, "%s%s:generateContent?key=%.*s", base_url, model,
                     (int)config->api_key_len, config->api_key);
    if (n < 0 || (size_t)n >= url_cap) {
        alloc->free(alloc->ctx, url, url_cap);
        return HU_ERR_INVALID_ARGUMENT;
    }

    /*
     * Build JSON body:
     * {"contents":[{"parts":[
     *   {"text":"<prompt>"},
     *   {"inline_data":{"mime_type":"<mt>","data":"<b64>"}}
     * ]}]}
     */
    size_t json_cap = 512 + audio_base64_len + strlen(mime_type);
    char *json = (char *)alloc->alloc(alloc->ctx, json_cap);
    if (!json) {
        alloc->free(alloc->ctx, url, url_cap);
        return HU_ERR_OUT_OF_MEMORY;
    }
    const char *prompt = HU_VOICE_GEMINI_PROMPT;
    if (mime_type && strncmp(mime_type, "video/", 6) == 0)
        prompt = HU_VOICE_GEMINI_VIDEO_PROMPT;

    n = snprintf(json, json_cap,
                 "{\"contents\":[{\"parts\":["
                 "{\"text\":\"%s\"},"
                 "{\"inline_data\":{\"mime_type\":\"%s\",\"data\":\"%.*s\"}}"
                 "]}]}",
                 prompt, mime_type, (int)audio_base64_len, audio_base64);
    if (n < 0 || (size_t)n >= json_cap) {
        alloc->free(alloc->ctx, json, json_cap);
        alloc->free(alloc->ctx, url, url_cap);
        return HU_ERR_INVALID_ARGUMENT;
    }
    size_t json_len = (size_t)n;

    /* Write JSON to temp file (too large for argv) */
    char *tmp_dir = hu_platform_get_temp_dir(alloc);
    if (!tmp_dir) {
        alloc->free(alloc->ctx, json, json_cap);
        alloc->free(alloc->ctx, url, url_cap);
        return HU_ERR_IO;
    }
    char tmp_path[256];
    n = snprintf(tmp_path, sizeof(tmp_path), "%s/human_gemini_stt_%d.json", tmp_dir, get_pid());
    alloc->free(alloc->ctx, tmp_dir, strlen(tmp_dir) + 1);
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        alloc->free(alloc->ctx, json, json_cap);
        alloc->free(alloc->ctx, url, url_cap);
        return HU_ERR_IO;
    }

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        alloc->free(alloc->ctx, json, json_cap);
        alloc->free(alloc->ctx, url, url_cap);
        return HU_ERR_IO;
    }
    if (fwrite(json, 1, json_len, f) != json_len) {
        fclose(f);
        unlink(tmp_path);
        alloc->free(alloc->ctx, json, json_cap);
        alloc->free(alloc->ctx, url, url_cap);
        return HU_ERR_IO;
    }
    fclose(f);
    alloc->free(alloc->ctx, json, json_cap);

    /* Build curl data-file arg: @<temp_dir>/human_gemini_stt_XXX.json */
    char data_arg[280];
    n = snprintf(data_arg, sizeof(data_arg), "@%s", tmp_path);
    if (n < 0 || (size_t)n >= sizeof(data_arg)) {
        unlink(tmp_path);
        alloc->free(alloc->ctx, url, url_cap);
        return HU_ERR_IO;
    }

    const char *argv[] = {"curl", "-s",     "-X", "POST", "-H", "Content-Type: application/json",
                          "-d",   data_arg, url,  NULL};

    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run(alloc, argv, NULL, 4 * 1024 * 1024, &result);
    unlink(tmp_path);
    alloc->free(alloc->ctx, url, url_cap);

    if (err != HU_OK) {
        hu_run_result_free(alloc, &result);
        return err;
    }
    if (!result.success || !result.stdout_buf) {
        hu_run_result_free(alloc, &result);
        return HU_ERR_PROVIDER_RESPONSE;
    }

    /* Parse: candidates[0].content.parts[0].text */
    hu_json_value_t *parsed = NULL;
    err = hu_json_parse(alloc, result.stdout_buf, result.stdout_len, &parsed);
    hu_run_result_free(alloc, &result);
    if (err != HU_OK)
        return HU_ERR_PARSE;

    hu_json_value_t *candidates = hu_json_object_get(parsed, "candidates");
    if (!candidates || candidates->type != HU_JSON_ARRAY || candidates->data.array.len == 0) {
        hu_json_free(alloc, parsed);
        return HU_ERR_PARSE;
    }
    hu_json_value_t *first = candidates->data.array.items[0];
    hu_json_value_t *content = hu_json_object_get(first, "content");
    if (!content) {
        hu_json_free(alloc, parsed);
        return HU_ERR_PARSE;
    }
    hu_json_value_t *parts = hu_json_object_get(content, "parts");
    if (!parts || parts->type != HU_JSON_ARRAY || parts->data.array.len == 0) {
        hu_json_free(alloc, parsed);
        return HU_ERR_PARSE;
    }
    hu_json_value_t *text_val = hu_json_object_get(parts->data.array.items[0], "text");
    if (!text_val || text_val->type != HU_JSON_STRING) {
        hu_json_free(alloc, parsed);
        return HU_ERR_PARSE;
    }

    size_t text_len = text_val->data.string.len;
    char *text = hu_strndup(alloc, text_val->data.string.ptr, text_len);
    hu_json_free(alloc, parsed);
    if (!text)
        return HU_ERR_OUT_OF_MEMORY;

    *out_text = text;
    *out_len = text_len;
    return HU_OK;
#endif
}

hu_error_t hu_voice_play(hu_allocator_t *alloc, const void *audio_data, size_t audio_len) {
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (!audio_data && audio_len > 0)
        return HU_ERR_INVALID_ARGUMENT;

#if HU_IS_TEST
    (void)audio_data;
    (void)audio_len;
    return HU_OK;
#else
    char *tmp_dir = hu_platform_get_temp_dir(alloc);
    if (!tmp_dir)
        return HU_ERR_IO;

    char tmp_path[256];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s/human_tts_%d.mp3", tmp_dir, get_pid());
    alloc->free(alloc->ctx, tmp_dir, strlen(tmp_dir) + 1);
    if (n < 0 || (size_t)n >= sizeof(tmp_path))
        return HU_ERR_IO;

    FILE *f = fopen(tmp_path, "wb");
    if (!f)
        return HU_ERR_IO;
    if (audio_len > 0 && fwrite(audio_data, 1, audio_len, f) != audio_len) {
        fclose(f);
        unlink(tmp_path);
        return HU_ERR_IO;
    }
    fclose(f);

#if defined(__APPLE__)
    const char *argv[] = {"afplay", tmp_path, NULL};
    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run(alloc, argv, NULL, 64 * 1024, &result);
    hu_run_result_free(alloc, &result);
#elif defined(__linux__)
    const char *argv_pa[] = {"paplay", tmp_path, NULL};
    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run(alloc, argv_pa, NULL, 64 * 1024, &result);
    if (err != HU_OK || !result.success) {
        hu_run_result_free(alloc, &result);
        const char *argv_a[] = {"aplay", tmp_path, NULL};
        err = hu_process_run(alloc, argv_a, NULL, 64 * 1024, &result);
    }
    hu_run_result_free(alloc, &result);
#else
    unlink(tmp_path);
    return HU_ERR_NOT_SUPPORTED;
#endif

    unlink(tmp_path);
    return err;
#endif
}
