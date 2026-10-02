#include "human/providers/local_only_config.h"
#include "human/core/local_only_guard.h"
#include "human/core/log.h"
#include "human/provider.h"

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

bool hu_config_primary_is_local(const hu_config_t *cfg) {
    const char *url = hu_config_primary_endpoint(cfg);
    return hu_provider_endpoint_is_local(url, url ? strlen(url) : 0);
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
    const char *why = hu_local_only_env_parse(getenv("HU_LOCAL_ONLY")) >= 0 ? "HU_LOCAL_ONLY env"
                      : (cfg && cfg->privacy.local_only_set)
                          ? "privacy.local_only"
                          : "default (primary endpoint locality)";
    hu_log_info("local_only", NULL, "local_only mode=%s (from %s; primary=%s)",
                mode == HU_GATE_LIVE     ? "enforce"
                : mode == HU_GATE_SHADOW ? "audit"
                                         : "off",
                why,
                hu_config_primary_provider_name(cfg) ? hu_config_primary_provider_name(cfg)
                                                     : "(none)");
    return mode;
}
