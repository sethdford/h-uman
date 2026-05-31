#include "human/config.h"
#include "human/core/arena.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include <string.h>

#include "config_internal.h"

void hu_config_deinit(hu_config_t *cfg) {
    if (!cfg)
        return;
    hu_allocator_t *a = &cfg->allocator;
    for (size_t i = 0; i < HU_ENSEMBLE_CONFIG_PROVIDER_NAMES_MAX; i++) {
        if (cfg->ensemble.providers[i]) {
            a->free(a->ctx, cfg->ensemble.providers[i], strlen(cfg->ensemble.providers[i]) + 1);
            cfg->ensemble.providers[i] = NULL;
        }
    }
    cfg->ensemble.providers_len = 0;
    if (cfg->ensemble.strategy) {
        a->free(a->ctx, cfg->ensemble.strategy, strlen(cfg->ensemble.strategy) + 1);
        cfg->ensemble.strategy = NULL;
    }
    /* 2026-05 audit follow-up — free the parsed model_fallbacks chain.
     * Allocated via the system allocator in config_parse.c (matching the
     * fallback_providers pattern), so explicit free is required even
     * though the arena gets bulk-destroyed below. */
    if (cfg->reliability.model_fallbacks) {
        for (size_t i = 0; i < cfg->reliability.model_fallbacks_len; i++) {
            hu_config_model_fallback_t *e = &cfg->reliability.model_fallbacks[i];
            if (e->model)
                a->free(a->ctx, e->model, strlen(e->model) + 1);
            if (e->fallback_models) {
                for (size_t j = 0; j < e->fallback_models_len; j++)
                    if (e->fallback_models[j])
                        a->free(a->ctx, e->fallback_models[j], strlen(e->fallback_models[j]) + 1);
                a->free(a->ctx, e->fallback_models, e->fallback_models_len * sizeof(char *));
            }
        }
        a->free(a->ctx, cfg->reliability.model_fallbacks,
                cfg->reliability.model_fallbacks_len * sizeof(hu_config_model_fallback_t));
        cfg->reliability.model_fallbacks = NULL;
        cfg->reliability.model_fallbacks_len = 0;
    }

    if (cfg->arena) {
        /* Arena holds most config strings (e.g. cfg->voice.* including mode, realtime_model,
         * realtime_voice); bulk-freed here. */
        hu_arena_destroy(cfg->arena);
        cfg->arena = NULL;
    }
    memset(cfg, 0, sizeof(*cfg));
}

/* parse_* and hu_config_parse_json moved to config_parse.c */
/* hu_config_load, set_defaults, sync_*, load_json_file, hu_config_apply_env_* moved to
 * config_merge.c */
/* hu_config_save moved to config_serialize.c */
/* hu_config_get_*, hu_config_validate, hu_config_provider_requires_api_key moved to
 * config_getters.c */
