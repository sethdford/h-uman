#include "seaclaw/memory.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <string.h>
#include <math.h>

typedef struct sc_none_memory {
    sc_allocator_t *alloc;
} sc_none_memory_t;

static const char *impl_name(void *ctx) {
    (void)ctx;
    return "none";
}

static sc_error_t impl_store(void *ctx,
    const char *key, size_t key_len,
    const char *content, size_t content_len,
    const sc_memory_category_t *category,
    const char *session_id, size_t session_id_len) {
    (void)ctx;
    (void)key;
    (void)key_len;
    (void)content;
    (void)content_len;
    (void)category;
    (void)session_id;
    (void)session_id_len;
    return SC_OK;
}

static sc_error_t impl_recall(void *ctx, sc_allocator_t *alloc,
    const char *query, size_t query_len,
    size_t limit,
    const char *session_id, size_t session_id_len,
    sc_memory_entry_t **out, size_t *out_count) {
    (void)ctx;
    (void)alloc;
    (void)query;
    (void)query_len;
    (void)limit;
    (void)session_id;
    (void)session_id_len;
    *out = NULL;
    *out_count = 0;
    return SC_OK;
}

static sc_error_t impl_get(void *ctx, sc_allocator_t *alloc,
    const char *key, size_t key_len,
    sc_memory_entry_t *out, bool *found) {
    (void)ctx;
    (void)alloc;
    (void)key;
    (void)key_len;
    (void)out;
    *found = false;
    return SC_OK;
}

static sc_error_t impl_list(void *ctx, sc_allocator_t *alloc,
    const sc_memory_category_t *category,
    const char *session_id, size_t session_id_len,
    sc_memory_entry_t **out, size_t *out_count) {
    (void)ctx;
    (void)alloc;
    (void)category;
    (void)session_id;
    (void)session_id_len;
    *out = NULL;
    *out_count = 0;
    return SC_OK;
}

static sc_error_t impl_forget(void *ctx,
    const char *key, size_t key_len,
    bool *deleted) {
    (void)ctx;
    (void)key;
    (void)key_len;
    *deleted = false;
    return SC_OK;
}

static sc_error_t impl_count(void *ctx, size_t *out) {
    (void)ctx;
    *out = 0;
    return SC_OK;
}

static bool impl_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static void impl_deinit(void *ctx) {
    sc_none_memory_t *self = (sc_none_memory_t *)ctx;
    if (self->alloc) {
        self->alloc->free(self->alloc->ctx, self, sizeof(sc_none_memory_t));
    }
}

static const sc_memory_vtable_t none_vtable = {
    .name = impl_name,
    .store = impl_store,
    .recall = impl_recall,
    .get = impl_get,
    .list = impl_list,
    .forget = impl_forget,
    .count = impl_count,
    .health_check = impl_health_check,
    .deinit = impl_deinit,
};

sc_memory_t sc_none_memory_create(sc_allocator_t *alloc) {
    sc_none_memory_t *self = (sc_none_memory_t *)alloc->alloc(alloc->ctx, sizeof(sc_none_memory_t));
    if (!self) return (sc_memory_t){ .ctx = NULL, .vtable = NULL };
    self->alloc = alloc;
    return (sc_memory_t){
        .ctx = self,
        .vtable = &none_vtable,
    };
}
