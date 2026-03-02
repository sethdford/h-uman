#include "seaclaw/memory/retrieval/rrf.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include <stdlib.h>
#include <string.h>

/* Simple hash map: key -> (score, index in merged). Key is entry key. */
#define RRF_MAP_CAP 256
typedef struct rrf_map_node {
    char *key;
    size_t key_len;
    double score;
    size_t merged_idx;
    struct rrf_map_node *next;
} rrf_map_node_t;

typedef struct {
    rrf_map_node_t *buckets[RRF_MAP_CAP];
} rrf_map_t;

static unsigned hash_key(const char *k, size_t len) {
    unsigned h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)k[i];
        h *= 16777619u;
    }
    return h % RRF_MAP_CAP;
}

static rrf_map_node_t *map_find(rrf_map_t *m, const char *key, size_t key_len) {
    unsigned i = hash_key(key, key_len);
    for (rrf_map_node_t *n = m->buckets[i]; n; n = n->next)
        if (n->key_len == key_len && memcmp(n->key, key, key_len) == 0)
            return n;
    return NULL;
}

static rrf_map_node_t *map_put(rrf_map_t *m, sc_allocator_t *alloc,
    const char *key, size_t key_len, double score, size_t merged_idx) {
    unsigned i = hash_key(key, key_len);
    rrf_map_node_t *n = (rrf_map_node_t *)alloc->alloc(alloc->ctx, sizeof(rrf_map_node_t));
    if (!n) return NULL;
    n->key = sc_strndup(alloc, key, key_len);
    if (!n->key) {
        alloc->free(alloc->ctx, n, sizeof(rrf_map_node_t));
        return NULL;
    }
    n->key_len = key_len;
    n->score = score;
    n->merged_idx = merged_idx;
    n->next = m->buckets[i];
    m->buckets[i] = n;
    return n;
}

static void map_clear(rrf_map_t *m, sc_allocator_t *alloc) {
    for (int i = 0; i < RRF_MAP_CAP; i++) {
        rrf_map_node_t *n = m->buckets[i];
        while (n) {
            rrf_map_node_t *nx = n->next;
            if (n->key) alloc->free(alloc->ctx, n->key, n->key_len + 1);
            alloc->free(alloc->ctx, n, sizeof(rrf_map_node_t));
            n = nx;
        }
        m->buckets[i] = NULL;
    }
}

static sc_error_t entry_dup(sc_allocator_t *alloc, const sc_memory_entry_t *src,
    sc_memory_entry_t *dst, double score) {
    memset(dst, 0, sizeof(*dst));
    if (src->id && src->id_len > 0) {
        dst->id = sc_strndup(alloc, src->id, src->id_len);
        if (!dst->id) return SC_ERR_OUT_OF_MEMORY;
        dst->id_len = src->id_len;
    }
    if (src->key && src->key_len > 0) {
        dst->key = sc_strndup(alloc, src->key, src->key_len);
        if (!dst->key) { sc_memory_entry_free_fields(alloc, dst); return SC_ERR_OUT_OF_MEMORY; }
        dst->key_len = src->key_len;
    }
    if (src->content && src->content_len > 0) {
        dst->content = sc_strndup(alloc, src->content, src->content_len);
        if (!dst->content) { sc_memory_entry_free_fields(alloc, dst); return SC_ERR_OUT_OF_MEMORY; }
        dst->content_len = src->content_len;
    }
    dst->category = src->category;
    if (src->category.data.custom.name && src->category.data.custom.name_len > 0) {
        dst->category.data.custom.name = sc_strndup(alloc, src->category.data.custom.name, src->category.data.custom.name_len);
        if (!dst->category.data.custom.name) { sc_memory_entry_free_fields(alloc, dst); return SC_ERR_OUT_OF_MEMORY; }
        dst->category.data.custom.name_len = src->category.data.custom.name_len;
    }
    if (src->timestamp && src->timestamp_len > 0) {
        dst->timestamp = sc_strndup(alloc, src->timestamp, src->timestamp_len);
        if (!dst->timestamp) { sc_memory_entry_free_fields(alloc, dst); return SC_ERR_OUT_OF_MEMORY; }
        dst->timestamp_len = src->timestamp_len;
    }
    if (src->session_id && src->session_id_len > 0) {
        dst->session_id = sc_strndup(alloc, src->session_id, src->session_id_len);
        if (!dst->session_id) { sc_memory_entry_free_fields(alloc, dst); return SC_ERR_OUT_OF_MEMORY; }
        dst->session_id_len = src->session_id_len;
    }
    dst->score = score;
    return SC_OK;
}

sc_error_t sc_rrf_merge(sc_allocator_t *alloc,
    const sc_memory_entry_t **source_lists, const size_t *source_lens, size_t num_sources,
    unsigned k, size_t limit,
    sc_memory_entry_t **out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;
    if (!alloc || !source_lists || !source_lens) return SC_ERR_INVALID_ARGUMENT;

    if (num_sources == 0) return SC_OK;

    /* Single source: passthrough with RRF scores */
    if (num_sources == 1) {
        size_t n = source_lens[0];
        if (n > limit) n = limit;
        if (n == 0) return SC_OK;
        const sc_memory_entry_t *src = source_lists[0];
        sc_memory_entry_t *res = (sc_memory_entry_t *)alloc->alloc(alloc->ctx, n * sizeof(sc_memory_entry_t));
        if (!res) return SC_ERR_OUT_OF_MEMORY;
        for (size_t i = 0; i < n; i++) {
            double rrf_score = 1.0 / (double)(i + 1 + k);
            if (entry_dup(alloc, &src[i], &res[i], rrf_score) != SC_OK) {
                for (size_t j = 0; j < i; j++) sc_memory_entry_free_fields(alloc, &res[j]);
                alloc->free(alloc->ctx, res, n * sizeof(sc_memory_entry_t));
                return SC_ERR_OUT_OF_MEMORY;
            }
        }
        *out = res;
        *out_count = n;
        return SC_OK;
    }

    rrf_map_t map = {{0}};
    sc_memory_entry_t *merged = NULL;
    size_t merged_cap = 128, merged_count = 0;
    double *scores = NULL;
    size_t scores_cap = 128;

    merged = (sc_memory_entry_t *)alloc->alloc(alloc->ctx, merged_cap * sizeof(sc_memory_entry_t));
    scores = (double *)alloc->alloc(alloc->ctx, scores_cap * sizeof(double));
    if (!merged || !scores) {
        if (merged) alloc->free(alloc->ctx, merged, merged_cap * sizeof(sc_memory_entry_t));
        if (scores) alloc->free(alloc->ctx, scores, scores_cap * sizeof(double));
        return SC_ERR_OUT_OF_MEMORY;
    }

    for (size_t s = 0; s < num_sources; s++) {
        const sc_memory_entry_t *list = source_lists[s];
        size_t len = source_lens[s];
        for (size_t i = 0; i < len; i++) {
            const sc_memory_entry_t *c = &list[i];
            const char *key = c->key && c->key_len > 0 ? c->key : c->id;
            size_t key_len = c->key && c->key_len > 0 ? c->key_len : (c->id ? c->id_len : 0);
            if (!key || key_len == 0) continue;

            double rrf_term = 1.0 / (double)(i + 1 + k);
            rrf_map_node_t *node = map_find(&map, key, key_len);
            if (node) {
                node->score += rrf_term;
            } else {
                if (merged_count >= merged_cap) {
                    merged_cap *= 2;
                    scores_cap *= 2;
                    sc_memory_entry_t *nm = (sc_memory_entry_t *)alloc->realloc(alloc->ctx, merged,
                        merged_count * sizeof(sc_memory_entry_t), merged_cap * sizeof(sc_memory_entry_t));
                    double *ns = (double *)alloc->realloc(alloc->ctx, scores,
                        merged_count * sizeof(double), scores_cap * sizeof(double));
                    if (!nm || !ns) break;
                    merged = nm;
                    scores = ns;
                }
                if (entry_dup(alloc, c, &merged[merged_count], 0.0) != SC_OK) break;
                scores[merged_count] = rrf_term;
                if (!map_put(&map, alloc, key, key_len, rrf_term, merged_count)) break;
                merged_count++;
            }
        }
    }

    /* Update scores from map */
    for (size_t i = 0; i < merged_count; i++) {
        const char *key = merged[i].key && merged[i].key_len > 0 ? merged[i].key : merged[i].id;
        size_t key_len = merged[i].key && merged[i].key_len > 0 ? merged[i].key_len : (merged[i].id ? merged[i].id_len : 0);
        if (key && key_len > 0) {
            rrf_map_node_t *node = map_find(&map, key, key_len);
            if (node) merged[i].score = node->score;
        }
    }
    map_clear(&map, alloc);

    /* Sort by score descending */
    for (size_t i = 0; i < merged_count; i++) {
        for (size_t j = i + 1; j < merged_count; j++) {
            if (scores[j] > scores[i] || (scores[j] == scores[i] && merged[j].score > merged[i].score)) {
                sc_memory_entry_t te = merged[i];
                merged[i] = merged[j];
                merged[j] = te;
                double td = scores[i];
                scores[i] = scores[j];
                scores[j] = td;
            }
        }
    }

    size_t out_len = merged_count > limit ? limit : merged_count;
    if (out_len < merged_count) {
        for (size_t i = out_len; i < merged_count; i++)
            sc_memory_entry_free_fields(alloc, &merged[i]);
    }
    alloc->free(alloc->ctx, scores, scores_cap * sizeof(double));
    *out = merged;
    *out_count = out_len;
    return SC_OK;
}

void sc_rrf_free_result(sc_allocator_t *alloc, sc_memory_entry_t *entries, size_t count) {
    if (!alloc || !entries) return;
    for (size_t i = 0; i < count; i++)
        sc_memory_entry_free_fields(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
}
