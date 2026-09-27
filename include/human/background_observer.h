#ifndef HU_BACKGROUND_OBSERVER_H
#define HU_BACKGROUND_OBSERVER_H

#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
typedef struct hu_app_context hu_app_context_t;

typedef struct hu_bg_observer_vtable {
    const char *(*name)(void *ctx);
    const char *(*description)(void *ctx);
    uint32_t (*interval_seconds)(void *ctx);
    hu_error_t (*tick)(void *ctx, hu_allocator_t *alloc, hu_app_context_t *app);
    bool (*is_enabled)(void *ctx);
    void (*set_enabled)(void *ctx, bool enabled);
    void (*deinit)(void *ctx, hu_allocator_t *alloc);
} hu_bg_observer_vtable_t;

typedef struct hu_bg_observer {
    const hu_bg_observer_vtable_t *vtable;
    void *ctx;
} hu_bg_observer_t;

#define HU_BG_OBSERVER_MAX 32

typedef struct hu_bg_registry {
    hu_bg_observer_t observers[HU_BG_OBSERVER_MAX];
    uint64_t last_tick_epoch[HU_BG_OBSERVER_MAX];
    size_t count;
} hu_bg_registry_t;

void hu_bg_registry_init(hu_bg_registry_t *r);
hu_error_t hu_bg_registry_register(hu_bg_registry_t *r, hu_bg_observer_t obs);
void hu_bg_registry_tick_all(hu_bg_registry_t *r, hu_allocator_t *alloc, hu_app_context_t *app);
size_t hu_bg_registry_count(const hu_bg_registry_t *r);
const hu_bg_observer_t *hu_bg_registry_get(const hu_bg_registry_t *r, size_t idx);
void hu_bg_registry_deinit(hu_bg_registry_t *r, hu_allocator_t *alloc);

/* Built-in observer constructors */
hu_error_t hu_bg_health_monitor_create(hu_allocator_t *alloc, hu_bg_observer_t *out);
hu_error_t hu_bg_memory_consolidation_create(hu_allocator_t *alloc, hu_bg_observer_t *out);
hu_error_t hu_bg_security_audit_create(hu_allocator_t *alloc, hu_bg_observer_t *out);
hu_error_t hu_bg_feed_processor_create(hu_allocator_t *alloc, hu_bg_observer_t *out);
hu_error_t hu_bg_dpo_export_create(hu_allocator_t *alloc, hu_bg_observer_t *out);
hu_error_t hu_bg_intelligence_cycle_create(hu_allocator_t *alloc, hu_bg_observer_t *out);
hu_error_t hu_bg_email_digest_create(hu_allocator_t *alloc, hu_bg_observer_t *out);

#endif
