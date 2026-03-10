#ifndef HU_WASM_PROVIDER_H
#define HU_WASM_PROVIDER_H

#ifdef __wasi__

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stddef.h>

hu_error_t hu_wasm_provider_create(hu_allocator_t *alloc, const char *api_key, size_t api_key_len,
                                   const char *base_url, size_t base_url_len, hu_provider_t *out);

#endif /* __wasi__ */

#endif /* HU_WASM_PROVIDER_H */
