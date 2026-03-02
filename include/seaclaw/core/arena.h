#ifndef SC_ARENA_H
#define SC_ARENA_H

#include "allocator.h"

typedef struct sc_arena sc_arena_t;

sc_arena_t *sc_arena_create(sc_allocator_t backing);
sc_allocator_t sc_arena_allocator(sc_arena_t *arena);
void sc_arena_reset(sc_arena_t *arena);
void sc_arena_destroy(sc_arena_t *arena);
size_t sc_arena_bytes_used(const sc_arena_t *arena);

#endif
