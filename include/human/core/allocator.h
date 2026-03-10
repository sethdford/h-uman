#ifndef HU_ALLOCATOR_H
#define HU_ALLOCATOR_H

#include <stdbool.h>
#include <stddef.h>

typedef struct hu_allocator {
    void *ctx;
    void *(*alloc)(void *ctx, size_t size);
    void *(*realloc)(void *ctx, void *ptr, size_t old_size, size_t new_size);
    void (*free)(void *ctx, void *ptr, size_t size);
} hu_allocator_t;

hu_allocator_t hu_system_allocator(void);

typedef struct hu_tracking_allocator hu_tracking_allocator_t;

hu_tracking_allocator_t *hu_tracking_allocator_create(void);
hu_allocator_t hu_tracking_allocator_allocator(hu_tracking_allocator_t *ta);
size_t hu_tracking_allocator_leaks(const hu_tracking_allocator_t *ta);
size_t hu_tracking_allocator_total_allocated(const hu_tracking_allocator_t *ta);
size_t hu_tracking_allocator_total_freed(const hu_tracking_allocator_t *ta);
void hu_tracking_allocator_destroy(hu_tracking_allocator_t *ta);

#endif
