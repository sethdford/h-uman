#ifndef HU_MY_PROVIDER_H
#define HU_MY_PROVIDER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stddef.h>

/**
 * Create a custom AI provider implementing hu_provider_t.
 *
 * Register in src/providers/factory.c:
 *   if (strcmp(name, "my_provider") == 0)
 *       return hu_my_provider_create(alloc, api_key, api_key_len, base_url, base_url_len, out);
 */
hu_error_t hu_my_provider_create(hu_allocator_t *alloc,
    const char *api_key, size_t api_key_len,
    const char *base_url, size_t base_url_len,
    hu_provider_t *out);

/** Call prov->vtable->deinit(prov->ctx, alloc) with the same allocator used at create. */
void hu_my_provider_destroy(hu_provider_t *prov, hu_allocator_t *alloc);

#endif /* HU_MY_PROVIDER_H */
