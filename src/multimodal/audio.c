#include "human/multimodal/audio.h"
#include "human/auth.h"
#include "human/core/file.h"
#include "human/core/string.h"
#include "human/multimodal.h"
#include "human/provider.h"
#include "human/voice.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define HU_OPENAI_AUDIO_TRANSCRIPTIONS_URL "https://api.openai.com/v1/audio/transcriptions"

static int to_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

static bool match_ext(const char *filename, size_t filename_len, const char *ext, size_t ext_len) {
    if (filename_len < ext_len + 1)
        return false;
    if (filename[filename_len - ext_len - 1] != '.')
        return false;
    for (size_t i = 0; i < ext_len; i++) {
        if (to_lower((unsigned char)filename[filename_len - ext_len + i]) != (unsigned char)ext[i])
            return false;
    }
    return true;
}

static bool is_supported_audio_ext(const char *path, size_t path_len) {
    return match_ext(path, path_len, "mp3", 3) || match_ext(path, path_len, "wav", 3) ||
           match_ext(path, path_len, "ogg", 3) || match_ext(path, path_len, "m4a", 3) ||
           match_ext(path, path_len, "caf", 3);
}

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
static hu_error_t audio_check_file_size(const char *path_z, size_t *out_size) {
    FILE *f = fopen(path_z, "rb");
    if (!f)
        return HU_ERR_NOT_FOUND;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    long sz = ftell(f);
    fclose(f);
    if (sz < 0)
        return HU_ERR_IO;
    *out_size = (size_t)sz;
    if (*out_size > HU_MULTIMODAL_MAX_AUDIO_SIZE)
        return HU_ERR_INVALID_ARGUMENT;
    return HU_OK;
}

static bool provider_name_is(const char *name, const char *want) {
    if (!name || !want)
        return false;
    return strcmp(name, want) == 0;
}
#endif

hu_error_t hu_multimodal_process_audio(hu_allocator_t *alloc, const char *file_path,
                                       size_t path_len, hu_provider_t *provider, const char *model,
                                       size_t model_len, char **out_text, size_t *out_text_len) {
    if (!alloc || !file_path || path_len == 0 || !provider || !out_text || !out_text_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (!provider->vtable || !provider->vtable->get_name)
        return HU_ERR_INVALID_ARGUMENT;
    *out_text = NULL;
    *out_text_len = 0;

    if (!is_supported_audio_ext(file_path, path_len))
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)model;
    (void)model_len;
    (void)file_path;
    const char *mock = "Mock audio transcription";
    size_t mlen = strlen(mock);
    char *dup = hu_strndup(alloc, mock, mlen);
    if (!dup)
        return HU_ERR_OUT_OF_MEMORY;
    *out_text = dup;
    *out_text_len = mlen;
    return HU_OK;
#else
    char path_buf[4096];
    if (path_len >= sizeof(path_buf))
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(path_buf, file_path, path_len);
    path_buf[path_len] = '\0';

    size_t file_size = 0;
    hu_error_t err = audio_check_file_size(path_buf, &file_size);
    if (err != HU_OK)
        return err;

    const char *pname = provider->vtable->get_name(provider->ctx);
    if (!pname)
        return HU_ERR_INVALID_ARGUMENT;

    char *api_key = hu_auth_get_api_key(alloc, pname);
    if (!api_key || api_key[0] == '\0') {
        if (api_key)
            alloc->free(alloc->ctx, api_key, strlen(api_key) + 1);
        return HU_ERR_PROVIDER_AUTH;
    }
    size_t api_key_len = strlen(api_key);

    char *model_owned = NULL;
    const char *model_use = NULL;
    if (model && model_len > 0) {
        model_owned = hu_strndup(alloc, model, model_len);
        if (!model_owned) {
            alloc->free(alloc->ctx, api_key, api_key_len + 1);
            return HU_ERR_OUT_OF_MEMORY;
        }
        model_use = model_owned;
    }

    if (provider_name_is(pname, "gemini")) {
        char *raw = NULL;
        size_t raw_len = 0;
        err = hu_file_slurp(alloc, path_buf, (size_t)HU_MULTIMODAL_MAX_AUDIO_SIZE, &raw, &raw_len);
        if (err != HU_OK) {
            alloc->free(alloc->ctx, api_key, api_key_len + 1);
            if (model_owned)
                alloc->free(alloc->ctx, model_owned, model_len + 1);
            return (err == HU_ERR_OUT_OF_MEMORY) ? HU_ERR_OUT_OF_MEMORY : HU_ERR_IO;
        }

        char *b64 = NULL;
        size_t b64_len = 0;
        err =
            hu_multimodal_encode_base64(alloc, (const unsigned char *)raw, raw_len, &b64, &b64_len);
        alloc->free(alloc->ctx, raw, raw_len + 1);
        if (err != HU_OK) {
            alloc->free(alloc->ctx, api_key, api_key_len + 1);
            if (model_owned)
                alloc->free(alloc->ctx, model_owned, model_len + 1);
            return err;
        }

        const char *mime = hu_multimodal_detect_audio_mime(file_path, path_len);
        hu_voice_config_t vcfg = {.api_key = api_key,
                                  .api_key_len = api_key_len,
                                  .stt_model = model_use,
                                  .stt_endpoint = NULL,
                                  .tts_endpoint = NULL,
                                  .tts_model = NULL,
                                  .tts_voice = NULL,
                                  .language = NULL};

        err = hu_voice_stt_gemini(alloc, &vcfg, b64, b64_len, mime, out_text, out_text_len);
        alloc->free(alloc->ctx, b64, b64_len + 1);
        alloc->free(alloc->ctx, api_key, api_key_len + 1);
        if (model_owned)
            alloc->free(alloc->ctx, model_owned, model_len + 1);
        return err;
    }

    const char *endpoint = NULL;
    if (provider_name_is(pname, "openai"))
        endpoint = HU_OPENAI_AUDIO_TRANSCRIPTIONS_URL;

    if (!model_use && provider_name_is(pname, "openai"))
        model_use = "whisper-1";

    hu_voice_config_t vcfg = {.api_key = api_key,
                              .api_key_len = api_key_len,
                              .stt_endpoint = endpoint,
                              .stt_model = model_use,
                              .tts_endpoint = NULL,
                              .tts_model = NULL,
                              .tts_voice = NULL,
                              .language = NULL};

    err = hu_voice_stt_file(alloc, &vcfg, path_buf, out_text, out_text_len);
    alloc->free(alloc->ctx, api_key, api_key_len + 1);
    if (model_owned)
        alloc->free(alloc->ctx, model_owned, model_len + 1);
    return err;
#endif
}
