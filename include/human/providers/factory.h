#ifndef HU_PROVIDERS_FACTORY_H
#define HU_PROVIDERS_FACTORY_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stddef.h>

struct hu_config;
hu_error_t hu_provider_create_from_config(hu_allocator_t *alloc, const struct hu_config *cfg,
                                          const char *name, size_t name_len, hu_provider_t *out);

hu_error_t hu_provider_create(hu_allocator_t *alloc, const char *name, size_t name_len,
                              const char *api_key, size_t api_key_len, const char *base_url,
                              size_t base_url_len, hu_provider_t *out);

/** Returns base URL for compatible providers (groq, mistral, etc.), NULL if unknown. */
const char *hu_compatible_provider_url(const char *name);

#endif /* HU_PROVIDERS_FACTORY_H */
