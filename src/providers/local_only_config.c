#include "human/providers/local_only_config.h"
#include "human/core/local_only_guard.h"
#include "human/core/log.h"
#include "human/provider.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *hu_config_primary_provider_name(const hu_config_t *cfg) {
    if (!cfg || !cfg->default_provider || !cfg->default_provider[0])
        return NULL;
    const char *name = cfg->default_provider;
    if (strcmp(name, "reliable") == 0)
        return cfg->reliability.primary_provider && cfg->reliability.primary_provider[0]
                   ? cfg->reliability.primary_provider
                   : "openai";
    if (strcmp(name, "router") == 0)
        return cfg->router.standard && cfg->router.standard[0] ? cfg->router.standard : "openai";
    return name;
}

const char *hu_config_primary_endpoint(const hu_config_t *cfg) {
    const char *name = hu_config_primary_provider_name(cfg);
    if (!name)
        return NULL;
    const char *url = hu_config_get_provider_base_url(cfg, name);
    if (url && url[0])
        return url;
    return hu_compatible_provider_url(name);
}

static int provider_local_override(const hu_config_t *cfg, const char *name) {
    for (size_t i = 0; cfg && name && i < cfg->providers_len; i++)
        if (cfg->providers[i].name && strcmp(cfg->providers[i].name, name) == 0)
            return cfg->providers[i].local_override;
    return 0;
}

bool hu_config_primary_is_local(const hu_config_t *cfg) {
    const char *name = hu_config_primary_provider_name(cfg);
    if (!name)
        return false;
    return hu_local_only_provider_endpoint_is_local(name, hu_config_primary_endpoint(cfg),
                                                    provider_local_override(cfg, name));
}

/* privacy.local_only_allow, else {"tts:cartesia", "stt:<voice.stt_provider
 * or cartesia>"}. Cartesia is the owner's voice vendor (providers[cartesia]);
 * prod's inbound memos are transcribed on-device by iOS, so stt is a backup. */
static void apply_allow_list(const hu_config_t *cfg) {
    if (cfg && cfg->privacy.local_only_allow_set) {
        hu_local_only_set_allow((const char *const *)cfg->privacy.local_only_allow,
                                cfg->privacy.local_only_allow_len);
        return;
    }
    char stt[48];
    const char *sp = cfg && cfg->voice.stt_provider && cfg->voice.stt_provider[0]
                         ? cfg->voice.stt_provider
                         : "cartesia";
    int n = snprintf(stt, sizeof(stt), "stt:%s", sp);
    const char *items[2] = {"tts:cartesia", (n > 0 && (size_t)n < sizeof(stt)) ? stt : NULL};
    hu_local_only_set_allow(items, items[1] ? 2 : 1);
}

hu_gate_mode_t hu_config_local_only_mode(const hu_config_t *cfg) {
    int cfg_value = -1;
    if (cfg && cfg->privacy.local_only_set)
        cfg_value = cfg->privacy.local_only ? 1 : 0;
    return hu_local_only_resolve(getenv("HU_LOCAL_ONLY"), cfg_value,
                                 hu_config_primary_is_local(cfg));
}

hu_gate_mode_t hu_config_apply_local_only(const hu_config_t *cfg) {
    hu_gate_mode_t mode = hu_config_local_only_mode(cfg);
    hu_local_only_configure(mode);
    apply_allow_list(cfg);
    const char *why = hu_local_only_env_parse(getenv("HU_LOCAL_ONLY")) >= 0 ? "HU_LOCAL_ONLY env"
                      : (cfg && cfg->privacy.local_only_set)
                          ? "privacy.local_only"
                          : "default (primary endpoint locality)";
    hu_log_info(
        "local_only", NULL, "local_only mode=%s (from %s; primary=%s; voice allow tts:cartesia=%s)",
        mode == HU_GATE_LIVE     ? "enforce"
        : mode == HU_GATE_SHADOW ? "audit"
                                 : "off",
        why, hu_config_primary_provider_name(cfg) ? hu_config_primary_provider_name(cfg) : "(none)",
        hu_local_only_service_allowed("tts:cartesia") ? "yes" : "no");
    return mode;
}
