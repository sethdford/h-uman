#include "seaclaw/core/arena.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SC_ARENA_BLOCK_SIZE 4096

typedef struct arena_block {
    uint8_t *data;
    size_t size;
    size_t used;
    struct arena_block *next;
} arena_block_t;

struct sc_arena {
    sc_allocator_t backing;
    arena_block_t *head;
    arena_block_t *current;
    size_t total_used;
};

static arena_block_t *block_new(sc_allocator_t *backing, size_t min_size) {
    size_t block_size = min_size > SC_ARENA_BLOCK_SIZE ? min_size : SC_ARENA_BLOCK_SIZE;
    if (block_size > SIZE_MAX - sizeof(arena_block_t)) return NULL;
    arena_block_t *blk = (arena_block_t *)backing->alloc(backing->ctx,
                                                          sizeof(arena_block_t) + block_size);
    if (!blk) return NULL;
    blk->data = (uint8_t *)(blk + 1);
    blk->size = block_size;
    blk->used = 0;
    blk->next = NULL;
    return blk;
}

static void *arena_alloc(void *ctx, size_t size) {
    sc_arena_t *arena = (sc_arena_t *)ctx;

    if (size > SIZE_MAX - 7) return NULL;
    size_t aligned = (size + 7) & ~(size_t)7;

    if (!arena->current || arena->current->used > SIZE_MAX - aligned ||
        arena->current->used + aligned > arena->current->size) {
        arena_block_t *blk = block_new(&arena->backing, aligned);
        if (!blk) return NULL;
        if (arena->current)
            arena->current->next = blk;
        else
            arena->head = blk;
        arena->current = blk;
    }

    void *ptr = arena->current->data + arena->current->used;
    arena->current->used += aligned;
    arena->total_used += size;
    return ptr;
}

static void *arena_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size) {
    void *new_ptr = arena_alloc(ctx, new_size);
    if (new_ptr && ptr) {
        memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    }
    return new_ptr;
}

static void arena_free(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    (void)ptr;
    (void)size;
}

sc_arena_t *sc_arena_create(sc_allocator_t backing) {
    sc_arena_t *arena = (sc_arena_t *)backing.alloc(backing.ctx, sizeof(sc_arena_t));
    if (!arena) return NULL;
    arena->backing = backing;
    arena->head = NULL;
    arena->current = NULL;
    arena->total_used = 0;
    return arena;
}

sc_allocator_t sc_arena_allocator(sc_arena_t *arena) {
    return (sc_allocator_t){
        .ctx = arena,
        .alloc = arena_alloc,
        .realloc = arena_realloc,
        .free = arena_free,
    };
}

void sc_arena_reset(sc_arena_t *arena) {
    for (arena_block_t *blk = arena->head; blk; blk = blk->next) {
        blk->used = 0;
    }
    arena->current = arena->head;
    arena->total_used = 0;
}

void sc_arena_destroy(sc_arena_t *arena) {
    arena_block_t *blk = arena->head;
    while (blk) {
        arena_block_t *next = blk->next;
        arena->backing.free(arena->backing.ctx, blk, sizeof(arena_block_t) + blk->size);
        blk = next;
    }
    arena->backing.free(arena->backing.ctx, arena, sizeof(sc_arena_t));
}

size_t sc_arena_bytes_used(const sc_arena_t *arena) {
    return arena->total_used;
}
