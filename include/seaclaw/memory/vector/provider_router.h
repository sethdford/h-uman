#ifndef SC_MEMORY_VECTOR_PROVIDER_ROUTER_H
#define SC_MEMORY_VECTOR_PROVIDER_ROUTER_H

#include "seaclaw/memory/vector/embeddings.h"
#include "seaclaw/core/allocator.h"
#include <stddef.h>

/* Route config: hint -> provider selection */
typedef struct sc_embedding_route {
    const char *hint;
    const char *provider_name;
    const char *model;
    size_t dimensions;
} sc_embedding_route_t;

/* Router wraps primary + fallbacks, exposes same vtable.
 * On embed failure, tries fallbacks in order. */
typedef struct sc_embedding_provider_router sc_embedding_provider_router_t;

sc_embedding_provider_t sc_embedding_provider_router_create(sc_allocator_t *alloc,
    sc_embedding_provider_t primary,
    const sc_embedding_provider_t *fallbacks,
    size_t fallback_count,
    const sc_embedding_route_t *routes,
    size_t route_count);

/* Extract hint from model string if it starts with "hint:". Returns NULL if no hint. */
const char *sc_embedding_extract_hint(const char *model, size_t model_len);

#endif
