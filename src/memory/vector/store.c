#include "human/memory/vector/store.h"
#include "human/core/string.h"
#include "human/memory/vector_math.h"
#include <stdlib.h>
#include <string.h>

typedef struct mem_vec_entry {
    char *id;
    float *embedding;
    size_t dims;
} mem_vec_entry_t;

typedef struct mem_vec_ctx {
    hu_allocator_t *alloc;
    mem_vec_entry_t *entries;
    size_t count;
    size_t capacity;
} mem_vec_ctx_t;

static int score_cmp(const void *va, const void *vb) {
    const hu_vector_search_result_t *a = (const hu_vector_search_result_t *)va;
    const hu_vector_search_result_t *b = (const hu_vector_search_result_t *)vb;
    if (a->score > b->score)
        return -1;
    if (a->score < b->score)
        return 1;
    return 0;
}

static hu_error_t mem_vec_upsert(void *ctx, hu_allocator_t *alloc, const char *id, size_t id_len,
                                 const float *embedding, size_t dims, const char *metadata,
                                 size_t metadata_len) {
    (void)metadata;
    (void)metadata_len;
    mem_vec_ctx_t *m = (mem_vec_ctx_t *)ctx;
    if (!m || !alloc || !id || !embedding)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < m->count; i++) {
        if (strlen(m->entries[i].id) == id_len && memcmp(m->entries[i].id, id, id_len) == 0) {
            alloc->free(alloc->ctx, m->entries[i].embedding, m->entries[i].dims * sizeof(float));
            m->entries[i].id = hu_strndup(alloc, id, id_len);
            m->entries[i].embedding = (float *)alloc->alloc(alloc->ctx, dims * sizeof(float));
            if (!m->entries[i].embedding)
                return HU_ERR_OUT_OF_MEMORY;
            memcpy(m->entries[i].embedding, embedding, dims * sizeof(float));
            m->entries[i].dims = dims;
            return HU_OK;
        }
    }

    if (m->count >= m->capacity) {
        size_t new_cap = m->capacity == 0 ? 16 : m->capacity * 2;
        size_t old_sz = m->capacity * sizeof(mem_vec_entry_t);
        size_t new_sz = new_cap * sizeof(mem_vec_entry_t);
        mem_vec_entry_t *tmp =
            (mem_vec_entry_t *)alloc->realloc(alloc->ctx, m->entries, old_sz, new_sz);
        if (!tmp)
            return HU_ERR_OUT_OF_MEMORY;
        m->entries = tmp;
        m->capacity = new_cap;
    }

    mem_vec_entry_t *e = &m->entries[m->count];
    e->id = hu_strndup(alloc, id, id_len);
    e->embedding = (float *)alloc->alloc(alloc->ctx, dims * sizeof(float));
    if (!e->id || !e->embedding)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(e->embedding, embedding, dims * sizeof(float));
    e->dims = dims;
    m->count++;
    return HU_OK;
}

static hu_error_t mem_vec_search(void *ctx, hu_allocator_t *alloc, const float *query_embedding,
                                 size_t dims, size_t limit, hu_vector_search_result_t **results,
                                 size_t *result_count) {
    mem_vec_ctx_t *m = (mem_vec_ctx_t *)ctx;
    if (!m || !query_embedding || !results || !result_count)
        return HU_ERR_INVALID_ARGUMENT;

    hu_vector_search_result_t *arr = (hu_vector_search_result_t *)alloc->alloc(
        alloc->ctx, m->count * sizeof(hu_vector_search_result_t));
    if (!arr) {
        *result_count = 0;
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(arr, 0, m->count * sizeof(hu_vector_search_result_t));

    size_t n = 0;
    for (size_t i = 0; i < m->count; i++) {
        if (m->entries[i].dims != dims)
            continue;
        float sim = hu_vector_cosine_similarity(query_embedding, m->entries[i].embedding, dims);
        arr[n].id = hu_strdup(alloc, m->entries[i].id);
        arr[n].score = sim;
        n++;
    }

    if (n > 1)
        qsort(arr, n, sizeof(hu_vector_search_result_t), score_cmp);
    if (limit > 0 && n > limit) {
        for (size_t i = limit; i < n; i++) {
            if (arr[i].id)
                alloc->free(alloc->ctx, arr[i].id, strlen(arr[i].id) + 1);
        }
        n = limit;
    }
    *results = arr;
    *result_count = n;
    return HU_OK;
}

static hu_error_t mem_vec_delete(void *ctx, hu_allocator_t *alloc, const char *id, size_t id_len) {
    mem_vec_ctx_t *m = (mem_vec_ctx_t *)ctx;
    if (!m || !alloc)
        return HU_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < m->count; i++) {
        if (strlen(m->entries[i].id) == id_len && memcmp(m->entries[i].id, id, id_len) == 0) {
            alloc->free(alloc->ctx, m->entries[i].id, strlen(m->entries[i].id) + 1);
            alloc->free(alloc->ctx, m->entries[i].embedding, m->entries[i].dims * sizeof(float));
            memmove(&m->entries[i], &m->entries[i + 1],
                    (m->count - 1 - i) * sizeof(mem_vec_entry_t));
            m->count--;
            return HU_OK;
        }
    }
    return HU_OK;
}

static size_t mem_vec_count(void *ctx) {
    mem_vec_ctx_t *m = (mem_vec_ctx_t *)ctx;
    return m ? m->count : 0;
}

static void mem_vec_deinit(void *ctx, hu_allocator_t *alloc) {
    mem_vec_ctx_t *m = (mem_vec_ctx_t *)ctx;
    if (!m || !alloc)
        return;
    for (size_t i = 0; i < m->count; i++) {
        if (m->entries[i].id)
            alloc->free(alloc->ctx, m->entries[i].id, strlen(m->entries[i].id) + 1);
        if (m->entries[i].embedding)
            alloc->free(alloc->ctx, m->entries[i].embedding, m->entries[i].dims * sizeof(float));
    }
    if (m->entries)
        alloc->free(alloc->ctx, m->entries, m->capacity * sizeof(mem_vec_entry_t));
    alloc->free(alloc->ctx, m, sizeof(mem_vec_ctx_t));
}

static const hu_vector_store_vtable_t mem_vec_vtable = {
    .upsert = mem_vec_upsert,
    .search = mem_vec_search,
    .delete = mem_vec_delete,
    .count = mem_vec_count,
    .deinit = mem_vec_deinit,
};

hu_vector_store_t hu_vector_store_mem_vec_create(hu_allocator_t *alloc) {
    hu_vector_store_t s = {.ctx = NULL, .vtable = &mem_vec_vtable};
    if (!alloc)
        return s;
    mem_vec_ctx_t *m = (mem_vec_ctx_t *)alloc->alloc(alloc->ctx, sizeof(mem_vec_ctx_t));
    if (!m)
        return s;
    memset(m, 0, sizeof(*m));
    m->alloc = alloc;
    s.ctx = m;
    return s;
}

void hu_vector_search_results_free(hu_allocator_t *alloc, hu_vector_search_result_t *results,
                                   size_t count) {
    if (!alloc || !results)
        return;
    for (size_t i = 0; i < count; i++) {
        if (results[i].id)
            alloc->free(alloc->ctx, results[i].id, strlen(results[i].id) + 1);
        if (results[i].metadata)
            alloc->free(alloc->ctx, results[i].metadata, strlen(results[i].metadata) + 1);
    }
    alloc->free(alloc->ctx, results, count * sizeof(hu_vector_search_result_t));
}
