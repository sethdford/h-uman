#include "human/config.h"
#include "human/voice.h"
#include <string.h>

hu_error_t hu_voice_config_from_settings(const hu_config_t *config, hu_voice_config_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!config)
        return HU_OK;

    const hu_voice_settings_t *vs = &config->voice;
    out->local_stt_endpoint = vs->local_stt_endpoint;
    out->local_tts_endpoint = vs->local_tts_endpoint;
    out->stt_provider = vs->stt_provider;
    out->tts_provider = vs->tts_provider;
    out->stt_model = vs->stt_model;
    out->tts_model = vs->tts_model;
    out->tts_voice = vs->tts_voice;
    out->language = vs->stt_language;
    out->privacy_mode = vs->privacy_mode;

    const char *ckey = hu_config_get_provider_key(config, "cartesia");
    if (ckey && ckey[0]) {
        out->cartesia_api_key = ckey;
        out->cartesia_api_key_len = strlen(ckey);
    }

    const char *default_key = hu_config_default_provider_key(config);
    if (default_key && default_key[0]) {
        out->api_key = default_key;
        out->api_key_len = strlen(default_key);
    }

    if (vs->stt_provider && vs->stt_provider[0]) {
        const char *skey = hu_config_get_provider_key(config, vs->stt_provider);
        if (skey && skey[0] && strcmp(vs->stt_provider, "cartesia") != 0) {
            out->api_key = skey;
            out->api_key_len = strlen(skey);
        }
    }

    if (vs->mode && strcmp(vs->mode, "realtime") == 0) {
        const char *oai_key = hu_config_get_provider_key(config, "openai");
        if (oai_key && oai_key[0]) {
            out->openai_api_key = oai_key;
            out->openai_api_key_len = strlen(oai_key);
        }
    }

    return HU_OK;
}
