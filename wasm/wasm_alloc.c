/* WASM-specific bump allocator for Human.
 * Uses a fixed memory budget, no reclaim (free is no-op).
 * Realloc allocates new block and copies when growing.
 * Also compiled for native tests (HU_IS_TEST) to exercise allocator logic. */
#if defined(__wasi__) || defined(HU_IS_TEST)

#include "human/wasm/wasm_alloc.h"
#include <string.h>
#include <stddef.h>

#ifndef HU_WASM_ALLOC_DEFAULT_LIMIT
#define HU_WASM_ALLOC_DEFAULT_LIMIT (4 * 1024 * 1024)  /* 4 MB */
#endif

static void *wasm_alloc(void *ctx, size_t size) {
    hu_wasm_alloc_ctx_t *a = (hu_wasm_alloc_ctx_t *)ctx;
    if (!a || size == 0) return NULL;
    if (a->used + size > a->limit) return NULL;
    if (a->used + size > a->capacity) return NULL;
    void *ptr = a->base + a->used;
    a->used += size;
    return ptr;
}

static void *wasm_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size) {
    hu_wasm_alloc_ctx_t *a = (hu_wasm_alloc_ctx_t *)ctx;
    if (!a) return NULL;
    if (new_size == 0) return NULL;
    if (!ptr) return wasm_alloc(ctx, new_size);
    if (new_size <= old_size) return ptr;

    void *new_ptr = wasm_alloc(ctx, new_size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, old_size);
    return new_ptr;
}

static void wasm_free(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    (void)ptr;
    (void)size;
    /* Bump allocator: free is no-op. */
}

/* Static buffer for default allocator. Must fit context + heap. */
static unsigned char hu_wasm_alloc_static_buf[sizeof(hu_wasm_alloc_ctx_t) + 1024 * 1024];
static hu_wasm_alloc_ctx_t *hu_wasm_alloc_default_ctx;

hu_wasm_alloc_ctx_t *hu_wasm_alloc_ctx_create(void *memory, size_t capacity, size_t limit) {
    if (!memory || capacity <= sizeof(hu_wasm_alloc_ctx_t)) return NULL;
    hu_wasm_alloc_ctx_t *ctx = (hu_wasm_alloc_ctx_t *)memory;
    ctx->base = (unsigned char *)memory + sizeof(hu_wasm_alloc_ctx_t);
    ctx->capacity = capacity - sizeof(hu_wasm_alloc_ctx_t);
    ctx->used = 0;
    ctx->limit = limit > 0 ? limit : ctx->capacity;
    if (ctx->limit > ctx->capacity) ctx->limit = ctx->capacity;
    return ctx;
}

hu_allocator_t hu_wasm_allocator(hu_wasm_alloc_ctx_t *ctx) {
    if (!ctx) return (hu_allocator_t){ 0 };
    return (hu_allocator_t){
        .ctx = ctx,
        .alloc = wasm_alloc,
        .realloc = wasm_realloc,
        .free = wasm_free,
    };
}

size_t hu_wasm_allocator_used(const hu_wasm_alloc_ctx_t *ctx) {
    return ctx ? ctx->used : 0;
}

size_t hu_wasm_allocator_limit(const hu_wasm_alloc_ctx_t *ctx) {
    return ctx ? ctx->limit : 0;
}

/* One-time init of default allocator using static buffer. */
hu_allocator_t hu_wasm_allocator_default(void) {
    if (!hu_wasm_alloc_default_ctx) {
        hu_wasm_alloc_default_ctx = hu_wasm_alloc_ctx_create(
            hu_wasm_alloc_static_buf, sizeof(hu_wasm_alloc_static_buf),
            HU_WASM_ALLOC_DEFAULT_LIMIT);
    }
    return hu_wasm_allocator(hu_wasm_alloc_default_ctx);
}

#endif /* __wasi__ || HU_IS_TEST */
