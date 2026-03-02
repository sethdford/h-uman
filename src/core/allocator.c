#include "seaclaw/core/allocator.h"
#include <stdlib.h>
#include <string.h>

/* --- System allocator (malloc/free) --- */

static void *sys_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}

static void *sys_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size) {
    (void)ctx;
    (void)old_size;
    return realloc(ptr, new_size);
}

static void sys_free(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    (void)size;
    free(ptr);
}

sc_allocator_t sc_system_allocator(void) {
    return (sc_allocator_t){
        .ctx = NULL,
        .alloc = sys_alloc,
        .realloc = sys_realloc,
        .free = sys_free,
    };
}

/* --- Tracking allocator for leak detection --- */

typedef struct track_entry {
    void *ptr;
    size_t size;
    struct track_entry *next;
} track_entry_t;

struct sc_tracking_allocator {
    sc_allocator_t backing;
    track_entry_t *entries;
    size_t total_allocated;
    size_t total_freed;
    size_t active_count;
};

static void track_free(void *ctx, void *ptr, size_t size);

static track_entry_t *track_find(sc_tracking_allocator_t *ta, void *ptr) {
    for (track_entry_t *e = ta->entries; e; e = e->next) {
        if (e->ptr == ptr) return e;
    }
    return NULL;
}

static void track_free(void *ctx, void *ptr, size_t size);

static void *track_alloc(void *ctx, size_t size) {
    sc_tracking_allocator_t *ta = (sc_tracking_allocator_t *)ctx;
    void *ptr = malloc(size);
    if (!ptr) return NULL;

    track_entry_t *entry = (track_entry_t *)malloc(sizeof(track_entry_t));
    if (!entry) { free(ptr); return NULL; }

    entry->ptr = ptr;
    entry->size = size;
    entry->next = ta->entries;
    ta->entries = entry;
    ta->total_allocated += size;
    ta->active_count++;
    return ptr;
}

static void *track_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size) {
    (void)old_size;
    sc_tracking_allocator_t *ta = (sc_tracking_allocator_t *)ctx;

    if (!ptr) return track_alloc(ctx, new_size);
    if (new_size == 0) {
        track_free(ctx, ptr, 0);
        return NULL;
    }

    track_entry_t *entry = track_find(ta, ptr);
    if (!entry) {
        /* ptr was not allocated by this allocator — reject to prevent UB */
        return NULL;
    }

    void *new_ptr = realloc(ptr, new_size);
    if (!new_ptr) return NULL;

    entry->ptr = new_ptr;
    if (new_size > entry->size)
        ta->total_allocated += (new_size - entry->size);
    else
        ta->total_freed += (entry->size - new_size);
    entry->size = new_size;
    return new_ptr;
}

static void track_free(void *ctx, void *ptr, size_t size) {
    sc_tracking_allocator_t *ta = (sc_tracking_allocator_t *)ctx;
    if (!ptr) return;

    track_entry_t **prev = &ta->entries;
    for (track_entry_t *e = ta->entries; e; prev = &e->next, e = e->next) {
        if (e->ptr == ptr) {
            *prev = e->next;
            ta->total_freed += e->size;
            ta->active_count--;
            free(e);
            break;
        }
    }
    free(ptr);
    (void)size;
}

sc_tracking_allocator_t *sc_tracking_allocator_create(void) {
    sc_tracking_allocator_t *ta = (sc_tracking_allocator_t *)calloc(1, sizeof(*ta));
    return ta;
}

sc_allocator_t sc_tracking_allocator_allocator(sc_tracking_allocator_t *ta) {
    return (sc_allocator_t){
        .ctx = ta,
        .alloc = track_alloc,
        .realloc = track_realloc,
        .free = track_free,
    };
}

size_t sc_tracking_allocator_leaks(const sc_tracking_allocator_t *ta) {
    return ta->active_count;
}

size_t sc_tracking_allocator_total_allocated(const sc_tracking_allocator_t *ta) {
    return ta->total_allocated;
}

size_t sc_tracking_allocator_total_freed(const sc_tracking_allocator_t *ta) {
    return ta->total_freed;
}

void sc_tracking_allocator_destroy(sc_tracking_allocator_t *ta) {
    track_entry_t *e = ta->entries;
    while (e) {
        track_entry_t *next = e->next;
        free(e->ptr);
        free(e);
        e = next;
    }
    free(ta);
}
