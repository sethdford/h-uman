#ifndef SC_WASM_ALLOC_H
#define SC_WASM_ALLOC_H

#if defined(__wasi__) || defined(SC_IS_TEST)

#include "seaclaw/core/allocator.h"
#include <stddef.h>

typedef struct sc_wasm_alloc_ctx {
    unsigned char *base;
    size_t capacity;
    size_t used;
    size_t limit;
} sc_wasm_alloc_ctx_t;

sc_wasm_alloc_ctx_t *sc_wasm_alloc_ctx_create(void *memory, size_t capacity, size_t limit);
sc_allocator_t sc_wasm_allocator(sc_wasm_alloc_ctx_t *ctx);
sc_allocator_t sc_wasm_allocator_default(void);
size_t sc_wasm_allocator_used(const sc_wasm_alloc_ctx_t *ctx);
size_t sc_wasm_allocator_limit(const sc_wasm_alloc_ctx_t *ctx);

#endif /* __wasi__ || SC_IS_TEST */

#endif /* SC_WASM_ALLOC_H */
