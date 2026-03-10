#include "human/memory/vector/outbox.h"
#include "human/core/string.h"
#include <stdlib.h>
#include <string.h>

typedef struct outbox_item {
    char *id;
    size_t id_len;
    char *text;
    size_t text_len;
} outbox_item_t;

struct hu_embedding_outbox {
    hu_allocator_t *alloc;
    outbox_item_t *items;
    size_t count;
    size_t capacity;
};

hu_embedding_outbox_t *hu_embedding_outbox_create(hu_allocator_t *alloc) {
    if (!alloc)
        return NULL;
    hu_embedding_outbox_t *ob =
        (hu_embedding_outbox_t *)alloc->alloc(alloc->ctx, sizeof(hu_embedding_outbox_t));
    if (!ob)
        return NULL;
    memset(ob, 0, sizeof(*ob));
    ob->alloc = alloc;
    return ob;
}

void hu_embedding_outbox_destroy(hu_allocator_t *alloc, hu_embedding_outbox_t *ob) {
    if (!ob || !alloc)
        return;
    for (size_t i = 0; i < ob->count; i++) {
        if (ob->items[i].id)
            alloc->free(alloc->ctx, ob->items[i].id, ob->items[i].id_len + 1);
        if (ob->items[i].text)
            alloc->free(alloc->ctx, ob->items[i].text, ob->items[i].text_len + 1);
    }
    if (ob->items)
        alloc->free(alloc->ctx, ob->items, ob->capacity * sizeof(outbox_item_t));
    alloc->free(alloc->ctx, ob, sizeof(hu_embedding_outbox_t));
}

hu_error_t hu_embedding_outbox_enqueue(hu_embedding_outbox_t *ob, const char *id, size_t id_len,
                                       const char *text, size_t text_len) {
    if (!ob || !ob->alloc)
        return HU_ERR_INVALID_ARGUMENT;

    if (ob->count >= ob->capacity) {
        size_t new_cap = ob->capacity == 0 ? 8 : ob->capacity * 2;
        size_t old_sz = ob->capacity * sizeof(outbox_item_t);
        size_t new_sz = new_cap * sizeof(outbox_item_t);
        outbox_item_t *tmp =
            (outbox_item_t *)ob->alloc->realloc(ob->alloc->ctx, ob->items, old_sz, new_sz);
        if (!tmp)
            return HU_ERR_OUT_OF_MEMORY;
        ob->items = tmp;
        ob->capacity = new_cap;
    }

    outbox_item_t *it = &ob->items[ob->count];
    it->id = id && id_len > 0 ? hu_strndup(ob->alloc, id, id_len) : NULL;
    it->id_len = id ? id_len : 0;
    it->text = text && text_len > 0 ? hu_strndup(ob->alloc, text, text_len) : NULL;
    it->text_len = text ? text_len : 0;
    ob->count++;
    return HU_OK;
}

hu_error_t hu_embedding_outbox_flush(hu_embedding_outbox_t *ob, hu_allocator_t *alloc,
                                     hu_embedding_provider_t *provider,
                                     hu_embedding_outbox_flush_cb callback, void *userdata) {
    if (!ob || !alloc || !provider || !provider->vtable || !provider->vtable->embed)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < ob->count; i++) {
        outbox_item_t *it = &ob->items[i];
        const char *txt = it->text ? it->text : "";
        size_t txt_len = it->text_len;

        hu_embedding_provider_result_t res = {0};
        hu_error_t err = provider->vtable->embed(provider->ctx, alloc, txt, txt_len, &res);
        if (err == HU_OK) {
            if (res.values && callback)
                callback(userdata, it->id ? it->id : "", it->id_len, res.values, res.dimensions);
            hu_embedding_provider_free(alloc, &res);
        }
    }

    /* Clear queue after flush */
    for (size_t i = 0; i < ob->count; i++) {
        if (ob->items[i].id)
            alloc->free(alloc->ctx, ob->items[i].id, ob->items[i].id_len + 1);
        if (ob->items[i].text)
            alloc->free(alloc->ctx, ob->items[i].text, ob->items[i].text_len + 1);
    }
    ob->count = 0;
    return HU_OK;
}

size_t hu_embedding_outbox_pending_count(const hu_embedding_outbox_t *ob) {
    return ob ? ob->count : 0;
}
