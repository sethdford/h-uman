#ifndef HU_ARENA_H
#define HU_ARENA_H

#include "allocator.h"

typedef struct hu_arena hu_arena_t;

hu_arena_t *hu_arena_create(hu_allocator_t backing);
hu_allocator_t hu_arena_allocator(hu_arena_t *arena);
void hu_arena_reset(hu_arena_t *arena);
void hu_arena_destroy(hu_arena_t *arena);
size_t hu_arena_bytes_used(const hu_arena_t *arena);

#endif
