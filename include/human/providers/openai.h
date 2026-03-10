#ifndef HU_PROVIDERS_OPENAI_H
#define HU_PROVIDERS_OPENAI_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stddef.h>

hu_error_t hu_openai_create(hu_allocator_t *alloc, const char *api_key, size_t api_key_len,
                            const char *base_url, size_t base_url_len, hu_provider_t *out);

void hu_openai_set_ws_streaming(hu_provider_t *p, bool enabled);

#endif /* HU_PROVIDERS_OPENAI_H */
