#include "human/core/allocator.h"
#include "human/memory.h"

void hu_memory_entry_free_fields(hu_allocator_t *alloc, hu_memory_entry_t *e) {
    if (!alloc || !e)
        return;
    if (e->id)
        alloc->free(alloc->ctx, (void *)e->id, e->id_len + 1);
    if (e->key && e->key != e->id)
        alloc->free(alloc->ctx, (void *)e->key, e->key_len + 1);
    if (e->content)
        alloc->free(alloc->ctx, (void *)e->content, e->content_len + 1);
    if (e->category.data.custom.name)
        alloc->free(alloc->ctx, (void *)e->category.data.custom.name,
                    e->category.data.custom.name_len + 1);
    if (e->timestamp)
        alloc->free(alloc->ctx, (void *)e->timestamp, e->timestamp_len + 1);
    if (e->session_id)
        alloc->free(alloc->ctx, (void *)e->session_id, e->session_id_len + 1);
    if (e->source)
        alloc->free(alloc->ctx, (void *)e->source, e->source_len + 1);
}

hu_error_t hu_memory_store_with_source(hu_memory_t *mem, const char *key, size_t key_len,
                                       const char *content, size_t content_len,
                                       const hu_memory_category_t *category, const char *session_id,
                                       size_t session_id_len, const char *source,
                                       size_t source_len) {
    if (!mem || !mem->vtable)
        return HU_ERR_INVALID_ARGUMENT;

    if (mem->vtable->store_ex && source && source_len > 0) {
        hu_memory_store_opts_t opts = {
            .source = source, .source_len = source_len, .importance = -1.0};
        return mem->vtable->store_ex(mem->ctx, key, key_len, content, content_len, category,
                                     session_id, session_id_len, &opts);
    }
    return mem->vtable->store(mem->ctx, key, key_len, content, content_len, category, session_id,
                              session_id_len);
}
