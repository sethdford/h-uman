/*
 * Gateway handler for voice.transcribe — delegates to sc_voice_stt_gemini().
 */
#include "cp_internal.h"
#include "seaclaw/config.h"
#include "seaclaw/voice.h"
#include <string.h>

#ifdef SC_GATEWAY_POSIX

#define CP_VOICE_MAX_AUDIO_LEN (10 * 1024 * 1024) /* 10 MB base64 limit */

sc_error_t cp_voice_transcribe(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                               const sc_control_protocol_t *proto, const sc_json_value_t *root,
                               char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    *out = NULL;
    *out_len = 0;

    if (!app || !app->config)
        return SC_ERR_INVALID_ARGUMENT;

    /* Extract params */
    sc_json_value_t *params = sc_json_object_get((sc_json_value_t *)root, "params");
    if (!params)
        return SC_ERR_INVALID_ARGUMENT;

    sc_json_value_t *audio_val = sc_json_object_get(params, "audio");
    sc_json_value_t *mime_val = sc_json_object_get(params, "mimeType");

    if (!audio_val || audio_val->type != SC_JSON_STRING || audio_val->data.string.len == 0)
        return SC_ERR_INVALID_ARGUMENT;

    if (audio_val->data.string.len > CP_VOICE_MAX_AUDIO_LEN)
        return SC_ERR_INVALID_ARGUMENT;

    const char *audio_b64 = audio_val->data.string.ptr;
    size_t audio_b64_len = audio_val->data.string.len;

    const char *mime_type = "audio/webm";
    if (mime_val && mime_val->type == SC_JSON_STRING && mime_val->data.string.len > 0)
        mime_type = mime_val->data.string.ptr;

    /* Get Gemini API key from config */
    const char *api_key = sc_config_get_provider_key((const sc_config_t *)app->config, "gemini");
    if (!api_key || api_key[0] == '\0')
        return SC_ERR_PROVIDER_AUTH;

    sc_voice_config_t voice_cfg = {0};
    voice_cfg.api_key = api_key;
    voice_cfg.api_key_len = strlen(api_key);

    char *text = NULL;
    size_t text_len = 0;
    sc_error_t err = sc_voice_stt_gemini(alloc, &voice_cfg, audio_b64, audio_b64_len, mime_type,
                                         &text, &text_len);
    if (err != SC_OK)
        return err;

    /* Build response: {"text":"..."} */
    sc_json_value_t *resp = sc_json_object_new(alloc);
    if (!resp) {
        alloc->free(alloc->ctx, text, text_len + 1);
        return SC_ERR_OUT_OF_MEMORY;
    }
    sc_json_object_set(alloc, resp, "text", sc_json_string_new(alloc, text, text_len));
    alloc->free(alloc->ctx, text, text_len + 1);

    err = sc_json_stringify(alloc, resp, out, out_len);
    sc_json_free(alloc, resp);
    return err;
}

#endif /* SC_GATEWAY_POSIX */
