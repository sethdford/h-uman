#ifndef SC_ROUTER_H
#define SC_ROUTER_H

#include "seaclaw/provider.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"

#define SC_HINT_PREFIX "hint:"
#define SC_HINT_PREFIX_LEN 5

typedef struct sc_route {
    const char *provider_name;
    size_t provider_name_len;
    const char *model;
    size_t model_len;
} sc_route_t;

typedef struct sc_router_route_entry {
    const char *hint;
    size_t hint_len;
    sc_route_t route;
} sc_router_route_entry_t;

/* Create a router that delegates to providers based on model/hint.
 * provider_names[i] and providers[i] must match; first provider is default.
 * routes[] maps hint names to (provider_name, model). Unknown hints use default. */
sc_error_t sc_router_create(sc_allocator_t *alloc,
    const char * const *provider_names,
    const size_t *provider_name_lens,
    size_t provider_count,
    sc_provider_t *providers,
    const sc_router_route_entry_t *routes,
    size_t route_count,
    const char *default_model,
    size_t default_model_len,
    sc_provider_t *out);

#endif /* SC_ROUTER_H */
