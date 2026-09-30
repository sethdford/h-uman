/*
 * Hybrid vector search reranking: RRF merge, score-level fusion, and the
 * cross-encoder term-overlap rerank.
 * No external dependencies.
 */
#include "human/memory/rerank.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
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

/* Release the owned strings of one result row (content + key). */
static void result_free_strings(hu_allocator_t *alloc, hu_search_result_t *r) {
    if (r->content) {
        alloc->free(alloc->ctx, r->content, strlen(r->content) + 1);
        r->content = NULL;
    }
    if (r->key) {
        alloc->free(alloc->ctx, r->key, r->key_len + 1);
        r->key = NULL;
    }
    r->key_len = 0;
}

/* Copy one source row into merged[idx]: content is required, key travels
 * with it when present. Returns false on OOM with nothing left allocated. */
static bool result_copy_strings(hu_allocator_t *alloc, const hu_search_result_t *src,
                                hu_search_result_t *dst) {
    dst->content = hu_strdup(alloc, src->content);
    if (!dst->content)
        return false;
    dst->key = NULL;
    dst->key_len = 0;
    if (src->key && src->key_len > 0) {
        dst->key = hu_strndup(alloc, src->key, src->key_len);
        if (!dst->key) {
            alloc->free(alloc->ctx, dst->content, strlen(dst->content) + 1);
            dst->content = NULL;
            return false;
        }
        dst->key_len = src->key_len;
    }
    return true;
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
                if (!result_copy_strings(alloc, r, &merged[merged_n])) {
                    map_clear(buckets, alloc);
                    for (size_t j = 0; j < merged_n; j++)
                        result_free_strings(alloc, &merged[j]);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                merged[merged_n].score = r->score;
                merged[merged_n].rerank_score = rrf_term;
                merged[merged_n].original_rank = i;
                if (!map_put(buckets, alloc, r->content, len, rrf_term, merged_n, r->score, i)) {
                    map_clear(buckets, alloc);
                    for (size_t j = 0; j <= merged_n; j++)
                        result_free_strings(alloc, &merged[j]);
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
                if (!result_copy_strings(alloc, r, &merged[merged_n])) {
                    map_clear(buckets, alloc);
                    for (size_t j = 0; j < merged_n; j++)
                        result_free_strings(alloc, &merged[j]);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                merged[merged_n].score = r->score;
                merged[merged_n].rerank_score = rrf_term;
                merged[merged_n].original_rank = i;
                if (!map_put(buckets, alloc, r->content, len, rrf_term, merged_n, r->score, i)) {
                    map_clear(buckets, alloc);
                    for (size_t j = 0; j <= merged_n; j++)
                        result_free_strings(alloc, &merged[j]);
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

/* ── Score-level fusion (arXiv 2606.04194) ─────────────────────────────── */

/* One distinct row (by content) across both legs. The normalised leg scores
 * start at 0, which is also what an absent leg contributes. */
typedef struct fusion_cand {
    const hu_search_result_t *src; /* the row's first appearance */
    size_t first_seen;             /* keyword index, or kw_count + dense index */
    size_t leg_rank;               /* index within the leg of first appearance */
    float lex;
    float dense;
    float rrf; /* tie-break: the score hu_rerank_rrf would give (k=60) */
    float fused;
} fusion_cand_t;

/* Per-call min-max over the leg's rows that carry content. A flat leg (or a
 * non-finite score) maps to 1.0 / 0.0 so the fused score stays in [0,1]. */
static float fusion_normalise(float s, float lo, float hi) {
    if (!isfinite(s))
        return 0.0f;
    if (!(hi > lo))
        return 1.0f;
    float v = (s - lo) / (hi - lo);
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static size_t fusion_add_leg(fusion_cand_t *cands, size_t n, const hu_search_result_t *rows,
                             size_t rows_n, bool is_dense, size_t seen_base) {
    float lo = INFINITY, hi = -INFINITY;
    for (size_t i = 0; i < rows_n; i++) {
        if (!rows[i].content || !isfinite(rows[i].score))
            continue;
        lo = rows[i].score < lo ? rows[i].score : lo;
        hi = rows[i].score > hi ? rows[i].score : hi;
    }
    for (size_t i = 0; i < rows_n; i++) {
        if (!rows[i].content)
            continue;
        size_t j = 0;
        while (j < n && strcmp(cands[j].src->content, rows[i].content) != 0)
            j++;
        if (j == n) {
            memset(&cands[n], 0, sizeof(cands[n]));
            cands[n].src = &rows[i];
            cands[n].first_seen = seen_base + i;
            cands[n].leg_rank = i;
            n++;
        }
        float norm = fusion_normalise(rows[i].score, lo, hi);
        float *slot = is_dense ? &cands[j].dense : &cands[j].lex;
        if (norm > *slot)
            *slot = norm;
        cands[j].rrf += 1.0f / (RRF_K_DEFAULT + (float)(i + 1));
    }
    return n;
}

/* Total order: fused desc, then RRF desc, then first appearance asc. */
static bool fusion_before(const fusion_cand_t *a, const fusion_cand_t *b) {
    if (a->fused != b->fused)
        return a->fused > b->fused;
    if (a->rrf != b->rrf)
        return a->rrf > b->rrf;
    return a->first_seen < b->first_seen;
}

hu_error_t hu_rerank_score_fusion(hu_search_result_t *keyword_results, size_t keyword_count,
                                  hu_search_result_t *vector_results, size_t vector_count,
                                  hu_search_result_t *merged_out, size_t max_results,
                                  size_t *merged_count, float alpha) {
    if (!merged_count)
        return HU_ERR_INVALID_ARGUMENT;
    *merged_count = 0;
    if (!(alpha >= 0.0f && alpha <= 1.0f)) /* also rejects NaN */
        return HU_ERR_INVALID_ARGUMENT;
    if ((keyword_count > 0 && !keyword_results) || (vector_count > 0 && !vector_results) ||
        (max_results > 0 && !merged_out))
        return HU_ERR_INVALID_ARGUMENT;
    size_t total = keyword_count + vector_count;
    if (total == 0 || max_results == 0)
        return HU_OK;

    hu_allocator_t sys = hu_system_allocator();
    fusion_cand_t *cands = (fusion_cand_t *)sys.alloc(sys.ctx, total * sizeof(fusion_cand_t));
    if (!cands)
        return HU_ERR_OUT_OF_MEMORY;
    size_t n = fusion_add_leg(cands, 0, keyword_results, keyword_count, false, 0);
    n = fusion_add_leg(cands, n, vector_results, vector_count, true, keyword_count);

    for (size_t i = 0; i < n; i++)
        cands[i].fused = alpha * cands[i].dense + (1.0f - alpha) * cands[i].lex;
    for (size_t i = 1; i < n; i++) { /* insertion sort; n is at most a few hundred */
        fusion_cand_t cur = cands[i];
        size_t j = i;
        while (j > 0 && fusion_before(&cur, &cands[j - 1])) {
            cands[j] = cands[j - 1];
            j--;
        }
        cands[j] = cur;
    }

    size_t out_n = n < max_results ? n : max_results;
    hu_error_t err = HU_OK;
    for (size_t i = 0; i < out_n; i++) {
        const fusion_cand_t *c = &cands[i];
        if (!result_copy_strings(&sys, c->src, &merged_out[i])) {
            for (size_t k = 0; k < i; k++)
                result_free_strings(&sys, &merged_out[k]);
            err = HU_ERR_OUT_OF_MEMORY;
            out_n = 0;
            break;
        }
        merged_out[i].score = c->src->score;
        merged_out[i].rerank_score = c->fused;
        merged_out[i].original_rank = c->leg_rank;
        merged_out[i].candidate_idx = c->src->candidate_idx;
    }
    sys.free(sys.ctx, cands, total * sizeof(fusion_cand_t));
    *merged_count = out_n;
    return err;
}

void hu_rerank_bm25_to_relevance(const double *engine_scores, const double *boosts, size_t count,
                                 double *out) {
    if (!engine_scores || !out)
        return;
    double prev_base = 0.0;
    for (size_t i = 0; i < count; i++) {
        /* Read before write: out may alias engine_scores. */
        double b = (boosts && isfinite(boosts[i])) ? boosts[i] : 0.0;
        double base =
            isfinite(engine_scores[i]) ? -(engine_scores[i] - b) : (i > 0 ? prev_base : 0.0);
        if (i > 0 && base > prev_base)
            base = prev_base;
        prev_base = base;
        out[i] = base + b;
    }
}

hu_hybrid_fusion_t hu_hybrid_fusion_mode(void) {
    static atomic_bool announced = false;
    static atomic_bool warned = false;
    const char *v = getenv("HU_HYBRID_FUSION");
    if (!v || !v[0] || strcmp(v, "rrf") == 0)
        return HU_HYBRID_FUSION_RRF;
    if (strcmp(v, "score") == 0) {
        hu_log_info_once(&announced, "hybrid_fusion", NULL,
                         "HU_HYBRID_FUSION=score: plain hybrid merge fuses min-max normalised "
                         "BM25 and cosine scores (dense weight HU_HYBRID_FUSION_ALPHA)");
        return HU_HYBRID_FUSION_SCORE;
    }
    hu_log_warn_once(&warned, "hybrid_fusion", NULL,
                     "HU_HYBRID_FUSION=%s is not rrf|score; using rrf", v);
    return HU_HYBRID_FUSION_RRF;
}

float hu_hybrid_fusion_alpha(void) {
    static atomic_bool warned = false;
    const char *v = getenv("HU_HYBRID_FUSION_ALPHA");
    if (!v || !v[0])
        return HU_HYBRID_FUSION_ALPHA_DEFAULT;
    char *end = NULL;
    float a = strtof(v, &end);
    if (end == v || *end != '\0' || !isfinite(a) || a < 0.0f || a > 1.0f) {
        hu_log_warn_once(&warned, "hybrid_fusion", NULL,
                         "HU_HYBRID_FUSION_ALPHA=%s is not a number in [0,1]; using %.2f", v,
                         (double)HU_HYBRID_FUSION_ALPHA_DEFAULT);
        return HU_HYBRID_FUSION_ALPHA_DEFAULT;
    }
    return a;
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
    for (size_t i = 0; i < count; i++)
        result_free_strings(&alloc, &results[i]);
}
