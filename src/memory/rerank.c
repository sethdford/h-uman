/*
 * Hybrid vector search reranking: RRF merge + cross-encoder term-overlap.
 * No external dependencies.
 */
#include "human/memory/rerank.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define RRF_K_DEFAULT 60.0f

/* Hash map for RRF: content -> (rrf_score, index in merged). */
#define RRF_MAP_CAP 128
typedef struct rrf_node {
    char *content;
    size_t content_len;
    float rrf_score;
    size_t merged_idx;
    float orig_score;
    size_t orig_rank;
    struct rrf_node *next;
} rrf_node_t;

static unsigned hash_content(const char *c, size_t len) {
    unsigned h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)c[i];
        h *= 16777619u;
    }
    return h % RRF_MAP_CAP;
}

static rrf_node_t *map_find(rrf_node_t **buckets, const char *content, size_t content_len) {
    unsigned i = hash_content(content, content_len);
    for (rrf_node_t *n = buckets[i]; n; n = n->next)
        if (n->content_len == content_len && memcmp(n->content, content, content_len) == 0)
            return n;
    return NULL;
}

static rrf_node_t *map_put(rrf_node_t **buckets, hu_allocator_t *alloc, const char *content,
                           size_t content_len, float rrf_term, size_t merged_idx, float orig_score,
                           size_t orig_rank) {
    unsigned i = hash_content(content, content_len);
    rrf_node_t *n = (rrf_node_t *)alloc->alloc(alloc->ctx, sizeof(rrf_node_t));
    if (!n)
        return NULL;
    n->content = hu_strndup(alloc, content, content_len);
    if (!n->content) {
        alloc->free(alloc->ctx, n, sizeof(rrf_node_t));
        return NULL;
    }
    n->content_len = content_len;
    n->rrf_score = rrf_term;
    n->merged_idx = merged_idx;
    n->orig_score = orig_score;
    n->orig_rank = orig_rank;
    n->next = buckets[i];
    buckets[i] = n;
    return n;
}

static void map_clear(rrf_node_t **buckets, hu_allocator_t *alloc) {
    for (int i = 0; i < RRF_MAP_CAP; i++) {
        rrf_node_t *n = buckets[i];
        while (n) {
            rrf_node_t *nx = n->next;
            if (n->content)
                alloc->free(alloc->ctx, n->content, n->content_len + 1);
            alloc->free(alloc->ctx, n, sizeof(rrf_node_t));
            n = nx;
        }
        buckets[i] = NULL;
    }
}

static hu_error_t rrf_merge_impl(hu_allocator_t *alloc, hu_search_result_t *keyword_results,
                                 size_t keyword_count, hu_search_result_t *vector_results,
                                 size_t vector_count, hu_search_result_t *merged_out,
                                 size_t max_results, size_t *merged_count, float k) {
    if (!alloc || !merged_count)
        return HU_ERR_INVALID_ARGUMENT;
    *merged_count = 0;

    if (keyword_count == 0 && vector_count == 0)
        return HU_OK;

    float k_val = (k > 0.0f) ? k : RRF_K_DEFAULT;

    rrf_node_t *buckets[RRF_MAP_CAP];
    memset(buckets, 0, sizeof(buckets));

    hu_search_result_t *merged = merged_out;
    size_t merged_cap = max_results;
    size_t merged_n = 0;

    /* Process keyword list */
    for (size_t i = 0; i < keyword_count; i++) {
        hu_search_result_t *r = &keyword_results[i];
        if (!r->content)
            continue;
        size_t len = strlen(r->content);
        float rrf_term = 1.0f / (k_val + (float)(i + 1));
        rrf_node_t *node = map_find(buckets, r->content, len);
        if (node) {
            node->rrf_score += rrf_term;
        } else {
            if (merged_n < merged_cap) {
                merged[merged_n].content = hu_strdup(alloc, r->content);
                if (!merged[merged_n].content) {
                    map_clear(buckets, alloc);
                    for (size_t j = 0; j < merged_n; j++)
                        alloc->free(alloc->ctx, merged[j].content, strlen(merged[j].content) + 1);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                merged[merged_n].score = r->score;
                merged[merged_n].rerank_score = rrf_term;
                merged[merged_n].original_rank = i;
                if (!map_put(buckets, alloc, r->content, len, rrf_term, merged_n, r->score, i)) {
                    map_clear(buckets, alloc);
                    for (size_t j = 0; j <= merged_n; j++)
                        alloc->free(alloc->ctx, merged[j].content, strlen(merged[j].content) + 1);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                merged_n++;
            }
        }
    }

    /* Process vector list */
    for (size_t i = 0; i < vector_count; i++) {
        hu_search_result_t *r = &vector_results[i];
        if (!r->content)
            continue;
        size_t len = strlen(r->content);
        float rrf_term = 1.0f / (k_val + (float)(i + 1));
        rrf_node_t *node = map_find(buckets, r->content, len);
        if (node) {
            node->rrf_score += rrf_term;
        } else {
            if (merged_n < merged_cap) {
                merged[merged_n].content = hu_strdup(alloc, r->content);
                if (!merged[merged_n].content) {
                    map_clear(buckets, alloc);
                    for (size_t j = 0; j < merged_n; j++)
                        alloc->free(alloc->ctx, merged[j].content, strlen(merged[j].content) + 1);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                merged[merged_n].score = r->score;
                merged[merged_n].rerank_score = rrf_term;
                merged[merged_n].original_rank = i;
                if (!map_put(buckets, alloc, r->content, len, rrf_term, merged_n, r->score, i)) {
                    map_clear(buckets, alloc);
                    for (size_t j = 0; j <= merged_n; j++)
                        alloc->free(alloc->ctx, merged[j].content, strlen(merged[j].content) + 1);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                merged_n++;
            }
        }
    }

    /* Update rrf scores from map */
    for (size_t i = 0; i < merged_n; i++) {
        if (merged[i].content) {
            rrf_node_t *node = map_find(buckets, merged[i].content, strlen(merged[i].content));
            if (node)
                merged[i].rerank_score = node->rrf_score;
        }
    }
    map_clear(buckets, alloc);

    /* Sort by rerank_score descending */
    for (size_t i = 0; i < merged_n; i++) {
        for (size_t j = i + 1; j < merged_n; j++) {
            if (merged[j].rerank_score > merged[i].rerank_score) {
                hu_search_result_t tmp = merged[i];
                merged[i] = merged[j];
                merged[j] = tmp;
            }
        }
    }

    *merged_count = merged_n;
    return HU_OK;
}

hu_error_t hu_rerank_rrf(hu_search_result_t *keyword_results, size_t keyword_count,
                         hu_search_result_t *vector_results, size_t vector_count,
                         hu_search_result_t *merged_out, size_t max_results, size_t *merged_count,
                         float k) {
    hu_allocator_t sys = hu_system_allocator();
    return rrf_merge_impl(&sys, keyword_results, keyword_count, vector_results, vector_count,
                          merged_out, max_results, merged_count, k);
}

/* Tokenize into words (lowercase, alnum+underscore) */
static size_t tokenize(const char *text, size_t text_len, char **words, size_t max_words,
                       hu_allocator_t *alloc) {
    size_t n = 0;
    const char *p = text;
    const char *end = text + text_len;
    while (p < end && n < max_words) {
        while (p < end && !isalnum((unsigned char)*p) && *p != '_')
            p++;
        if (p >= end)
            break;
        const char *start = p;
        while (p < end && (isalnum((unsigned char)*p) || *p == '_'))
            p++;
        size_t len = (size_t)(p - start);
        if (len > 0) {
            words[n] = hu_strndup(alloc, start, len);
            if (!words[n])
                return n;
            for (size_t i = 0; i < len; i++)
                words[n][i] = (char)(unsigned char)tolower((unsigned char)words[n][i]);
            n++;
        }
    }
    return n;
}

hu_error_t hu_rerank_cross_encoder(const char *query, hu_search_result_t *results, size_t count) {
    if (!query || count == 0)
        return HU_OK;

    hu_allocator_t alloc = hu_system_allocator();
    size_t query_len = strlen(query);

    char *query_words[256];
    size_t nq = tokenize(query, query_len, query_words, 256, &alloc);
    if (nq == 0) {
        return HU_OK;
    }

    for (size_t i = 0; i < count; i++) {
        if (!results[i].content)
            continue;
        size_t doc_len = strlen(results[i].content);
        char *doc_words[256];
        size_t nd = tokenize(results[i].content, doc_len, doc_words, 256, &alloc);

        size_t match = 0;
        for (size_t q = 0; q < nq; q++) {
            for (size_t d = 0; d < nd; d++) {
                if (strcmp(query_words[q], doc_words[d]) == 0) {
                    match++;
                    break;
                }
            }
        }
        results[i].rerank_score = (nq > 0) ? (float)match / (float)nq : 0.0f;

        for (size_t d = 0; d < nd; d++)
            alloc.free(alloc.ctx, doc_words[d], strlen(doc_words[d]) + 1);
    }

    for (size_t q = 0; q < nq; q++)
        alloc.free(alloc.ctx, query_words[q], strlen(query_words[q]) + 1);

    /* Sort by rerank_score descending */
    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            if (results[j].rerank_score > results[i].rerank_score) {
                hu_search_result_t tmp = results[i];
                results[i] = results[j];
                results[j] = tmp;
            }
        }
    }

    return HU_OK;
}

void hu_rerank_free_results(hu_search_result_t *results, size_t count) {
    if (!results)
        return;
    hu_allocator_t alloc = hu_system_allocator();
    for (size_t i = 0; i < count; i++) {
        if (results[i].content) {
            alloc.free(alloc.ctx, results[i].content, strlen(results[i].content) + 1);
            results[i].content = NULL;
        }
    }
}
