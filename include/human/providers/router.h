#ifndef HU_ROUTER_H
#define HU_ROUTER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"

#define HU_HINT_PREFIX     "hint:"
#define HU_HINT_PREFIX_LEN 5

typedef struct hu_route {
    const char *provider_name;
    size_t provider_name_len;
    const char *model;
    size_t model_len;
} hu_route_t;

typedef struct hu_router_route_entry {
    const char *hint;
    size_t hint_len;
    hu_route_t route;
} hu_router_route_entry_t;

/* Create a router that delegates to providers based on model/hint.
 * provider_names[i] and providers[i] must match; first provider is default.
 * routes[] maps hint names to (provider_name, model). Unknown hints use default. */
hu_error_t hu_router_create(hu_allocator_t *alloc, const char *const *provider_names,
                            const size_t *provider_name_lens, size_t provider_count,
                            hu_provider_t *providers, const hu_router_route_entry_t *routes,
                            size_t route_count, const char *default_model, size_t default_model_len,
                            hu_provider_t *out);

/* Multi-model router: selects fast/standard/powerful based on prompt complexity.
 * fast/standard/powerful are provider structs; missing fast or powerful falls back to standard. */
typedef struct hu_multi_model_router_config {
    hu_provider_t fast;
    hu_provider_t standard;
    hu_provider_t powerful;
    int complexity_threshold_low;  /* below this -> fast (default 50) */
    int complexity_threshold_high; /* above this -> powerful (default 500) */
} hu_multi_model_router_config_t;

hu_error_t hu_multi_model_router_create(hu_allocator_t *alloc,
                                        const hu_multi_model_router_config_t *config,
                                        hu_provider_t *out);

#endif /* HU_ROUTER_H */
