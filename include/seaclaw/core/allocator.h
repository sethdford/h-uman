#ifndef SC_ALLOCATOR_H
#define SC_ALLOCATOR_H

#include <stddef.h>
#include <stdbool.h>

typedef struct sc_allocator {
    void *ctx;
    void* (*alloc)(void *ctx, size_t size);
    void* (*realloc)(void *ctx, void *ptr, size_t old_size, size_t new_size);
    void  (*free)(void *ctx, void *ptr, size_t size);
} sc_allocator_t;

sc_allocator_t sc_system_allocator(void);

typedef struct sc_tracking_allocator sc_tracking_allocator_t;

sc_tracking_allocator_t *sc_tracking_allocator_create(void);
sc_allocator_t sc_tracking_allocator_allocator(sc_tracking_allocator_t *ta);
size_t sc_tracking_allocator_leaks(const sc_tracking_allocator_t *ta);
size_t sc_tracking_allocator_total_allocated(const sc_tracking_allocator_t *ta);
size_t sc_tracking_allocator_total_freed(const sc_tracking_allocator_t *ta);
void sc_tracking_allocator_destroy(sc_tracking_allocator_t *ta);

#endif
