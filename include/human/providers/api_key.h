#ifndef HU_API_KEY_H
#define HU_API_KEY_H

#include "human/core/allocator.h"
#include <stdbool.h>
#include <stddef.h>

/* Resolve API key: explicit key (trimmed), provider env var, or generic fallbacks.
 * Returns owned string or NULL. Caller must free. */
char *hu_api_key_resolve(hu_allocator_t *alloc, const char *provider_name, size_t provider_name_len,
                         const char *api_key, size_t api_key_len);

/* Validate API key format - returns true if non-empty after trim */
bool hu_api_key_valid(const char *key, size_t key_len);

/* Mask key for logs - show only last 4 chars */
char *hu_api_key_mask(hu_allocator_t *alloc, const char *key, size_t key_len);

#endif /* HU_API_KEY_H */
