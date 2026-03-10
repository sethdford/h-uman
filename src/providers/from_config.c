/* Create provider from config (handles composite providers like "reliable", "router"). */
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/providers/factory.h"
#include "human/providers/reliable.h"
#include "human/providers/router.h"
#include <string.h>

static hu_error_t create_provider_from_name(hu_allocator_t *alloc, const hu_config_t *cfg,
                                            const char *prov_name, hu_provider_t *out) {
    if (!prov_name || !prov_name[0])
        return HU_ERR_INVALID_ARGUMENT;
    size_t len = strlen(prov_name);
    const char *api_key = hu_config_get_provider_key(cfg, prov_name);
    size_t api_key_len = api_key ? strlen(api_key) : 0;
    const char *base_url = hu_config_get_provider_base_url(cfg, prov_name);
    size_t base_url_len = base_url ? strlen(base_url) : 0;
    if (!base_url || base_url_len == 0) {
        base_url = hu_compatible_provider_url(prov_name);
        base_url_len = base_url ? strlen(base_url) : 0;
    }
    return hu_provider_create(alloc, prov_name, len, api_key, api_key_len, base_url, base_url_len,
                              out);
}

hu_error_t hu_provider_create_from_config(hu_allocator_t *alloc, const hu_config_t *cfg,
                                          const char *name, size_t name_len, hu_provider_t *out) {
    if (!alloc || !cfg || !name || name_len == 0 || !out)
        return HU_ERR_INVALID_ARGUMENT;

    if (name_len == 6 && memcmp(name, "router", 6) == 0) {
        const char *standard_name =
            cfg->router.standard && cfg->router.standard[0] ? cfg->router.standard : "openai";
        hu_provider_t standard = {0};
        hu_error_t err = create_provider_from_name(alloc, cfg, standard_name, &standard);
        if (err != HU_OK)
            return err;

        hu_provider_t fast = {0};
        hu_provider_t powerful = {0};
        bool has_fast = (cfg->router.fast && cfg->router.fast[0]);
        bool has_powerful = (cfg->router.powerful && cfg->router.powerful[0]);

        if (has_fast) {
            err = create_provider_from_name(alloc, cfg, cfg->router.fast, &fast);
            if (err != HU_OK) {
                if (standard.vtable && standard.vtable->deinit)
                    standard.vtable->deinit(standard.ctx, alloc);
                return err;
            }
        }
        if (has_powerful) {
            err = create_provider_from_name(alloc, cfg, cfg->router.powerful, &powerful);
            if (err != HU_OK) {
                if (standard.vtable && standard.vtable->deinit)
                    standard.vtable->deinit(standard.ctx, alloc);
                if (has_fast && fast.vtable && fast.vtable->deinit)
                    fast.vtable->deinit(fast.ctx, alloc);
                return err;
            }
        }

        hu_multi_model_router_config_t rcfg = {
            .fast = fast,
            .standard = standard,
            .powerful = powerful,
            .complexity_threshold_low =
                cfg->router.complexity_low > 0 ? cfg->router.complexity_low : 50,
            .complexity_threshold_high =
                cfg->router.complexity_high > 0 ? cfg->router.complexity_high : 500,
        };
        err = hu_multi_model_router_create(alloc, &rcfg, out);
        if (err != HU_OK) {
            if (standard.vtable && standard.vtable->deinit)
                standard.vtable->deinit(standard.ctx, alloc);
            if (has_fast && fast.vtable && fast.vtable->deinit)
                fast.vtable->deinit(fast.ctx, alloc);
            if (has_powerful && powerful.vtable && powerful.vtable->deinit)
                powerful.vtable->deinit(powerful.ctx, alloc);
            return err;
        }
        return HU_OK;
    }

    if (name_len == 8 && memcmp(name, "reliable", 8) == 0) {
        const char *primary_name =
            cfg->reliability.primary_provider && cfg->reliability.primary_provider[0]
                ? cfg->reliability.primary_provider
                : "openai";
        size_t primary_len = strlen(primary_name);
        const char *api_key = hu_config_get_provider_key(cfg, primary_name);
        size_t api_key_len = api_key ? strlen(api_key) : 0;
        const char *base_url = hu_config_get_provider_base_url(cfg, primary_name);
        size_t base_url_len = base_url ? strlen(base_url) : 0;
        if (!base_url || base_url_len == 0) {
            base_url = hu_compatible_provider_url(primary_name);
            base_url_len = base_url ? strlen(base_url) : 0;
        }

        hu_provider_t primary;
        hu_error_t err = hu_provider_create(alloc, primary_name, primary_len, api_key, api_key_len,
                                            base_url, base_url_len, &primary);
        if (err != HU_OK)
            return err;

        hu_provider_t fallback = {0};
        if (cfg->reliability.fallback_providers_len > 0 && cfg->reliability.fallback_providers[0] &&
            cfg->reliability.fallback_providers[0][0]) {
            const char *fb_name = cfg->reliability.fallback_providers[0];
            size_t fb_len = strlen(fb_name);
            const char *fb_key = hu_config_get_provider_key(cfg, fb_name);
            size_t fb_key_len = fb_key ? strlen(fb_key) : 0;
            const char *fb_url = hu_config_get_provider_base_url(cfg, fb_name);
            size_t fb_url_len = fb_url ? strlen(fb_url) : 0;
            if (!fb_url || fb_url_len == 0) {
                fb_url = hu_compatible_provider_url(fb_name);
                fb_url_len = fb_url ? strlen(fb_url) : 0;
            }
            err = hu_provider_create(alloc, fb_name, fb_len, fb_key, fb_key_len, fb_url, fb_url_len,
                                     &fallback);
            if (err != HU_OK) {
                if (primary.vtable && primary.vtable->deinit)
                    primary.vtable->deinit(primary.ctx, alloc);
                return err;
            }
        }

        hu_reliable_config_t rcfg = {
            .primary = primary,
            .fallback = fallback,
            .max_retries =
                cfg->reliability.provider_retries > 0 ? (int)cfg->reliability.provider_retries : 3,
            .base_delay_ms = cfg->reliability.provider_backoff_ms > 0
                                 ? (int)cfg->reliability.provider_backoff_ms
                                 : 1000,
            .max_delay_ms = 30000,
            .failure_threshold = 5,
            .recovery_timeout_seconds = 60,
        };

        err = hu_reliable_provider_create(alloc, &rcfg, out);
        if (err != HU_OK) {
            if (primary.vtable && primary.vtable->deinit)
                primary.vtable->deinit(primary.ctx, alloc);
            if (fallback.vtable && fallback.vtable->deinit)
                fallback.vtable->deinit(fallback.ctx, alloc);
            return err;
        }
        return HU_OK;
    }

    return HU_ERR_NOT_SUPPORTED;
}
