#ifndef SC_API_KEY_H
#define SC_API_KEY_H

#include "seaclaw/core/allocator.h"
#include <stdbool.h>
#include <stddef.h>

/* Resolve API key: explicit key (trimmed), provider env var, or generic fallbacks.
 * Returns owned string or NULL. Caller must free. */
char *sc_api_key_resolve(sc_allocator_t *alloc,
    const char *provider_name, size_t provider_name_len,
    const char *api_key, size_t api_key_len);

/* Validate API key format - returns true if non-empty after trim */
bool sc_api_key_valid(const char *key, size_t key_len);

/* Mask key for logs - show only last 4 chars */
char *sc_api_key_mask(sc_allocator_t *alloc, const char *key, size_t key_len);

#endif /* SC_API_KEY_H */
