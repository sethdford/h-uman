#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/memory/graph.h"
#include "human/memory/retrieval.h"
#include "human/memory/vector.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct hu_retrieval_engine_ctx {
    hu_allocator_t *alloc;
    hu_memory_t backend;
    hu_embedder_t *embedder;
    hu_vector_store_t *vector_store;
    hu_graph_t *graph;
} hu_retrieval_engine_ctx_t;

hu_error_t hu_semantic_retrieve(hu_allocator_t *alloc, hu_embedder_t *embedder,
                                hu_vector_store_t *vector_store, const char *query,
                                size_t query_len, const hu_retrieval_options_t *opts,
                                hu_retrieval_result_t *out);

static hu_error_t impl_retrieve(void *ctx, hu_allocator_t *alloc, const char *query,
                                size_t query_len, const hu_retrieval_options_t *opts,
                                hu_retrieval_result_t *out) {
    hu_retrieval_engine_ctx_t *e = (hu_retrieval_engine_ctx_t *)ctx;
    out->entries = NULL;
    out->count = 0;
    out->scores = NULL;

    if (!query || !opts)
        return HU_ERR_INVALID_ARGUMENT;

    hu_retrieval_options_t o = *opts;
    if (o.limit == 0)
        o.limit = 10;

    hu_error_t err;
    switch (o.mode) {
    case HU_RETRIEVAL_KEYWORD:
        err = hu_keyword_retrieve(alloc, &e->backend, query, query_len, &o, out);
        break;
    case HU_RETRIEVAL_SEMANTIC:
        if (!e->embedder || !e->vector_store)
            return HU_ERR_NOT_SUPPORTED;
        err = hu_semantic_retrieve(alloc, e->embedder, e->vector_store, query, query_len, &o, out);
        break;
    case HU_RETRIEVAL_HYBRID:
        err = hu_hybrid_retrieve(alloc, &e->backend, e->embedder, e->vector_store, e->graph, query,
                                 query_len, &o, out);
        break;
    default:
        err = hu_keyword_retrieve(alloc, &e->backend, query, query_len, &o, out);
        break;
    }
    if (err != HU_OK || out->count == 0)
        return err;

    /* Apply temporal decay */
    if (o.temporal_decay_factor > 0.0) {
        for (size_t i = 0; i < out->count; i++) {
            hu_memory_entry_t *ent = &out->entries[i];
            out->scores[i] = hu_temporal_decay_score(out->scores[i], o.temporal_decay_factor,
                                                     ent->timestamp, ent->timestamp_len);
        }
        /* Re-sort by score after decay */
        for (size_t i = 0; i < out->count; i++) {
            for (size_t j = i + 1; j < out->count; j++) {
                if (out->scores[j] > out->scores[i]) {
                    hu_memory_entry_t te = out->entries[i];
                    double ts = out->scores[i];
                    out->entries[i] = out->entries[j];
                    out->scores[i] = out->scores[j];
                    out->entries[j] = te;
                    out->scores[j] = ts;
                }
            }
        }
    }

    /* Apply MMR reranking if requested */
    if (o.use_reranking && out->count > 1) {
        err = hu_mmr_rerank(alloc, query, query_len, out->entries, out->scores, out->count, 0.7);
        if (err != HU_OK) {
            hu_retrieval_result_free(alloc, out);
            return err;
        }
    }

    return HU_OK;
}

hu_error_t hu_semantic_retrieve(hu_allocator_t *alloc, hu_embedder_t *embedder,
                                hu_vector_store_t *vector_store, const char *query,
                                size_t query_len, const hu_retrieval_options_t *opts,
                                hu_retrieval_result_t *out) {
    out->entries = NULL;
    out->count = 0;
    out->scores = NULL;
    if (!embedder || !embedder->vtable || !vector_store || !vector_store->vtable)
        return HU_ERR_INVALID_ARGUMENT;

    hu_embedding_t query_embedding = {0};
    hu_error_t err =
        embedder->vtable->embed(embedder->ctx, alloc, query, query_len, &query_embedding);
    if (err != HU_OK)
        return err;

    size_t limit = opts && opts->limit > 0 ? opts->limit : 10;
    hu_vector_entry_t *results = NULL;
    size_t count = 0;
    err = vector_store->vtable->search(vector_store->ctx, alloc, &query_embedding, limit, &results,
                                       &count);
    hu_embedding_free(alloc, &query_embedding);
    if (err != HU_OK)
        return err;
    if (!results || count == 0)
        return HU_OK;

    double min_score = opts && opts->min_score >= 0.0 ? opts->min_score : 0.0;
    hu_memory_entry_t *entries =
        (hu_memory_entry_t *)alloc->alloc(alloc->ctx, count * sizeof(hu_memory_entry_t));
    double *scores = (double *)alloc->alloc(alloc->ctx, count * sizeof(double));
    if (!entries || !scores) {
        if (entries)
            alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
        if (scores)
            alloc->free(alloc->ctx, scores, count * sizeof(double));
        hu_vector_entries_free(alloc, results, count);
        return HU_ERR_OUT_OF_MEMORY;
    }

    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        if ((float)min_score > 0.0f && results[i].score < (float)min_score)
            continue;
        memset(&entries[n], 0, sizeof(entries[n]));
        entries[n].key = hu_strndup(alloc, results[i].id, results[i].id_len);
        entries[n].key_len = results[i].id_len;
        entries[n].id = entries[n].key;
        entries[n].id_len = entries[n].key_len;
        if (results[i].content && results[i].content_len > 0) {
            entries[n].content = hu_strndup(alloc, results[i].content, results[i].content_len);
            entries[n].content_len = results[i].content_len;
        }
        entries[n].score = (double)results[i].score;
        scores[n] = (double)results[i].score;
        n++;
    }
    hu_vector_entries_free(alloc, results, count);

    if (n == 0) {
        alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
        alloc->free(alloc->ctx, scores, count * sizeof(double));
        return HU_OK;
    }
    if (n < count) {
        hu_memory_entry_t *trim_e = (hu_memory_entry_t *)alloc->realloc(
            alloc->ctx, entries, count * sizeof(hu_memory_entry_t), n * sizeof(hu_memory_entry_t));
        double *trim_s = (double *)alloc->realloc(alloc->ctx, scores, count * sizeof(double),
                                                  n * sizeof(double));
        if (trim_e)
            entries = trim_e;
        if (trim_s)
            scores = trim_s;
    }
    out->entries = entries;
    out->count = n;
    out->scores = scores;
    return HU_OK;
}

static void impl_deinit(void *ctx, hu_allocator_t *alloc) {
    hu_retrieval_engine_ctx_t *e = (hu_retrieval_engine_ctx_t *)ctx;
    /* Backend is owned by caller, we do not deinit it */
    (void)e;
    alloc->free(alloc->ctx, e, sizeof(hu_retrieval_engine_ctx_t));
}

static const hu_retrieval_vtable_t engine_vtable = {
    .retrieve = impl_retrieve,
    .deinit = impl_deinit,
};

hu_retrieval_engine_t hu_retrieval_create(hu_allocator_t *alloc, hu_memory_t *backend) {
    return hu_retrieval_create_with_vector(alloc, backend, NULL, NULL);
}

hu_retrieval_engine_t hu_retrieval_create_with_vector(hu_allocator_t *alloc, hu_memory_t *backend,
                                                      hu_embedder_t *embedder,
                                                      hu_vector_store_t *vector_store) {
    if (!alloc || !backend || !backend->ctx || !backend->vtable)
        return (hu_retrieval_engine_t){.ctx = NULL, .vtable = NULL};

    hu_retrieval_engine_ctx_t *ctx =
        (hu_retrieval_engine_ctx_t *)alloc->alloc(alloc->ctx, sizeof(hu_retrieval_engine_ctx_t));
    if (!ctx)
        return (hu_retrieval_engine_t){.ctx = NULL, .vtable = NULL};

    ctx->alloc = alloc;
    ctx->backend = *backend;
    ctx->embedder = embedder;
    ctx->vector_store = vector_store;
    ctx->graph = NULL;
    return (hu_retrieval_engine_t){
        .ctx = ctx,
        .vtable = &engine_vtable,
    };
}

hu_error_t hu_retrieval_index_entry(hu_retrieval_engine_t *engine, hu_allocator_t *alloc,
                                    const char *key, size_t key_len, const char *content,
                                    size_t content_len) {
    if (!engine || !engine->ctx || !engine->vtable || !alloc)
        return HU_ERR_INVALID_ARGUMENT;
    hu_retrieval_engine_ctx_t *e = (hu_retrieval_engine_ctx_t *)engine->ctx;
    if (!e->embedder || !e->embedder->vtable || !e->vector_store || !e->vector_store->vtable)
        return HU_ERR_NOT_SUPPORTED;

    hu_embedding_t emb = {0};
    hu_error_t err =
        e->embedder->vtable->embed(e->embedder->ctx, alloc, content, content_len, &emb);
    if (err != HU_OK)
        return err;

    err = e->vector_store->vtable->insert(e->vector_store->ctx, alloc, key, key_len, &emb, content,
                                          content_len);
    hu_embedding_free(alloc, &emb);
    return err;
}

void hu_retrieval_set_graph(hu_retrieval_engine_t *engine, hu_graph_t *graph) {
    if (!engine || !engine->ctx)
        return;
    hu_retrieval_engine_ctx_t *e = (hu_retrieval_engine_ctx_t *)engine->ctx;
    e->graph = graph;
}

void hu_retrieval_result_free(hu_allocator_t *alloc, hu_retrieval_result_t *r) {
    if (!alloc || !r)
        return;
    for (size_t i = 0; i < r->count; i++)
        hu_memory_entry_free_fields(alloc, &r->entries[i]);
    if (r->entries)
        alloc->free(alloc->ctx, r->entries, r->count * sizeof(hu_memory_entry_t));
    if (r->scores)
        alloc->free(alloc->ctx, r->scores, r->count * sizeof(double));
    r->entries = NULL;
    r->count = 0;
    r->scores = NULL;
}
