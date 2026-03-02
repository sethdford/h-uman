#ifndef SC_WASM_PROVIDER_H
#define SC_WASM_PROVIDER_H

#ifdef __wasi__

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/provider.h"
#include <stddef.h>

sc_error_t sc_wasm_provider_create(sc_allocator_t *alloc,
    const char *api_key, size_t api_key_len,
    const char *base_url, size_t base_url_len,
    sc_provider_t *out);

#endif /* __wasi__ */

#endif /* SC_WASM_PROVIDER_H */
