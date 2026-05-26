/* Create provider from config (handles composite providers like "reliable", "router"). */
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/providers/api_key.h"
#include "human/providers/ensemble.h"
#include "human/providers/factory.h"
#include "human/providers/reliable.h"
#include "human/providers/router.h"
#include <string.h>

/* Resolve API key: config.json → env var (ANTHROPIC_API_KEY, etc.) → NULL */
static char *resolve_key(hu_allocator_t *alloc, const hu_config_t *cfg, const char *prov_name) {
    const char *cfg_key = hu_config_get_provider_key(cfg, prov_name);
    char *key = hu_api_key_resolve(alloc, prov_name, strlen(prov_name), cfg_key,
                                   cfg_key ? strlen(cfg_key) : 0);
    if (!key) {
        hu_log_error("provider", NULL,
                     "No API key for '%s'. Set the corresponding env var or add to config.json",
                     prov_name);
    }
    return key;
}

/* 2026-05 audit follow-up — translate parsed cfg->reliability.model_fallbacks
 * into the reliable provider's struct shape. Used by both the explicit
 * "reliable" default_provider branch and the auto-wrap path so neither
 * silently drops the operator's model-substitution config.
 *
 * Returns NULL entries + count=0 when no model_fallbacks are configured;
 * the caller still passes those into hu_reliable_create_ex which is a
 * no-op for zero entries. Model name strings are BORROWED from cfg
 * (cfg outlives the provider per the existing primary_provider pattern). */
static hu_error_t build_model_fallback_chain(hu_allocator_t *alloc, const hu_config_t *cfg,
                                             hu_reliable_model_fallback_entry_t **out_entries,
                                             hu_reliable_fallback_model_t ***out_inner_arrays,
                                             size_t *out_count, size_t *out_alloc_size) {
    *out_entries = NULL;
    *out_inner_arrays = NULL;
    *out_count = 0;
    *out_alloc_size = 0;
    size_t n = cfg->reliability.model_fallbacks_len;
    if (n == 0)
        return HU_OK;

    size_t alloc_size = n * sizeof(hu_reliable_model_fallback_entry_t);
    hu_reliable_model_fallback_entry_t *entries =
        (hu_reliable_model_fallback_entry_t *)alloc->alloc(alloc->ctx, alloc_size);
    hu_reliable_fallback_model_t **inners = (hu_reliable_fallback_model_t **)alloc->alloc(
        alloc->ctx, n * sizeof(hu_reliable_fallback_model_t *));
    if (!entries || !inners) {
        if (entries)
            alloc->free(alloc->ctx, entries, alloc_size);
        if (inners)
            alloc->free(alloc->ctx, inners, n * sizeof(hu_reliable_fallback_model_t *));
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(entries, 0, alloc_size);
    memset(inners, 0, n * sizeof(hu_reliable_fallback_model_t *));

    size_t valid = 0;
    for (size_t i = 0; i < n; i++) {
        const hu_config_model_fallback_t *src = &cfg->reliability.model_fallbacks[i];
        if (!src->model || src->fallback_models_len == 0)
            continue;
        hu_reliable_fallback_model_t *inner = (hu_reliable_fallback_model_t *)alloc->alloc(
            alloc->ctx, src->fallback_models_len * sizeof(hu_reliable_fallback_model_t));
        if (!inner)
            continue; /* skip this entry rather than failing the whole chain */
        for (size_t j = 0; j < src->fallback_models_len; j++) {
            inner[j].model = src->fallback_models[j];
            inner[j].model_len = src->fallback_models[j] ? strlen(src->fallback_models[j]) : 0;
        }
        inners[valid] = inner;
        entries[valid].model = src->model;
        entries[valid].model_len = strlen(src->model);
        entries[valid].fallbacks = inner;
        entries[valid].fallbacks_count = src->fallback_models_len;
        valid++;
    }
    *out_entries = entries;
    *out_inner_arrays = inners;
    *out_count = valid;
    *out_alloc_size = alloc_size;
    return HU_OK;
}

/* Sibling of build_model_fallback_chain — frees BOTH the inner per-entry
 * arrays AND the outer arrays. Safe to call with all-NULL inputs. */
static void free_model_fallback_chain(hu_allocator_t *alloc, const hu_config_t *cfg,
                                      hu_reliable_model_fallback_entry_t *entries,
                                      hu_reliable_fallback_model_t **inners, size_t alloc_size) {
    if (inners) {
        for (size_t i = 0; i < cfg->reliability.model_fallbacks_len; i++) {
            if (inners[i]) {
                const hu_config_model_fallback_t *src = &cfg->reliability.model_fallbacks[i];
                alloc->free(alloc->ctx, inners[i],
                            src->fallback_models_len * sizeof(hu_reliable_fallback_model_t));
            }
        }
        alloc->free(alloc->ctx, inners,
                    cfg->reliability.model_fallbacks_len * sizeof(hu_reliable_fallback_model_t *));
    }
    if (entries && alloc_size > 0)
        alloc->free(alloc->ctx, entries, alloc_size);
}

static hu_error_t create_provider_from_name(hu_allocator_t *alloc, const hu_config_t *cfg,
                                            const char *prov_name, hu_provider_t *out) {
    if (!prov_name || !prov_name[0])
        return HU_ERR_INVALID_ARGUMENT;
    size_t len = strlen(prov_name);
    char *api_key = resolve_key(alloc, cfg, prov_name);
    size_t api_key_len = api_key ? strlen(api_key) : 0;
    const char *base_url = hu_config_get_provider_base_url(cfg, prov_name);
    size_t base_url_len = base_url ? strlen(base_url) : 0;
    if (!base_url || base_url_len == 0) {
        base_url = hu_compatible_provider_url(prov_name);
        base_url_len = base_url ? strlen(base_url) : 0;
    }
    hu_error_t err = hu_provider_create(alloc, prov_name, len, api_key, api_key_len, base_url,
                                        base_url_len, out);
    /* `hu_provider_create` takes a `const char *` api_key — every provider
     * deep-copies it into its own context (see openai.c:1220-1226 et al).
     * The heap copy allocated by `hu_api_key_resolve` is the caller's to
     * free. PR55's ASan run attributed seven separate 8-20 byte leaks
     * here, one per provider in the test matrix. Free on both success
     * and failure paths — providers don't retain ownership on error. */
    if (api_key)
        alloc->free(alloc->ctx, api_key, api_key_len + 1);
    return err;
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
        char *api_key = resolve_key(alloc, cfg, primary_name);
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
        /* Mirror the create_provider_from_name fix above: every provider
         * deep-copies api_key into its own context, so the heap copy from
         * resolve_key is ours to release on both success and failure paths.
         * This branch was missed in 8cff4a99 — PR55's ASan run still
         * attributed an 8 b leak to api_key.c:60 here. */
        if (api_key)
            alloc->free(alloc->ctx, api_key, api_key_len + 1);
        if (err != HU_OK)
            return err;

        /* Build the full extras chain from cfg->reliability.fallback_providers[].
         * Previously only fallback_providers[0] was honoured, which silently
         * dropped users' carefully configured 2nd / 3rd fallbacks. */
        size_t extras_capacity = cfg->reliability.fallback_providers_len;
        hu_reliable_provider_entry_t *extras = NULL;
        size_t extras_count = 0;
        size_t extras_alloc_size = 0;
        if (extras_capacity > 0) {
            extras_alloc_size = extras_capacity * sizeof(*extras);
            extras = (hu_reliable_provider_entry_t *)alloc->alloc(alloc->ctx, extras_alloc_size);
            if (!extras) {
                if (primary.vtable && primary.vtable->deinit)
                    primary.vtable->deinit(primary.ctx, alloc);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memset(extras, 0, extras_alloc_size);
            for (size_t i = 0; i < extras_capacity; i++) {
                const char *fb_name = cfg->reliability.fallback_providers[i];
                if (!fb_name || !fb_name[0])
                    continue;
                hu_provider_t fb = {0};
                if (create_provider_from_name(alloc, cfg, fb_name, &fb) != HU_OK) {
                    hu_log_warn("provider", NULL, "reliable: skipping unconfigured fallback '%s'",
                                fb_name);
                    continue;
                }
                extras[extras_count].name = fb_name;
                extras[extras_count].name_len = strlen(fb_name);
                extras[extras_count].provider = fb;
                extras_count++;
            }
        }

        uint32_t max_retries =
            cfg->reliability.provider_retries > 0 ? cfg->reliability.provider_retries : 3;
        uint64_t backoff_ms =
            cfg->reliability.provider_backoff_ms > 0 ? cfg->reliability.provider_backoff_ms : 1000;

        /* 2026-05 audit follow-up — thread cfg->reliability.model_fallbacks
         * through so the cloud-fallback path sees the right model name. */
        hu_reliable_model_fallback_entry_t *mf_entries = NULL;
        hu_reliable_fallback_model_t **mf_inners = NULL;
        size_t mf_count = 0;
        size_t mf_alloc = 0;
        hu_error_t mferr =
            build_model_fallback_chain(alloc, cfg, &mf_entries, &mf_inners, &mf_count, &mf_alloc);
        if (mferr != HU_OK) {
            if (primary.vtable && primary.vtable->deinit)
                primary.vtable->deinit(primary.ctx, alloc);
            for (size_t i = 0; i < extras_count; i++) {
                if (extras[i].provider.vtable && extras[i].provider.vtable->deinit)
                    extras[i].provider.vtable->deinit(extras[i].provider.ctx, alloc);
            }
            if (extras)
                alloc->free(alloc->ctx, extras, extras_alloc_size);
            return mferr;
        }
        if (mf_count > 0) {
            hu_log_info("provider", NULL, "reliable: wired %zu model_fallback chain(s)", mf_count);
        }

        err = hu_reliable_create_ex(alloc, primary, max_retries, backoff_ms, extras, extras_count,
                                    mf_entries, mf_count, out);
        if (err != HU_OK) {
            if (primary.vtable && primary.vtable->deinit)
                primary.vtable->deinit(primary.ctx, alloc);
            for (size_t i = 0; i < extras_count; i++) {
                if (extras[i].provider.vtable && extras[i].provider.vtable->deinit)
                    extras[i].provider.vtable->deinit(extras[i].provider.ctx, alloc);
            }
            if (extras)
                alloc->free(alloc->ctx, extras, extras_alloc_size);
            free_model_fallback_chain(alloc, cfg, mf_entries, mf_inners, mf_alloc);
            return err;
        }
        if (extras)
            alloc->free(alloc->ctx, extras, extras_alloc_size);
        free_model_fallback_chain(alloc, cfg, mf_entries, mf_inners, mf_alloc);
        return HU_OK;
    }

    if (name_len == 8 && memcmp(name, "ensemble", 8) == 0) {
        const char *names_buf[HU_ENSEMBLE_MAX_PROVIDERS];
        size_t name_count = 0;

        if (cfg->ensemble.providers_len > 0) {
            for (size_t i = 0;
                 i < cfg->ensemble.providers_len && name_count < HU_ENSEMBLE_MAX_PROVIDERS; i++) {
                const char *pname = cfg->ensemble.providers[i];
                if (pname && pname[0])
                    names_buf[name_count++] = pname;
            }
        } else {
            if (cfg->default_provider && cfg->default_provider[0])
                names_buf[name_count++] = cfg->default_provider;
            if (cfg->reliability.fallback_providers_len > 0 &&
                cfg->reliability.fallback_providers[0] &&
                cfg->reliability.fallback_providers[0][0] && name_count < HU_ENSEMBLE_MAX_PROVIDERS)
                names_buf[name_count++] = cfg->reliability.fallback_providers[0];
        }

        hu_ensemble_spec_t ecfg = {0};
        hu_error_t err = HU_OK;
        for (size_t i = 0; i < name_count; i++) {
            err = create_provider_from_name(alloc, cfg, names_buf[i],
                                            &ecfg.providers[ecfg.provider_count]);
            if (err == HU_OK)
                ecfg.provider_count++;
        }

        if (ecfg.provider_count == 0)
            return HU_ERR_INVALID_ARGUMENT;

        const char *strat = cfg->ensemble.strategy;
        if (strat && strcmp(strat, "best_for_task") == 0)
            ecfg.strategy = HU_ENSEMBLE_BEST_FOR_TASK;
        else if (strat && strcmp(strat, "consensus") == 0)
            ecfg.strategy = HU_ENSEMBLE_CONSENSUS;
        else
            ecfg.strategy = HU_ENSEMBLE_ROUND_ROBIN;

        err = hu_ensemble_create(alloc, &ecfg, out);
        if (err != HU_OK) {
            for (size_t i = 0; i < ecfg.provider_count; i++) {
                if (ecfg.providers[i].vtable && ecfg.providers[i].vtable->deinit)
                    ecfg.providers[i].vtable->deinit(ecfg.providers[i].ctx, alloc);
            }
        }
        return err;
    }

    /* Plain providers (openai, gemini, groq, …): delegate to factory via config keys/URLs. */
    {
        char nbuf[160];
        if (name_len >= sizeof(nbuf))
            return HU_ERR_INVALID_ARGUMENT;
        memcpy(nbuf, name, name_len);
        nbuf[name_len] = '\0';
        return create_provider_from_name(alloc, cfg, nbuf, out);
    }
}

/* Composite provider names handle their own internal fallback / routing logic,
 * so we never auto-wrap them. */
static bool default_is_composite(const char *name) {
    if (!name || !name[0])
        return false;
    return (strcmp(name, "router") == 0) || (strcmp(name, "ensemble") == 0) ||
           (strcmp(name, "reliable") == 0);
}

hu_error_t hu_provider_create_default(hu_allocator_t *alloc, const hu_config_t *cfg,
                                      hu_provider_t *out) {
    if (!alloc || !cfg || !out)
        return HU_ERR_INVALID_ARGUMENT;

    const char *prov_name = cfg->default_provider ? cfg->default_provider : "openai";
    size_t prov_name_len = strlen(prov_name);

    hu_provider_t base = {0};
    hu_error_t err = hu_provider_create_from_config(alloc, cfg, prov_name, prov_name_len, &base);
    if (err != HU_OK)
        return err;

    /* No fallbacks configured, or default is itself a composite that already
     * encodes a chain — return the base provider as-is. */
    bool fallbacks_configured = false;
    for (size_t i = 0; i < cfg->reliability.fallback_providers_len; i++) {
        const char *n = cfg->reliability.fallback_providers[i];
        if (n && n[0]) {
            fallbacks_configured = true;
            break;
        }
    }
    if (!fallbacks_configured || default_is_composite(prov_name)) {
        *out = base;
        return HU_OK;
    }

    /* Build a hu_reliable_create_ex chain: base + each configured fallback.
     * The wrapper takes ownership of base on success. */
    size_t extras_capacity = cfg->reliability.fallback_providers_len;
    size_t extras_alloc_size = extras_capacity * sizeof(hu_reliable_provider_entry_t);
    hu_reliable_provider_entry_t *extras =
        (hu_reliable_provider_entry_t *)alloc->alloc(alloc->ctx, extras_alloc_size);
    if (!extras) {
        if (base.vtable && base.vtable->deinit)
            base.vtable->deinit(base.ctx, alloc);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(extras, 0, extras_alloc_size);

    size_t extras_count = 0;
    for (size_t i = 0; i < extras_capacity; i++) {
        const char *fb_name = cfg->reliability.fallback_providers[i];
        if (!fb_name || !fb_name[0])
            continue;
        /* Skip a fallback that is the SAME as the primary — that would just
         * be duplicate work and could confuse the breaker accounting. */
        if (strcmp(fb_name, prov_name) == 0)
            continue;
        hu_provider_t fb = {0};
        if (create_provider_from_name(alloc, cfg, fb_name, &fb) != HU_OK) {
            hu_log_warn("provider", NULL,
                        "default_provider auto-wrap: skipping unconfigured fallback '%s'", fb_name);
            continue;
        }
        extras[extras_count].name = fb_name;
        extras[extras_count].name_len = strlen(fb_name);
        extras[extras_count].provider = fb;
        extras_count++;
    }

    if (extras_count == 0) {
        /* All fallbacks were unusable — return the base unwrapped rather than
         * a degenerate single-provider "reliable" wrapper. */
        alloc->free(alloc->ctx, extras, extras_alloc_size);
        *out = base;
        return HU_OK;
    }

    uint32_t max_retries =
        cfg->reliability.provider_retries > 0 ? cfg->reliability.provider_retries : 2;
    uint64_t backoff_ms =
        cfg->reliability.provider_backoff_ms > 0 ? cfg->reliability.provider_backoff_ms : 500;

    hu_log_info("provider", NULL,
                "default_provider '%s' auto-wrapped with %zu fallback(s) for self-healing",
                prov_name, extras_count);

    /* 2026-05 audit follow-up — thread parsed model_fallbacks (already
     * translated by the shared helper) into hu_reliable_create_ex so the
     * auto-wrap path doesn't silently drop the operator's chain. */
    hu_reliable_model_fallback_entry_t *mf_entries = NULL;
    hu_reliable_fallback_model_t **mf_inners = NULL;
    size_t mf_count = 0;
    size_t mf_alloc = 0;
    hu_error_t mferr =
        build_model_fallback_chain(alloc, cfg, &mf_entries, &mf_inners, &mf_count, &mf_alloc);
    if (mferr != HU_OK) {
        if (base.vtable && base.vtable->deinit)
            base.vtable->deinit(base.ctx, alloc);
        for (size_t i = 0; i < extras_count; i++) {
            if (extras[i].provider.vtable && extras[i].provider.vtable->deinit)
                extras[i].provider.vtable->deinit(extras[i].provider.ctx, alloc);
        }
        alloc->free(alloc->ctx, extras, extras_alloc_size);
        return mferr;
    }
    if (mf_count > 0) {
        hu_log_info("provider", NULL, "default_provider '%s' wired %zu model_fallback chain(s)",
                    prov_name, mf_count);
    }

    err = hu_reliable_create_ex(alloc, base, max_retries, backoff_ms, extras, extras_count,
                                mf_entries, mf_count, out);
    if (err != HU_OK) {
        if (base.vtable && base.vtable->deinit)
            base.vtable->deinit(base.ctx, alloc);
        for (size_t i = 0; i < extras_count; i++) {
            if (extras[i].provider.vtable && extras[i].provider.vtable->deinit)
                extras[i].provider.vtable->deinit(extras[i].provider.ctx, alloc);
        }
    }
    alloc->free(alloc->ctx, extras, extras_alloc_size);
    free_model_fallback_chain(alloc, cfg, mf_entries, mf_inners, mf_alloc);
    return err;
}
