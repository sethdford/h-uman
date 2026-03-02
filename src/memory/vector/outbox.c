#include "seaclaw/memory/vector/outbox.h"
#include "seaclaw/core/string.h"
#include <stdlib.h>
#include <string.h>

typedef struct outbox_item {
    char *id;
    size_t id_len;
    char *text;
    size_t text_len;
} outbox_item_t;

struct sc_embedding_outbox {
    sc_allocator_t *alloc;
    outbox_item_t *items;
    size_t count;
    size_t capacity;
};

sc_embedding_outbox_t *sc_embedding_outbox_create(sc_allocator_t *alloc) {
    if (!alloc) return NULL;
    sc_embedding_outbox_t *ob = (sc_embedding_outbox_t *)alloc->alloc(alloc->ctx,
        sizeof(sc_embedding_outbox_t));
    if (!ob) return NULL;
    memset(ob, 0, sizeof(*ob));
    ob->alloc = alloc;
    return ob;
}

void sc_embedding_outbox_destroy(sc_allocator_t *alloc, sc_embedding_outbox_t *ob) {
    if (!ob || !alloc) return;
    for (size_t i = 0; i < ob->count; i++) {
        if (ob->items[i].id) alloc->free(alloc->ctx, ob->items[i].id, ob->items[i].id_len + 1);
        if (ob->items[i].text) alloc->free(alloc->ctx, ob->items[i].text, ob->items[i].text_len + 1);
    }
    if (ob->items) alloc->free(alloc->ctx, ob->items,
        ob->capacity * sizeof(outbox_item_t));
    alloc->free(alloc->ctx, ob, sizeof(sc_embedding_outbox_t));
}

sc_error_t sc_embedding_outbox_enqueue(sc_embedding_outbox_t *ob,
    const char *id, size_t id_len,
    const char *text, size_t text_len) {
    if (!ob || !ob->alloc) return SC_ERR_INVALID_ARGUMENT;

    if (ob->count >= ob->capacity) {
        size_t new_cap = ob->capacity == 0 ? 8 : ob->capacity * 2;
        size_t old_sz = ob->capacity * sizeof(outbox_item_t);
        size_t new_sz = new_cap * sizeof(outbox_item_t);
        outbox_item_t *tmp = (outbox_item_t *)ob->alloc->realloc(ob->alloc->ctx,
            ob->items, old_sz, new_sz);
        if (!tmp) return SC_ERR_OUT_OF_MEMORY;
        ob->items = tmp;
        ob->capacity = new_cap;
    }

    outbox_item_t *it = &ob->items[ob->count];
    it->id = id && id_len > 0 ? sc_strndup(ob->alloc, id, id_len) : NULL;
    it->id_len = id ? id_len : 0;
    it->text = text && text_len > 0 ? sc_strndup(ob->alloc, text, text_len) : NULL;
    it->text_len = text ? text_len : 0;
    ob->count++;
    return SC_OK;
}

sc_error_t sc_embedding_outbox_flush(sc_embedding_outbox_t *ob,
    sc_allocator_t *alloc,
    sc_embedding_provider_t *provider,
    sc_embedding_outbox_flush_cb callback,
    void *userdata) {
    if (!ob || !alloc || !provider || !provider->vtable || !provider->vtable->embed)
        return SC_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < ob->count; i++) {
        outbox_item_t *it = &ob->items[i];
        const char *txt = it->text ? it->text : "";
        size_t txt_len = it->text_len;

        sc_embedding_provider_result_t res = {0};
        sc_error_t err = provider->vtable->embed(provider->ctx, alloc, txt, txt_len, &res);
        if (err == SC_OK && res.values && callback) {
            callback(userdata, it->id ? it->id : "", it->id_len, res.values, res.dimensions);
            sc_embedding_provider_free(alloc, &res);
        }
    }

    /* Clear queue after flush */
    for (size_t i = 0; i < ob->count; i++) {
        if (ob->items[i].id) alloc->free(alloc->ctx, ob->items[i].id, ob->items[i].id_len + 1);
        if (ob->items[i].text) alloc->free(alloc->ctx, ob->items[i].text, ob->items[i].text_len + 1);
    }
    ob->count = 0;
    return SC_OK;
}

size_t sc_embedding_outbox_pending_count(const sc_embedding_outbox_t *ob) {
    return ob ? ob->count : 0;
}
