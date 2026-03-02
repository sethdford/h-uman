#include "seaclaw/memory.h"
#include "seaclaw/core/allocator.h"

void sc_memory_entry_free_fields(sc_allocator_t *alloc, sc_memory_entry_t *e) {
    if (!alloc || !e) return;
    if (e->id) alloc->free(alloc->ctx, (void *)e->id, e->id_len + 1);
    if (e->key && e->key != e->id) alloc->free(alloc->ctx, (void *)e->key, e->key_len + 1);
    if (e->content) alloc->free(alloc->ctx, (void *)e->content, e->content_len + 1);
    if (e->category.data.custom.name)
        alloc->free(alloc->ctx, (void *)e->category.data.custom.name,
            e->category.data.custom.name_len + 1);
    if (e->timestamp)
        alloc->free(alloc->ctx, (void *)e->timestamp, e->timestamp_len + 1);
    if (e->session_id)
        alloc->free(alloc->ctx, (void *)e->session_id, e->session_id_len + 1);
}
