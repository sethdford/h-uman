#include "human/background_observer.h"
#include <string.h>
#include <time.h>

void hu_bg_registry_init(hu_bg_registry_t *r) {
    if (!r) {
        return;
    }
    memset(r, 0, sizeof(*r));
}

hu_error_t hu_bg_registry_register(hu_bg_registry_t *r, hu_bg_observer_t obs) {
    if (!r || !obs.vtable) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (r->count >= HU_BG_OBSERVER_MAX) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    r->observers[r->count] = obs;
    r->last_tick_epoch[r->count] = 0;
    r->count++;
    return HU_OK;
}

void hu_bg_registry_tick_all(hu_bg_registry_t *r, hu_allocator_t *alloc, hu_app_context_t *app) {
    if (!r || !alloc) {
        return;
    }
    time_t now_tt = time(NULL);
    if (now_tt < 0) {
        return;
    }
    uint64_t now = (uint64_t)now_tt;
    for (size_t i = 0; i < r->count; i++) {
        hu_bg_observer_t *o = &r->observers[i];
        const hu_bg_observer_vtable_t *vt = o->vtable;
        if (!vt || !vt->is_enabled || !vt->interval_seconds || !vt->tick) {
            continue;
        }
        if (!vt->is_enabled(o->ctx)) {
            continue;
        }
        uint32_t interval = vt->interval_seconds(o->ctx);
        uint64_t last = r->last_tick_epoch[i];
        if (interval == 0U) {
            continue;
        }
        if (now - last < (uint64_t)interval) {
            continue;
        }
        (void)vt->tick(o->ctx, alloc, app);
        r->last_tick_epoch[i] = now;
    }
}

size_t hu_bg_registry_count(const hu_bg_registry_t *r) {
    return r ? r->count : 0;
}

const hu_bg_observer_t *hu_bg_registry_get(const hu_bg_registry_t *r, size_t idx) {
    if (!r || idx >= r->count) {
        return NULL;
    }
    return &r->observers[idx];
}

void hu_bg_registry_deinit(hu_bg_registry_t *r, hu_allocator_t *alloc) {
    if (!r) {
        return;
    }
    for (size_t i = 0; i < r->count; i++) {
        hu_bg_observer_t *o = &r->observers[i];
        const hu_bg_observer_vtable_t *vt = o->vtable;
        if (vt && vt->deinit && alloc) {
            vt->deinit(o->ctx, alloc);
        }
        o->vtable = NULL;
        o->ctx = NULL;
    }
    r->count = 0;
    memset(r->last_tick_epoch, 0, sizeof(r->last_tick_epoch));
}

/* --- Built-in observers (shared context) --- */

typedef struct hu_bg_builtin_ctx {
    bool enabled;
} hu_bg_builtin_ctx_t;

static void hu_bg_builtin_deinit(void *ctx, hu_allocator_t *alloc) {
    if (!ctx || !alloc) {
        return;
    }
    alloc->free(alloc->ctx, ctx, sizeof(hu_bg_builtin_ctx_t));
}

static bool hu_bg_builtin_is_enabled(void *ctx) {
    return ((const hu_bg_builtin_ctx_t *)ctx)->enabled;
}

static void hu_bg_builtin_set_enabled(void *ctx, bool enabled) {
    ((hu_bg_builtin_ctx_t *)ctx)->enabled = enabled;
}

/* health_monitor — 60s */
static const char *hu_bg_health_name(void *ctx) {
    (void)ctx;
    return "health_monitor";
}
static const char *hu_bg_health_desc(void *ctx) {
    (void)ctx;
    return "Periodic health checks";
}
static uint32_t hu_bg_health_interval(void *ctx) {
    (void)ctx;
    return 60U;
}
static hu_error_t hu_bg_health_tick(void *ctx, hu_allocator_t *alloc, hu_app_context_t *app) {
    (void)ctx;
    (void)alloc;
    (void)app;
#if defined(HU_IS_TEST) && HU_IS_TEST
    /* No side effects in unit tests. */
#else
    /* Reserved: production health monitoring. */
#endif
    return HU_OK;
}
static const hu_bg_observer_vtable_t hu_bg_health_vtable = {
    .name = hu_bg_health_name,
    .description = hu_bg_health_desc,
    .interval_seconds = hu_bg_health_interval,
    .tick = hu_bg_health_tick,
    .is_enabled = hu_bg_builtin_is_enabled,
    .set_enabled = hu_bg_builtin_set_enabled,
    .deinit = hu_bg_builtin_deinit,
};

/* memory_consolidation — 3600s */
static const char *hu_bg_mem_name(void *ctx) {
    (void)ctx;
    return "memory_consolidation";
}
static const char *hu_bg_mem_desc(void *ctx) {
    (void)ctx;
    return "Memory consolidation pass";
}
static uint32_t hu_bg_mem_interval(void *ctx) {
    (void)ctx;
    return 3600U;
}
static hu_error_t hu_bg_mem_tick(void *ctx, hu_allocator_t *alloc, hu_app_context_t *app) {
    (void)ctx;
    (void)alloc;
    (void)app;
#if defined(HU_IS_TEST) && HU_IS_TEST
    /* No side effects in unit tests. */
#else
    /* Reserved: memory consolidation. */
#endif
    return HU_OK;
}
static const hu_bg_observer_vtable_t hu_bg_mem_vtable = {
    .name = hu_bg_mem_name,
    .description = hu_bg_mem_desc,
    .interval_seconds = hu_bg_mem_interval,
    .tick = hu_bg_mem_tick,
    .is_enabled = hu_bg_builtin_is_enabled,
    .set_enabled = hu_bg_builtin_set_enabled,
    .deinit = hu_bg_builtin_deinit,
};

/* security_audit — 86400s */
static const char *hu_bg_sec_name(void *ctx) {
    (void)ctx;
    return "security_audit";
}
static const char *hu_bg_sec_desc(void *ctx) {
    (void)ctx;
    return "Security audit sweep";
}
static uint32_t hu_bg_sec_interval(void *ctx) {
    (void)ctx;
    return 86400U;
}
static hu_error_t hu_bg_sec_tick(void *ctx, hu_allocator_t *alloc, hu_app_context_t *app) {
    (void)ctx;
    (void)alloc;
    (void)app;
#if defined(HU_IS_TEST) && HU_IS_TEST
    /* No side effects in unit tests. */
#else
    /* Reserved: security audit. */
#endif
    return HU_OK;
}
static const hu_bg_observer_vtable_t hu_bg_sec_vtable = {
    .name = hu_bg_sec_name,
    .description = hu_bg_sec_desc,
    .interval_seconds = hu_bg_sec_interval,
    .tick = hu_bg_sec_tick,
    .is_enabled = hu_bg_builtin_is_enabled,
    .set_enabled = hu_bg_builtin_set_enabled,
    .deinit = hu_bg_builtin_deinit,
};

/* feed_processor — 1800s */
static const char *hu_bg_feed_name(void *ctx) {
    (void)ctx;
    return "feed_processor";
}
static const char *hu_bg_feed_desc(void *ctx) {
    (void)ctx;
    return "Feed processing cycle";
}
static uint32_t hu_bg_feed_interval(void *ctx) {
    (void)ctx;
    return 1800U;
}
static hu_error_t hu_bg_feed_tick(void *ctx, hu_allocator_t *alloc, hu_app_context_t *app) {
    (void)ctx;
    (void)alloc;
    (void)app;
#if defined(HU_IS_TEST) && HU_IS_TEST
    /* No side effects in unit tests. */
#else
    /* Reserved: feed processing. */
#endif
    return HU_OK;
}
static const hu_bg_observer_vtable_t hu_bg_feed_vtable = {
    .name = hu_bg_feed_name,
    .description = hu_bg_feed_desc,
    .interval_seconds = hu_bg_feed_interval,
    .tick = hu_bg_feed_tick,
    .is_enabled = hu_bg_builtin_is_enabled,
    .set_enabled = hu_bg_builtin_set_enabled,
    .deinit = hu_bg_builtin_deinit,
};

/* dpo_export — 604800s */
static const char *hu_bg_dpo_name(void *ctx) {
    (void)ctx;
    return "dpo_export";
}
static const char *hu_bg_dpo_desc(void *ctx) {
    (void)ctx;
    return "DPO training export";
}
static uint32_t hu_bg_dpo_interval(void *ctx) {
    (void)ctx;
    return 604800U;
}
static hu_error_t hu_bg_dpo_tick(void *ctx, hu_allocator_t *alloc, hu_app_context_t *app) {
    (void)ctx;
    (void)alloc;
    (void)app;
#if defined(HU_IS_TEST) && HU_IS_TEST
    /* No side effects in unit tests. */
#else
    /* Reserved: DPO export. */
#endif
    return HU_OK;
}
static const hu_bg_observer_vtable_t hu_bg_dpo_vtable = {
    .name = hu_bg_dpo_name,
    .description = hu_bg_dpo_desc,
    .interval_seconds = hu_bg_dpo_interval,
    .tick = hu_bg_dpo_tick,
    .is_enabled = hu_bg_builtin_is_enabled,
    .set_enabled = hu_bg_builtin_set_enabled,
    .deinit = hu_bg_builtin_deinit,
};

/* intelligence_cycle — 7200s */
static const char *hu_bg_intel_name(void *ctx) {
    (void)ctx;
    return "intelligence_cycle";
}
static const char *hu_bg_intel_desc(void *ctx) {
    (void)ctx;
    return "Intelligence cycle";
}
static uint32_t hu_bg_intel_interval(void *ctx) {
    (void)ctx;
    return 7200U;
}
static hu_error_t hu_bg_intel_tick(void *ctx, hu_allocator_t *alloc, hu_app_context_t *app) {
    (void)ctx;
    (void)alloc;
    (void)app;
#if defined(HU_IS_TEST) && HU_IS_TEST
    /* No side effects in unit tests. */
#else
    /* Reserved: intelligence cycle. */
#endif
    return HU_OK;
}
static const hu_bg_observer_vtable_t hu_bg_intel_vtable = {
    .name = hu_bg_intel_name,
    .description = hu_bg_intel_desc,
    .interval_seconds = hu_bg_intel_interval,
    .tick = hu_bg_intel_tick,
    .is_enabled = hu_bg_builtin_is_enabled,
    .set_enabled = hu_bg_builtin_set_enabled,
    .deinit = hu_bg_builtin_deinit,
};

/* email_digest — 86400s */
static const char *hu_bg_email_name(void *ctx) {
    (void)ctx;
    return "email_digest";
}
static const char *hu_bg_email_desc(void *ctx) {
    (void)ctx;
    return "Email digest";
}
static uint32_t hu_bg_email_interval(void *ctx) {
    (void)ctx;
    return 86400U;
}
static hu_error_t hu_bg_email_tick(void *ctx, hu_allocator_t *alloc, hu_app_context_t *app) {
    (void)ctx;
    (void)alloc;
    (void)app;
#if defined(HU_IS_TEST) && HU_IS_TEST
    /* No side effects in unit tests. */
#else
    /* Reserved: email digest. */
#endif
    return HU_OK;
}
static const hu_bg_observer_vtable_t hu_bg_email_vtable = {
    .name = hu_bg_email_name,
    .description = hu_bg_email_desc,
    .interval_seconds = hu_bg_email_interval,
    .tick = hu_bg_email_tick,
    .is_enabled = hu_bg_builtin_is_enabled,
    .set_enabled = hu_bg_builtin_set_enabled,
    .deinit = hu_bg_builtin_deinit,
};

static hu_error_t hu_bg_builtin_create(hu_allocator_t *alloc, const hu_bg_observer_vtable_t *vt,
                                       hu_bg_observer_t *out) {
    if (!alloc || !vt || !out) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    hu_bg_builtin_ctx_t *c = alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c) {
        return HU_ERR_OUT_OF_MEMORY;
    }
    c->enabled = true;
    out->ctx = c;
    out->vtable = vt;
    return HU_OK;
}

hu_error_t hu_bg_health_monitor_create(hu_allocator_t *alloc, hu_bg_observer_t *out) {
    return hu_bg_builtin_create(alloc, &hu_bg_health_vtable, out);
}

hu_error_t hu_bg_memory_consolidation_create(hu_allocator_t *alloc, hu_bg_observer_t *out) {
    return hu_bg_builtin_create(alloc, &hu_bg_mem_vtable, out);
}

hu_error_t hu_bg_security_audit_create(hu_allocator_t *alloc, hu_bg_observer_t *out) {
    return hu_bg_builtin_create(alloc, &hu_bg_sec_vtable, out);
}

hu_error_t hu_bg_feed_processor_create(hu_allocator_t *alloc, hu_bg_observer_t *out) {
    return hu_bg_builtin_create(alloc, &hu_bg_feed_vtable, out);
}

hu_error_t hu_bg_dpo_export_create(hu_allocator_t *alloc, hu_bg_observer_t *out) {
    return hu_bg_builtin_create(alloc, &hu_bg_dpo_vtable, out);
}

hu_error_t hu_bg_intelligence_cycle_create(hu_allocator_t *alloc, hu_bg_observer_t *out) {
    return hu_bg_builtin_create(alloc, &hu_bg_intel_vtable, out);
}

hu_error_t hu_bg_email_digest_create(hu_allocator_t *alloc, hu_bg_observer_t *out) {
    return hu_bg_builtin_create(alloc, &hu_bg_email_vtable, out);
}
