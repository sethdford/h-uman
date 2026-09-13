/*
 * Voice provider factory — create a provider from hu_config_t + mode string.
 * Centralizes API key lookup, model/voice defaults, and backend config construction.
 */
#include "human/config.h"
#include "human/core/endpoints.h"
#include "human/core/privacy.h"
#include "human/voice/gemini_live.h"
#include "human/voice/mlx_local.h"
#include "human/voice/provider.h"
#include <string.h>

/* Privacy kill-switch: the realtime cloud backends (Gemini Live / OpenAI
 * Realtime) stream microphone audio to the cloud. Under privacy mode only the
 * on-device "mlx_local" backend is permitted; cloud modes are refused at this
 * factory boundary so every caller (gateway cp_voice_stream, voice_channel,
 * daemon session-start) is covered by construction. */
static bool voice_rt_mode_blocked_by_privacy(const char *mode) {
    return hu_privacy_enforced() && strcmp(mode, "mlx_local") != 0;
}

hu_error_t hu_voice_provider_create_from_config(hu_allocator_t *alloc, const hu_config_t *config,
                                                const char *mode,
                                                const hu_voice_provider_extras_t *extras,
                                                hu_voice_provider_t *out) {
    if (!alloc || !config || !mode || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    if (voice_rt_mode_blocked_by_privacy(mode))
        return HU_ERR_NOT_SUPPORTED;

    const char *rt_model = (config->voice.realtime_model && config->voice.realtime_model[0])
                               ? config->voice.realtime_model
                               : config->voice.tts_model;
    const char *rt_voice = (config->voice.realtime_voice && config->voice.realtime_voice[0])
                               ? config->voice.realtime_voice
                               : config->voice.tts_voice;

    if (strcmp(mode, "gemini_live") == 0) {
        const char *key = (extras && extras->api_key && extras->api_key[0])
                              ? extras->api_key
                              : hu_config_get_provider_key(config, "google");
        if (!key || !key[0])
            key = hu_config_get_provider_key(config, "gemini");

        /* Vertex AI with ADC: allow missing API key when region + project +
         * access token are all present (token-based auth, no key needed). */
        const char *vtok = (extras && extras->vertex_access_token)
                               ? extras->vertex_access_token
                               : config->voice.vertex_access_token;
        const char *vregion =
            (extras && extras->vertex_region) ? extras->vertex_region : config->voice.vertex_region;
        const char *vproject = (extras && extras->vertex_project) ? extras->vertex_project
                                                                  : config->voice.vertex_project;
        bool has_vertex = (vtok && vtok[0] && vregion && vregion[0] && vproject && vproject[0]);
        if ((!key || !key[0]) && !has_vertex)
            return HU_ERR_GATEWAY_AUTH; /* no Google AI key or Vertex AI credentials */

        if (!rt_model || !rt_model[0])
            rt_model = "gemini-3.1-flash-live-preview";
        if (!rt_voice || !rt_voice[0])
            rt_voice = "Puck";

        hu_gemini_live_config_t glc = {
            .api_key = key ? key : "",
            .access_token = vtok,
            .model = rt_model,
            .voice = rt_voice,
            .region = vregion,
            .project_id = vproject,
            .transcribe_input = true,
            .transcribe_output = true,
            .affective_dialog = true,
            .manual_vad = true,
            .enable_session_resumption = true,
            .thinking_level = HU_GL_THINKING_MINIMAL,
        };
        if (extras) {
            glc.system_instruction = extras->system_instruction;
            glc.tools_json = extras->tools_json;
            if (extras->voice_id && extras->voice_id[0])
                glc.voice = extras->voice_id;
            if (extras->model_id && extras->model_id[0])
                glc.model = extras->model_id;
        }
        return hu_voice_provider_gemini_live_create(alloc, &glc, out);
    }

    if (strcmp(mode, "openai_realtime") == 0 || strcmp(mode, "realtime") == 0) {
        const char *key = (extras && extras->api_key && extras->api_key[0])
                              ? extras->api_key
                              : hu_config_get_provider_key(config, "openai");
        if (!key || !key[0])
            return HU_ERR_GATEWAY_AUTH; /* no OpenAI API key */

        if (!rt_model || !rt_model[0])
            rt_model = "gpt-4o-realtime-preview";
        if (!rt_voice || !rt_voice[0])
            rt_voice = "alloy";

        int sr = (extras && extras->sample_rate > 0) ? extras->sample_rate : 24000;
        hu_voice_rt_config_t rtc = {
            .api_key = key,
            .model = rt_model,
            .voice = rt_voice,
            .sample_rate = sr,
            .vad_enabled = true,
        };
        return hu_voice_provider_openai_create(alloc, &rtc, out);
    }

    if (strcmp(mode, "mlx_local") == 0) {
        hu_voice_mlx_local_config_t mlc = {
            .endpoint = HU_MLX_DEFAULT_ORIGIN,
            .model = rt_model,
            .voice_id = rt_voice,
            .sample_rate = (extras && extras->sample_rate > 0) ? extras->sample_rate : 24000,
        };
        if (extras) {
            mlc.system_prompt = extras->system_instruction;
            if (extras->model_id && extras->model_id[0])
                mlc.model = extras->model_id;
            if (extras->voice_id && extras->voice_id[0])
                mlc.voice_id = extras->voice_id;
        }
        return hu_voice_provider_mlx_local_create(alloc, &mlc, out);
    }

    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_voice_provider_create_from_extras(hu_allocator_t *alloc, const char *mode,
                                                const hu_voice_provider_extras_t *extras,
                                                hu_voice_provider_t *out) {
    if (!alloc || !mode || !extras || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    if (voice_rt_mode_blocked_by_privacy(mode))
        return HU_ERR_NOT_SUPPORTED;

    const char *model = (extras->model_id && extras->model_id[0]) ? extras->model_id : NULL;
    const char *voice = (extras->voice_id && extras->voice_id[0]) ? extras->voice_id : NULL;
    const char *key = extras->api_key;

    if (strcmp(mode, "gemini_live") == 0) {
        if (!model || !model[0])
            model = "gemini-3.1-flash-live-preview";
        if (!voice || !voice[0])
            voice = "Puck";
        hu_gemini_live_config_t glc = {
            .api_key = key,
            .access_token = extras->vertex_access_token,
            .model = model,
            .voice = voice,
            .region = extras->vertex_region,
            .project_id = extras->vertex_project,
            .system_instruction = extras->system_instruction,
            .tools_json = extras->tools_json,
            .transcribe_input = true,
            .transcribe_output = true,
            .affective_dialog = true,
            .manual_vad = true,
            .enable_session_resumption = true,
            .thinking_level = HU_GL_THINKING_MINIMAL,
        };
        return hu_voice_provider_gemini_live_create(alloc, &glc, out);
    }

    if (strcmp(mode, "openai_realtime") == 0 || strcmp(mode, "realtime") == 0) {
        if (!key || !key[0])
            return HU_ERR_INVALID_ARGUMENT;
        if (!model || !model[0])
            model = "gpt-4o-realtime-preview";
        if (!voice || !voice[0])
            voice = "alloy";
        int sr = extras->sample_rate > 0 ? extras->sample_rate : 24000;
        hu_voice_rt_config_t rtc = {
            .api_key = key,
            .model = model,
            .voice = voice,
            .sample_rate = sr,
            .vad_enabled = true,
        };
        return hu_voice_provider_openai_create(alloc, &rtc, out);
    }

    if (strcmp(mode, "mlx_local") == 0) {
        hu_voice_mlx_local_config_t mlc = {
            .endpoint = HU_MLX_DEFAULT_ORIGIN,
            .model = model,
            .system_prompt = extras->system_instruction,
            .voice_id = voice,
            .sample_rate = extras->sample_rate > 0 ? extras->sample_rate : 24000,
        };
        return hu_voice_provider_mlx_local_create(alloc, &mlc, out);
    }

    return HU_ERR_NOT_SUPPORTED;
}
