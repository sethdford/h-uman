#ifndef HU_WASM_ALLOC_H
#define HU_WASM_ALLOC_H

#if defined(__wasi__) || defined(HU_IS_TEST)

#include "human/core/allocator.h"
#include <stddef.h>

typedef struct hu_wasm_alloc_ctx {
    unsigned char *base;
    size_t capacity;
    size_t used;
    size_t limit;
} hu_wasm_alloc_ctx_t;

hu_wasm_alloc_ctx_t *hu_wasm_alloc_ctx_create(void *memory, size_t capacity, size_t limit);
hu_allocator_t hu_wasm_allocator(hu_wasm_alloc_ctx_t *ctx);
hu_allocator_t hu_wasm_allocator_default(void);
size_t hu_wasm_allocator_used(const hu_wasm_alloc_ctx_t *ctx);
size_t hu_wasm_allocator_limit(const hu_wasm_alloc_ctx_t *ctx);

#endif /* __wasi__ || HU_IS_TEST */

#endif /* HU_WASM_ALLOC_H */
