#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/memory/graph.h"
#include "human/memory/rerank.h"
#include "human/memory/retrieval.h"
#include "human/memory/vector.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HU_RRF_K 60.0f

/* Convert retrieval result to search results for RRF. Caller frees with hu_rerank_free_results. */
static hu_error_t entries_to_search_results(hu_allocator_t *alloc, const hu_memory_entry_t *entries,
                                            const double *scores, size_t count,
                                            hu_search_result_t *out) {
    for (size_t i = 0; i < count; i++) {
        const char *content =
            entries[i].content && entries[i].content_len > 0
                ? entries[i].content
                : (entries[i].key && entries[i].key_len > 0 ? entries[i].key : NULL);
        size_t len =
            content ? (entries[i].content && entries[i].content_len > 0 ? entries[i].content_len
                                                                        : entries[i].key_len)
                    : 0;
        out[i].content = content && len > 0 ? hu_strndup(alloc, content, len) : NULL;
        out[i].score = (float)(scores && i < count ? scores[i] : 0.0);
        out[i].rerank_score = 0.0f;
        out[i].original_rank = i;
    }
    return HU_OK;
}

/* Convert search results back to retrieval result. Allocates entries/scores. */
static hu_error_t search_results_to_entries(hu_allocator_t *alloc, const char *query,
                                            hu_search_result_t *results, size_t count, size_t limit,
                                            hu_retrieval_result_t *out) {
    out->entries = NULL;
    out->count = 0;
    out->scores = NULL;
    if (count == 0)
        return HU_OK;
    size_t n = count > limit ? limit : count;
    hu_memory_entry_t *entries =
        (hu_memory_entry_t *)alloc->alloc(alloc->ctx, n * sizeof(hu_memory_entry_t));
    double *scores = (double *)alloc->alloc(alloc->ctx, n * sizeof(double));
    if (!entries || !scores) {
        if (entries)
            alloc->free(alloc->ctx, entries, n * sizeof(hu_memory_entry_t));
        if (scores)
            alloc->free(alloc->ctx, scores, n * sizeof(double));
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(entries, 0, n * sizeof(hu_memory_entry_t));
    for (size_t i = 0; i < n; i++) {
        if (results[i].content) {
            size_t len = strlen(results[i].content);
            entries[i].content = hu_strndup(alloc, results[i].content, len);
            entries[i].content_len = len;
            entries[i].key = hu_strndup(alloc, results[i].content, len);
            entries[i].key_len = len;
            entries[i].id = entries[i].key;
            entries[i].id_len = len;
        }
        entries[i].score = (double)results[i].rerank_score;
        scores[i] = (double)results[i].rerank_score;
    }
    (void)query;
    out->entries = entries;
    out->count = n;
    out->scores = scores;
    return HU_OK;
}

hu_error_t hu_hybrid_retrieve(hu_allocator_t *alloc, hu_memory_t *backend, hu_embedder_t *embedder,
                              hu_vector_store_t *vector_store, hu_graph_t *graph,
                              const char *query, size_t query_len,
                              const hu_retrieval_options_t *opts, hu_retrieval_result_t *out) {
    out->entries = NULL;
    out->count = 0;
    out->scores = NULL;

    if (!alloc || !query || query_len == 0)
        return HU_OK;

    size_t limit = opts && opts->limit > 0 ? opts->limit : 10;

#ifndef HU_ENABLE_SQLITE
    (void)graph;
#endif

    hu_retrieval_result_t keyword_result = {0};
    hu_error_t err = hu_keyword_retrieve(alloc, backend, query, query_len, opts, &keyword_result);
    if (err != HU_OK)
        return err;

    /* Graph retrieval: add as extra source when graph is set */
#ifdef HU_ENABLE_SQLITE
    hu_retrieval_result_t graph_result = {0};
    if (graph) {
        char *graph_ctx = NULL;
        size_t graph_ctx_len = 0;
        if (hu_graph_build_context(graph, alloc, query, query_len, 2, 2048, &graph_ctx,
                                  &graph_ctx_len) == HU_OK &&
            graph_ctx && graph_ctx_len > 0) {
            graph_result.entries =
                (hu_memory_entry_t *)alloc->alloc(alloc->ctx, sizeof(hu_memory_entry_t));
            graph_result.scores = (double *)alloc->alloc(alloc->ctx, sizeof(double));
            if (graph_result.entries && graph_result.scores) {
                memset(graph_result.entries, 0, sizeof(hu_memory_entry_t));
                graph_result.entries[0].content = graph_ctx;
                graph_result.entries[0].content_len = graph_ctx_len;
                graph_result.entries[0].key = hu_strndup(alloc, "graph", 5);
                graph_result.entries[0].key_len = 5;
                graph_result.entries[0].id = graph_result.entries[0].key;
                graph_result.entries[0].id_len = 5;
                graph_result.entries[0].score = 0.9;
                graph_result.scores[0] = 0.9;
                graph_result.count = 1;
            } else {
                if (graph_result.entries)
                    alloc->free(alloc->ctx, graph_result.entries, sizeof(hu_memory_entry_t));
                if (graph_result.scores)
                    alloc->free(alloc->ctx, graph_result.scores, sizeof(double));
                alloc->free(alloc->ctx, graph_ctx, graph_ctx_len + 1);
            }
        }
    }
#endif

    bool has_vector = embedder && embedder->vtable && vector_store && vector_store->vtable;

    if (!has_vector) {
#ifdef HU_ENABLE_SQLITE
        if (graph_result.count > 0) {
            /* Merge keyword + graph */
            size_t kw_count = keyword_result.count;
            size_t gr_count = graph_result.count;
            size_t total = kw_count + gr_count;
            hu_memory_entry_t *merged =
                (hu_memory_entry_t *)alloc->alloc(alloc->ctx, total * sizeof(hu_memory_entry_t));
            double *scores = (double *)alloc->alloc(alloc->ctx, total * sizeof(double));
            if (merged && scores) {
                if (kw_count > 0) {
                    memcpy(merged, keyword_result.entries, kw_count * sizeof(hu_memory_entry_t));
                    memcpy(scores, keyword_result.scores, kw_count * sizeof(double));
                }
                memcpy(merged + kw_count, graph_result.entries,
                       gr_count * sizeof(hu_memory_entry_t));
                memcpy(scores + kw_count, graph_result.scores, gr_count * sizeof(double));
                hu_retrieval_result_free(alloc, &keyword_result);
                hu_retrieval_result_free(alloc, &graph_result);
                out->entries = merged;
                out->count = total;
                out->scores = scores;
                return HU_OK;
            }
            if (merged)
                alloc->free(alloc->ctx, merged, total * sizeof(hu_memory_entry_t));
            if (scores)
                alloc->free(alloc->ctx, scores, total * sizeof(double));
        }
#endif
        *out = keyword_result;
        return HU_OK;
    }

    hu_retrieval_result_t semantic_result = {0};
    err = hu_semantic_retrieve(alloc, embedder, vector_store, query, query_len, opts,
                               &semantic_result);
    if (err != HU_OK) {
        hu_retrieval_result_free(alloc, &keyword_result);
#ifdef HU_ENABLE_SQLITE
        hu_retrieval_result_free(alloc, &graph_result);
#endif
        return err;
    }

    /* Keyword, semantic, and optionally graph: merge with RRF, rerank with cross-encoder */
    size_t kw_count = keyword_result.count;
    size_t sem_count = semantic_result.count;
#ifdef HU_ENABLE_SQLITE
    size_t gr_count = graph_result.count;
#else
    size_t gr_count = 0;
#endif

    if (kw_count == 0 && sem_count == 0 && gr_count == 0) {
        return HU_OK;
    }

    size_t max_merged = (kw_count + sem_count + gr_count) > limit
                            ? (kw_count + sem_count + gr_count)
                            : limit;
    if (max_merged > 128)
        max_merged = 128;

    /* Include graph in keyword list for RRF (graph first so it gets rank 1) */
    size_t kw_total = kw_count + gr_count;
    hu_search_result_t *kw_sr =
        (hu_search_result_t *)alloc->alloc(alloc->ctx, kw_total * sizeof(hu_search_result_t));
    hu_search_result_t *sem_sr =
        (hu_search_result_t *)alloc->alloc(alloc->ctx, sem_count * sizeof(hu_search_result_t));
    hu_search_result_t *merged =
        (hu_search_result_t *)alloc->alloc(alloc->ctx, max_merged * sizeof(hu_search_result_t));

    if (!kw_sr || !sem_sr || !merged) {
        if (kw_sr)
            alloc->free(alloc->ctx, kw_sr, kw_total * sizeof(hu_search_result_t));
        if (sem_sr)
            alloc->free(alloc->ctx, sem_sr, sem_count * sizeof(hu_search_result_t));
        if (merged)
            alloc->free(alloc->ctx, merged, max_merged * sizeof(hu_search_result_t));
        hu_retrieval_result_free(alloc, &keyword_result);
        hu_retrieval_result_free(alloc, &semantic_result);
#ifdef HU_ENABLE_SQLITE
        hu_retrieval_result_free(alloc, &graph_result);
#endif
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(kw_sr, 0, kw_total * sizeof(hu_search_result_t));
    memset(sem_sr, 0, sem_count * sizeof(hu_search_result_t));
    memset(merged, 0, max_merged * sizeof(hu_search_result_t));

    hu_allocator_t sys = hu_system_allocator();
    size_t kw_fill = 0;
#ifdef HU_ENABLE_SQLITE
    if (gr_count > 0) {
        entries_to_search_results(&sys, graph_result.entries, graph_result.scores, gr_count, kw_sr);
        kw_fill = gr_count;
    }
#endif
    if (kw_count > 0)
        entries_to_search_results(&sys, keyword_result.entries, keyword_result.scores, kw_count,
                                  kw_sr + kw_fill);
    if (sem_count > 0)
        entries_to_search_results(&sys, semantic_result.entries, semantic_result.scores, sem_count,
                                  sem_sr);

    hu_retrieval_result_free(alloc, &keyword_result);
    hu_retrieval_result_free(alloc, &semantic_result);
#ifdef HU_ENABLE_SQLITE
    hu_retrieval_result_free(alloc, &graph_result);
#endif

    size_t merged_count = 0;
    err = hu_rerank_rrf(kw_sr, kw_total, sem_sr, sem_count, merged, max_merged, &merged_count,
                        (float)HU_RRF_K);
    hu_rerank_free_results(kw_sr, kw_total);
    hu_rerank_free_results(sem_sr, sem_count);
    alloc->free(alloc->ctx, kw_sr, kw_total * sizeof(hu_search_result_t));
    alloc->free(alloc->ctx, sem_sr, sem_count * sizeof(hu_search_result_t));

    if (err != HU_OK) {
        hu_rerank_free_results(merged, merged_count);
        alloc->free(alloc->ctx, merged, max_merged * sizeof(hu_search_result_t));
        return err;
    }

    err = hu_rerank_cross_encoder(query, merged, merged_count);
    if (err != HU_OK) {
        hu_rerank_free_results(merged, merged_count);
        alloc->free(alloc->ctx, merged, max_merged * sizeof(hu_search_result_t));
        return err;
    }

    err = search_results_to_entries(alloc, query, merged, merged_count, limit, out);
    hu_rerank_free_results(merged, merged_count);
    alloc->free(alloc->ctx, merged, max_merged * sizeof(hu_search_result_t));
    return err;
}
