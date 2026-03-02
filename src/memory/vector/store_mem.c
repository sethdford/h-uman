#include "seaclaw/memory/vector.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdlib.h>
#include <string.h>

typedef struct mem_entry {
    char *id;
    size_t id_len;
    float *embedding;
    size_t embedding_dim;
    char *content;
    size_t content_len;
} mem_entry_t;

typedef struct mem_store_ctx {
    sc_allocator_t *alloc;
    mem_entry_t *entries;
    size_t count;
    size_t capacity;
} mem_store_ctx_t;

static int score_cmp(const void *va, const void *vb) {
    const sc_vector_entry_t *a = (const sc_vector_entry_t *)va;
    const sc_vector_entry_t *b = (const sc_vector_entry_t *)vb;
    if (a->score > b->score) return -1;
    if (a->score < b->score) return 1;
    return 0;
}

static sc_error_t insert_impl(void *ctx, sc_allocator_t *alloc,
    const char *id, size_t id_len,
    const sc_embedding_t *embedding,
    const char *content, size_t content_len) {
    mem_store_ctx_t *m = (mem_store_ctx_t *)ctx;
    if (!m || !alloc || !id || !embedding || !embedding->values)
        return SC_ERR_INVALID_ARGUMENT;

    /* Replace existing if same id */
    for (size_t i = 0; i < m->count; i++) {
        if (m->entries[i].id_len == id_len &&
            memcmp(m->entries[i].id, id, id_len) == 0) {
            alloc->free(alloc->ctx, m->entries[i].id, m->entries[i].id_len + 1);
            alloc->free(alloc->ctx, m->entries[i].embedding,
                m->entries[i].embedding_dim * sizeof(float));
            alloc->free(alloc->ctx, m->entries[i].content,
                m->entries[i].content_len + 1);
            mem_entry_t *e = &m->entries[i];
            e->id = (char *)alloc->alloc(alloc->ctx, id_len + 1);
            if (!e->id) return SC_ERR_OUT_OF_MEMORY;
            memcpy(e->id, id, id_len);
            e->id[id_len] = '\0';
            e->id_len = id_len;
            e->embedding_dim = embedding->dim;
            e->embedding = (float *)alloc->alloc(alloc->ctx, embedding->dim * sizeof(float));
            if (!e->embedding) {
                alloc->free(alloc->ctx, e->id, id_len + 1);
                return SC_ERR_OUT_OF_MEMORY;
            }
            memcpy(e->embedding, embedding->values, embedding->dim * sizeof(float));
            e->content_len = content ? content_len : 0;
            e->content = NULL;
            if (content && content_len > 0) {
                e->content = (char *)alloc->alloc(alloc->ctx, content_len + 1);
                if (!e->content) {
                    alloc->free(alloc->ctx, e->id, id_len + 1);
                    alloc->free(alloc->ctx, e->embedding, embedding->dim * sizeof(float));
                    return SC_ERR_OUT_OF_MEMORY;
                }
                memcpy(e->content, content, content_len);
                e->content[content_len] = '\0';
            }
            return SC_OK;
        }
    }

    if (m->count >= m->capacity) {
        size_t new_cap = m->capacity == 0 ? 16 : m->capacity * 2;
        mem_entry_t *tmp = (mem_entry_t *)alloc->realloc(alloc->ctx,
            m->entries,
            m->capacity * sizeof(mem_entry_t),
            new_cap * sizeof(mem_entry_t));
        if (!tmp) return SC_ERR_OUT_OF_MEMORY;
        m->entries = tmp;
        m->capacity = new_cap;
    }

    mem_entry_t *e = &m->entries[m->count];
    e->id = (char *)alloc->alloc(alloc->ctx, id_len + 1);
    if (!e->id) return SC_ERR_OUT_OF_MEMORY;
    memcpy(e->id, id, id_len);
    e->id[id_len] = '\0';
    e->id_len = id_len;
    e->embedding_dim = embedding->dim;
    e->embedding = (float *)alloc->alloc(alloc->ctx, embedding->dim * sizeof(float));
    if (!e->embedding) {
        alloc->free(alloc->ctx, e->id, id_len + 1);
        return SC_ERR_OUT_OF_MEMORY;
    }
    memcpy(e->embedding, embedding->values, embedding->dim * sizeof(float));
    e->content_len = content ? content_len : 0;
    e->content = NULL;
    if (content && content_len > 0) {
        e->content = (char *)alloc->alloc(alloc->ctx, content_len + 1);
        if (!e->content) {
            alloc->free(alloc->ctx, e->id, id_len + 1);
            alloc->free(alloc->ctx, e->embedding, embedding->dim * sizeof(float));
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(e->content, content, content_len);
        e->content[content_len] = '\0';
    }
    m->count++;
    return SC_OK;
}

static sc_error_t search_impl(void *ctx, sc_allocator_t *alloc,
    const sc_embedding_t *query, size_t limit,
    sc_vector_entry_t **out, size_t *out_count) {
    mem_store_ctx_t *m = (mem_store_ctx_t *)ctx;
    if (!m || !alloc || !query || !query->values || !out || !out_count)
        return SC_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_count = 0;

    if (m->count == 0)
        return SC_OK;

    sc_vector_entry_t *results = (sc_vector_entry_t *)alloc->alloc(alloc->ctx,
        sizeof(sc_vector_entry_t) * m->count);
    if (!results)
        return SC_ERR_OUT_OF_MEMORY;

    size_t n = 0;
    for (size_t i = 0; i < m->count; i++) {
        mem_entry_t *e = &m->entries[i];
        if (e->embedding_dim != query->dim)
            continue;
        float score = sc_cosine_similarity(query->values, e->embedding, e->embedding_dim);
        results[n].id_len = e->id_len;
        results[n].embedding.dim = e->embedding_dim;
        results[n].content_len = e->content_len;
        results[n].score = score;

        results[n].id = (char *)alloc->alloc(alloc->ctx, e->id_len + 1);
        if (!results[n].id) {
            for (size_t j = 0; j < n; j++) {
                alloc->free(alloc->ctx, (void *)results[j].id, results[j].id_len + 1);
                alloc->free(alloc->ctx, results[j].embedding.values,
                    results[j].embedding.dim * sizeof(float));
                if (results[j].content)
                    alloc->free(alloc->ctx, (void *)results[j].content, results[j].content_len + 1);
            }
            alloc->free(alloc->ctx, results, sizeof(sc_vector_entry_t) * m->count);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy((void *)results[n].id, e->id, e->id_len + 1);

        results[n].embedding.values = (float *)alloc->alloc(alloc->ctx,
            e->embedding_dim * sizeof(float));
        if (!results[n].embedding.values) {
            alloc->free(alloc->ctx, (void *)results[n].id, e->id_len + 1);
            for (size_t j = 0; j < n; j++) {
                alloc->free(alloc->ctx, (void *)results[j].id, results[j].id_len + 1);
                alloc->free(alloc->ctx, results[j].embedding.values,
                    results[j].embedding.dim * sizeof(float));
                if (results[j].content)
                    alloc->free(alloc->ctx, (void *)results[j].content, results[j].content_len + 1);
            }
            alloc->free(alloc->ctx, results, sizeof(sc_vector_entry_t) * m->count);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(results[n].embedding.values, e->embedding, e->embedding_dim * sizeof(float));

        if (e->content && e->content_len > 0) {
            results[n].content = (char *)alloc->alloc(alloc->ctx, e->content_len + 1);
            if (!results[n].content) {
                alloc->free(alloc->ctx, (void *)results[n].id, e->id_len + 1);
                alloc->free(alloc->ctx, results[n].embedding.values,
                    e->embedding_dim * sizeof(float));
                for (size_t j = 0; j < n; j++) {
                    alloc->free(alloc->ctx, (void *)results[j].id, results[j].id_len + 1);
                    alloc->free(alloc->ctx, results[j].embedding.values,
                        results[j].embedding.dim * sizeof(float));
                    if (results[j].content)
                        alloc->free(alloc->ctx, (void *)results[j].content, results[j].content_len + 1);
                }
                alloc->free(alloc->ctx, results, sizeof(sc_vector_entry_t) * m->count);
                return SC_ERR_OUT_OF_MEMORY;
            }
            memcpy((void *)results[n].content, e->content, e->content_len + 1);
        } else {
            results[n].content = NULL;
        }
        n++;
    }

    if (n == 0) {
        alloc->free(alloc->ctx, results, sizeof(sc_vector_entry_t) * m->count);
        return SC_OK;
    }

    qsort(results, n, sizeof(sc_vector_entry_t), score_cmp);

    size_t out_n = n < limit ? n : limit;
    if (out_n < n) {
        for (size_t j = out_n; j < n; j++) {
            alloc->free(alloc->ctx, (void *)results[j].id, results[j].id_len + 1);
            alloc->free(alloc->ctx, results[j].embedding.values,
                results[j].embedding.dim * sizeof(float));
            if (results[j].content)
                alloc->free(alloc->ctx, (void *)results[j].content, results[j].content_len + 1);
        }
        sc_vector_entry_t *trimmed = (sc_vector_entry_t *)alloc->alloc(alloc->ctx,
            sizeof(sc_vector_entry_t) * out_n);
        if (!trimmed) {
            for (size_t j = 0; j < n; j++) {
                alloc->free(alloc->ctx, (void *)results[j].id, results[j].id_len + 1);
                alloc->free(alloc->ctx, results[j].embedding.values,
                    results[j].embedding.dim * sizeof(float));
                if (results[j].content)
                    alloc->free(alloc->ctx, (void *)results[j].content, results[j].content_len + 1);
            }
            alloc->free(alloc->ctx, results, sizeof(sc_vector_entry_t) * m->count);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(trimmed, results, sizeof(sc_vector_entry_t) * out_n);
        alloc->free(alloc->ctx, results, sizeof(sc_vector_entry_t) * m->count);
        *out = trimmed;
    } else {
        *out = results;
    }
    *out_count = out_n;
    return SC_OK;
}

static sc_error_t remove_impl(void *ctx, const char *id, size_t id_len) {
    mem_store_ctx_t *m = (mem_store_ctx_t *)ctx;
    if (!m || !id)
        return SC_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < m->count; i++) {
        if (m->entries[i].id_len == id_len &&
            memcmp(m->entries[i].id, id, id_len) == 0) {
            sc_allocator_t *a = m->alloc;
            a->free(a->ctx, m->entries[i].id, m->entries[i].id_len + 1);
            a->free(a->ctx, m->entries[i].embedding,
                m->entries[i].embedding_dim * sizeof(float));
            if (m->entries[i].content)
                a->free(a->ctx, m->entries[i].content, m->entries[i].content_len + 1);
            memmove(&m->entries[i], &m->entries[i + 1],
                (m->count - 1 - i) * sizeof(mem_entry_t));
            m->count--;
            return SC_OK;
        }
    }
    return SC_ERR_NOT_FOUND;
}

static size_t count_impl(void *ctx) {
    mem_store_ctx_t *m = (mem_store_ctx_t *)ctx;
    return m ? m->count : 0;
}

static void deinit_impl(void *ctx, sc_allocator_t *alloc) {
    mem_store_ctx_t *m = (mem_store_ctx_t *)ctx;
    if (!m || !alloc) return;
    for (size_t i = 0; i < m->count; i++) {
        alloc->free(alloc->ctx, m->entries[i].id, m->entries[i].id_len + 1);
        alloc->free(alloc->ctx, m->entries[i].embedding,
            m->entries[i].embedding_dim * sizeof(float));
        if (m->entries[i].content)
            alloc->free(alloc->ctx, m->entries[i].content, m->entries[i].content_len + 1);
    }
    if (m->entries)
        alloc->free(alloc->ctx, m->entries, m->capacity * sizeof(mem_entry_t));
    m->entries = NULL;
    m->count = 0;
    m->capacity = 0;
}

static const sc_vector_store_vtable_t mem_vtable = {
    .insert = insert_impl,
    .search = search_impl,
    .remove = remove_impl,
    .count = count_impl,
    .deinit = deinit_impl,
};

sc_vector_store_t sc_vector_store_mem_create(sc_allocator_t *alloc) {
    sc_vector_store_t vs = { .ctx = NULL, .vtable = &mem_vtable };
    if (!alloc) return vs;

    mem_store_ctx_t *ctx = (mem_store_ctx_t *)alloc->alloc(alloc->ctx, sizeof(mem_store_ctx_t));
    if (!ctx) return vs;
    ctx->alloc = alloc;
    ctx->entries = NULL;
    ctx->count = 0;
    ctx->capacity = 0;

    vs.ctx = ctx;
    return vs;
}
