#ifndef HU_PROVIDERS_COMPATIBLE_H
#define HU_PROVIDERS_COMPATIBLE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stddef.h>

hu_error_t hu_compatible_create(hu_allocator_t *alloc, const char *api_key, size_t api_key_len,
                                const char *base_url, size_t base_url_len, hu_provider_t *out);

#endif
