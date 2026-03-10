/*
 * Gateway handler for voice.transcribe — delegates to hu_voice_stt_gemini().
 */
#include "cp_internal.h"
#include "human/config.h"
#include "human/voice.h"
#include <string.h>

#ifdef HU_GATEWAY_POSIX

#define CP_VOICE_MAX_AUDIO_LEN (10 * 1024 * 1024) /* 10 MB base64 limit */

hu_error_t cp_voice_transcribe(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                               const hu_control_protocol_t *proto, const hu_json_value_t *root,
                               char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    *out = NULL;
    *out_len = 0;

    if (!app || !app->config)
        return HU_ERR_INVALID_ARGUMENT;

    /* Extract params */
    hu_json_value_t *params = hu_json_object_get((hu_json_value_t *)root, "params");
    if (!params)
        return HU_ERR_INVALID_ARGUMENT;

    hu_json_value_t *audio_val = hu_json_object_get(params, "audio");
    hu_json_value_t *mime_val = hu_json_object_get(params, "mimeType");

    if (!audio_val || audio_val->type != HU_JSON_STRING || audio_val->data.string.len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    if (audio_val->data.string.len > CP_VOICE_MAX_AUDIO_LEN)
        return HU_ERR_INVALID_ARGUMENT;

    const char *audio_b64 = audio_val->data.string.ptr;
    size_t audio_b64_len = audio_val->data.string.len;

    const char *mime_type = "audio/webm";
    if (mime_val && mime_val->type == HU_JSON_STRING && mime_val->data.string.len > 0)
        mime_type = mime_val->data.string.ptr;

    /* Get Gemini API key from config */
    const char *api_key = hu_config_get_provider_key((const hu_config_t *)app->config, "gemini");
    if (!api_key || api_key[0] == '\0')
        return HU_ERR_PROVIDER_AUTH;

    hu_voice_config_t voice_cfg = {0};
    voice_cfg.api_key = api_key;
    voice_cfg.api_key_len = strlen(api_key);

    char *text = NULL;
    size_t text_len = 0;
    hu_error_t err = hu_voice_stt_gemini(alloc, &voice_cfg, audio_b64, audio_b64_len, mime_type,
                                         &text, &text_len);
    if (err != HU_OK)
        return err;

    /* Build response: {"text":"..."} */
    hu_json_value_t *resp = hu_json_object_new(alloc);
    if (!resp) {
        alloc->free(alloc->ctx, text, text_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    hu_json_object_set(alloc, resp, "text", hu_json_string_new(alloc, text, text_len));
    alloc->free(alloc->ctx, text, text_len + 1);

    err = hu_json_stringify(alloc, resp, out, out_len);
    hu_json_free(alloc, resp);
    return err;
}

#endif /* HU_GATEWAY_POSIX */
