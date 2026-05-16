/*
 * PWA Learner — reads PWA tab content and stores it in persistent memory.
 * Runs in the service loop when the PWA channel polls and finds new content.
 */
#include "human/pwa_learner.h"
#include "human/core/string.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Built-in app names (same order as hu_pwa_drivers_all semantics).
 * We iterate via hu_pwa_driver_resolve to support custom registry overrides. */
static const char *const PWA_LEARNER_APPS[] = {
    "slack", "discord", "whatsapp", "gmail", "calendar",
    "notion", "twitter", "telegram", "linkedin", "facebook",
};
#define PWA_LEARNER_APP_COUNT (sizeof(PWA_LEARNER_APPS) / sizeof(PWA_LEARNER_APPS[0]))

static uint32_t hash_content(const char *content, size_t len) {
    uint32_t h = 5381u;
    for (size_t i = 0; i < len; i++)
        h = ((h << 5) + h) + (unsigned char)content[i];
    return h;
}

hu_error_t hu_pwa_learner_init(hu_allocator_t *alloc, hu_pwa_learner_t *out,
                               hu_memory_t *memory) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    out->alloc = alloc;
    out->memory = memory;
    out->app_count = PWA_LEARNER_APP_COUNT;

    out->content_hashes =
        (uint32_t *)alloc->alloc(alloc->ctx, out->app_count * sizeof(uint32_t));
    if (!out->content_hashes)
        return HU_ERR_OUT_OF_MEMORY;
    memset(out->content_hashes, 0, out->app_count * sizeof(uint32_t));

#if HU_IS_TEST
    out->browser = HU_PWA_BROWSER_CHROME;
    out->browser_ok = true;
#else
    hu_error_t err = hu_pwa_detect_browser(&out->browser);
    out->browser_ok = (err == HU_OK);
#endif

    return HU_OK;
}

void hu_pwa_learner_destroy(hu_pwa_learner_t *learner) {
    if (!learner || !learner->alloc)
        return;
    if (learner->content_hashes) {
        learner->alloc->free(learner->alloc->ctx, learner->content_hashes,
                             learner->app_count * sizeof(uint32_t));
        learner->content_hashes = NULL;
    }
    learner->app_count = 0;
    learner->ingest_count = 0;
}

hu_error_t hu_pwa_learner_store(hu_pwa_learner_t *learner, const char *app_name,
                                const char *content, size_t content_len) {
    if (!learner || !app_name || !content)
        return HU_ERR_INVALID_ARGUMENT;
    if (!learner->memory || !learner->memory->vtable)
        return HU_ERR_INVALID_ARGUMENT;

    time_t now = time(NULL);
    char key_buf[128];
    int n = snprintf(key_buf, sizeof(key_buf), "pwa:%s:%ld", app_name, (long)now);
    if (n <= 0 || (size_t)n >= sizeof(key_buf))
        return HU_ERR_PARSE;

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_DAILY};
    hu_error_t err = learner->memory->vtable->store(
        learner->memory->ctx, key_buf, (size_t)n, content, content_len, &cat, NULL, 0);
    if (err != HU_OK)
        return err;

    learner->ingest_count++;
    return HU_OK;
}

hu_error_t hu_pwa_learner_scan(hu_pwa_learner_t *learner, size_t *ingested_count) {
    if (!learner || !ingested_count)
        return HU_ERR_INVALID_ARGUMENT;

    *ingested_count = 0;

#if HU_IS_TEST
    return HU_OK;
#endif

    if (!learner->browser_ok || !learner->memory || !learner->memory->vtable)
        return HU_OK;

    for (size_t i = 0; i < learner->app_count; i++) {
        const char *app_name = PWA_LEARNER_APPS[i];
        const hu_pwa_driver_t *drv = hu_pwa_driver_resolve(app_name);
        if (!drv || !drv->read_messages_js)
            continue;

        char *content = NULL;
        size_t content_len = 0;
        hu_error_t err = hu_pwa_read_messages(learner->alloc, learner->browser,
                                               app_name, &content, &content_len);
        if (err != HU_OK || !content)
            continue;

        uint32_t hash = hash_content(content, content_len);
        if (learner->content_hashes[i] != hash) {
            learner->content_hashes[i] = hash;
            err = hu_pwa_learner_store(learner, app_name, content, content_len);
            if (err == HU_OK)
                (*ingested_count)++;
        }

        learner->alloc->free(learner->alloc->ctx, content, content_len + 1);
    }

    return HU_OK;
}
