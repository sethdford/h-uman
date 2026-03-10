#ifndef HU_MEMORY_VECTOR_PROVIDER_ROUTER_H
#define HU_MEMORY_VECTOR_PROVIDER_ROUTER_H

#include "human/core/allocator.h"
#include "human/memory/vector/embeddings.h"
#include <stddef.h>

/* Route config: hint -> provider selection */
typedef struct hu_embedding_route {
    const char *hint;
    const char *provider_name;
    const char *model;
    size_t dimensions;
} hu_embedding_route_t;

/* Router wraps primary + fallbacks, exposes same vtable.
 * On embed failure, tries fallbacks in order. */
typedef struct hu_embedding_provider_router hu_embedding_provider_router_t;

hu_embedding_provider_t
hu_embedding_provider_router_create(hu_allocator_t *alloc, hu_embedding_provider_t primary,
                                    const hu_embedding_provider_t *fallbacks, size_t fallback_count,
                                    const hu_embedding_route_t *routes, size_t route_count);

/* Extract hint from model string if it starts with "hint:". Returns NULL if no hint. */
const char *hu_embedding_extract_hint(const char *model, size_t model_len);

#endif
