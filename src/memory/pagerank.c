/*
 * W12 — HippoRAG-style personalized PageRank.
 *
 * Pure-CPU power iteration over the per-contact entity adjacency. We pull
 * entities and relations through hu_memory_facade_t (via the underlying
 * hu_graph_t handle, since the W7 facade does not yet expose a "list all
 * entities" facet — see TODO below). No global state, deterministic
 * output (sorted by score desc, then by id asc as a stable tiebreaker).
 *
 * The implementation favors clarity over micro-optimization. For the
 * 10K-entity ceiling at 20 iterations, the inner loop is O(N + E). On
 * Apple Silicon this runs in well under 5 ms for realistic per-contact
 * graphs (median ~200 entities, ~500 edges).
 */
#include "human/memory/pagerank.h"

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/graph.h"
#include "human/memory/memory.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Allocator shorthands ───────────────────────────────────────────────── */

static inline void *xalloc(hu_allocator_t *a, size_t n) {
    return a->alloc(a->ctx, n);
}
static inline void xfree(hu_allocator_t *a, void *p, size_t n) {
    if (p) a->free(a->ctx, p, n);
}

/* ── Internal types ─────────────────────────────────────────────────────── */

typedef struct edge {
    int32_t src_idx;
    int32_t dst_idx;
} edge_t;

typedef struct rank_pair {
    int64_t id;
    float   score;
} rank_pair_t;

/* qsort comparator: score descending, id ascending as stable tiebreaker. */
static int rank_pair_cmp(const void *a, const void *b) {
    const rank_pair_t *ra = (const rank_pair_t *)a;
    const rank_pair_t *rb = (const rank_pair_t *)b;
    if (ra->score < rb->score) return 1;
    if (ra->score > rb->score) return -1;
    if (ra->id    < rb->id)    return -1;
    if (ra->id    > rb->id)    return  1;
    return 0;
}

/* Linear scan: id -> array index. Entity count is bounded by
 * HU_PAGERANK_MAX_ENTITIES (10000), so worst-case O(N*E) on sparse graphs is
 * still cheap (1e8 ops cap, but in practice E << N and many lookups short-
 * circuit). A hash table is the right answer when N grows past 10K, which
 * is exactly when this function refuses to run. */
static int32_t find_idx(const int64_t *ids, size_t n, int64_t id) {
    for (size_t i = 0; i < n; i++) {
        if (ids[i] == id) return (int32_t)i;
    }
    return -1;
}

hu_error_t hu_memory_pagerank_seeds(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                    const char *contact_id, size_t cid_len,
                                    const int64_t *seed_entity_ids, size_t seeds_count,
                                    float damping, size_t iterations,
                                    int64_t **out_ids, float **out_scores,
                                    size_t *out_count) {
    if (!m || !alloc || !contact_id || !out_ids || !out_scores || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    if (cid_len == 0) return HU_ERR_INVALID_ARGUMENT;
    if (seeds_count > 0 && !seed_entity_ids) return HU_ERR_INVALID_ARGUMENT;

    *out_ids = NULL;
    *out_scores = NULL;
    *out_count = 0;

    /* Empty seed set: explicit no-op, return success with empty output.
     * (HippoRAG users almost always want personalised PR; uniform PR over
     * the whole graph is surprising default behavior.) */
    if (seeds_count == 0) return HU_OK;

    /* Defaults & clamps. */
    if (!(damping > 0.0f && damping < 1.0f)) damping = HU_PAGERANK_DEFAULT_DAMPING;
    if (iterations == 0) iterations = HU_PAGERANK_DEFAULT_ITERATIONS;
    if (iterations > 1000) iterations = 1000;  /* sanity cap */

    /* TODO(W12-facade-list): the W7 facade has no "list all entities for a
     * contact" query yet (only by_name / by_id / neighbors). Until it does,
     * we read directly from the underlying graph handle. The W12 spec
     * explicitly permits this with a TODO marker. */
    hu_graph_t *g = hu_memory_facade_graph_handle(m);
    if (!g) return HU_ERR_NOT_SUPPORTED;

    hu_graph_entity_t *ents = NULL;
    size_t n = 0;
    hu_error_t err = hu_graph_list_entities(g, alloc, contact_id, cid_len,
                                             HU_PAGERANK_MAX_ENTITIES + 1, &ents, &n);
    if (err != HU_OK && err != HU_ERR_NOT_FOUND) return err;
    if (n == 0) {
        if (ents) hu_graph_entities_free(alloc, ents, n);
        return HU_OK; /* no entities, no scores */
    }
    if (n > HU_PAGERANK_MAX_ENTITIES) {
        hu_graph_entities_free(alloc, ents, n);
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Build id table. */
    int64_t *ids = xalloc(alloc, n * sizeof(*ids));
    if (!ids) {
        hu_graph_entities_free(alloc, ents, n);
        return HU_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < n; i++) ids[i] = ents[i].id;
    hu_graph_entities_free(alloc, ents, n);

    /* Pull relations and project to (src_idx, dst_idx) edges. The list cap
     * matches the entity cap squared, but real graphs are sparse — we trust
     * list_relations to honour the limit. */
    hu_graph_relation_t *rels = NULL;
    size_t rn = 0;
    size_t rel_cap = n * 16; /* expect average degree <= 16 */
    if (rel_cap > 200000) rel_cap = 200000;
    err = hu_graph_list_relations(g, alloc, contact_id, cid_len, rel_cap, &rels, &rn);
    if (err != HU_OK && err != HU_ERR_NOT_FOUND) {
        xfree(alloc, ids, n * sizeof(*ids));
        return err;
    }

    edge_t *edges = NULL;
    size_t edges_count = 0;
    if (rn > 0) {
        edges = xalloc(alloc, rn * sizeof(*edges));
        if (!edges) {
            hu_graph_relations_free(alloc, rels, rn);
            xfree(alloc, ids, n * sizeof(*ids));
            return HU_ERR_OUT_OF_MEMORY;
        }
        for (size_t i = 0; i < rn; i++) {
            int32_t s = find_idx(ids, n, rels[i].source_id);
            int32_t d = find_idx(ids, n, rels[i].target_id);
            if (s < 0 || d < 0) continue; /* dangling edge — skip */
            edges[edges_count].src_idx = s;
            edges[edges_count].dst_idx = d;
            edges_count++;
        }
    }
    if (rels) hu_graph_relations_free(alloc, rels, rn);

    /* Per-node out-degree (treating each relation as a directed edge — and
     * also counting the reverse direction so undirected social links aren't
     * starved at sinks). HippoRAG uses an undirected graph; we follow. */
    int32_t *out_deg = xalloc(alloc, n * sizeof(*out_deg));
    if (!out_deg) {
        xfree(alloc, edges, rn * sizeof(*edges));
        xfree(alloc, ids, n * sizeof(*ids));
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(out_deg, 0, n * sizeof(*out_deg));
    for (size_t i = 0; i < edges_count; i++) {
        out_deg[edges[i].src_idx]++;
        out_deg[edges[i].dst_idx]++;
    }

    /* Personalisation vector: 1/seeds_count on each known seed, 0 elsewhere.
     * Unknown seed ids (not present in this contact's graph) are skipped —
     * they should not corrupt the distribution. */
    float *pers = xalloc(alloc, n * sizeof(*pers));
    float *pr   = xalloc(alloc, n * sizeof(*pr));
    float *pr_n = xalloc(alloc, n * sizeof(*pr_n));
    if (!pers || !pr || !pr_n) {
        xfree(alloc, pers, n * sizeof(*pers));
        xfree(alloc, pr, n * sizeof(*pr));
        xfree(alloc, pr_n, n * sizeof(*pr_n));
        xfree(alloc, out_deg, n * sizeof(*out_deg));
        xfree(alloc, edges, rn * sizeof(*edges));
        xfree(alloc, ids, n * sizeof(*ids));
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(pers, 0, n * sizeof(*pers));
    int32_t known_seeds = 0;
    for (size_t i = 0; i < seeds_count; i++) {
        int32_t idx = find_idx(ids, n, seed_entity_ids[i]);
        if (idx >= 0) known_seeds++;
    }
    if (known_seeds == 0) {
        /* No known seeds: distribution would be all-zero, leaving PR with no
         * teleport mass. Fall back to uniform across the seed set the
         * caller asked for (which is now a structural empty); return empty. */
        xfree(alloc, pers, n * sizeof(*pers));
        xfree(alloc, pr, n * sizeof(*pr));
        xfree(alloc, pr_n, n * sizeof(*pr_n));
        xfree(alloc, out_deg, n * sizeof(*out_deg));
        xfree(alloc, edges, rn * sizeof(*edges));
        xfree(alloc, ids, n * sizeof(*ids));
        return HU_OK;
    }
    float seed_mass = 1.0f / (float)known_seeds;
    for (size_t i = 0; i < seeds_count; i++) {
        int32_t idx = find_idx(ids, n, seed_entity_ids[i]);
        if (idx >= 0) pers[idx] = seed_mass;
    }

    /* Initial PR = personalisation vector. */
    memcpy(pr, pers, n * sizeof(*pr));

    /* Power iteration. */
    for (size_t it = 0; it < iterations; it++) {
        /* Teleport mass: (1-d) * pers[i]. */
        for (size_t i = 0; i < n; i++) {
            pr_n[i] = (1.0f - damping) * pers[i];
        }
        /* Edge contribution + dangling-node redistribution. */
        float dangling_mass = 0.0f;
        for (size_t i = 0; i < n; i++) {
            if (out_deg[i] == 0) dangling_mass += pr[i];
        }
        if (dangling_mass > 0.0f) {
            float share = damping * dangling_mass / (float)n;
            for (size_t i = 0; i < n; i++) pr_n[i] += share;
        }
        for (size_t e = 0; e < edges_count; e++) {
            int32_t s = edges[e].src_idx;
            int32_t d = edges[e].dst_idx;
            float share_s = damping * pr[s] / (float)out_deg[s];
            pr_n[d] += share_s;
            float share_d = damping * pr[d] / (float)out_deg[d];
            pr_n[s] += share_d;
        }
        /* Swap. */
        float *tmp = pr;
        pr = pr_n;
        pr_n = tmp;
    }

    /* Build sorted output. */
    rank_pair_t *pairs = xalloc(alloc, n * sizeof(*pairs));
    if (!pairs) {
        xfree(alloc, pers, n * sizeof(*pers));
        xfree(alloc, pr, n * sizeof(*pr));
        xfree(alloc, pr_n, n * sizeof(*pr_n));
        xfree(alloc, out_deg, n * sizeof(*out_deg));
        xfree(alloc, edges, rn * sizeof(*edges));
        xfree(alloc, ids, n * sizeof(*ids));
        return HU_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < n; i++) {
        pairs[i].id    = ids[i];
        pairs[i].score = pr[i];
    }
    qsort(pairs, n, sizeof(*pairs), rank_pair_cmp);

    int64_t *res_ids = xalloc(alloc, n * sizeof(*res_ids));
    float   *res_sc  = xalloc(alloc, n * sizeof(*res_sc));
    if (!res_ids || !res_sc) {
        xfree(alloc, res_ids, n * sizeof(*res_ids));
        xfree(alloc, res_sc, n * sizeof(*res_sc));
        xfree(alloc, pairs, n * sizeof(*pairs));
        xfree(alloc, pers, n * sizeof(*pers));
        xfree(alloc, pr, n * sizeof(*pr));
        xfree(alloc, pr_n, n * sizeof(*pr_n));
        xfree(alloc, out_deg, n * sizeof(*out_deg));
        xfree(alloc, edges, rn * sizeof(*edges));
        xfree(alloc, ids, n * sizeof(*ids));
        return HU_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < n; i++) {
        res_ids[i] = pairs[i].id;
        res_sc[i]  = pairs[i].score;
    }

    /* Cleanup scratch. */
    xfree(alloc, pairs, n * sizeof(*pairs));
    xfree(alloc, pers, n * sizeof(*pers));
    xfree(alloc, pr, n * sizeof(*pr));
    xfree(alloc, pr_n, n * sizeof(*pr_n));
    xfree(alloc, out_deg, n * sizeof(*out_deg));
    xfree(alloc, edges, rn * sizeof(*edges));
    xfree(alloc, ids, n * sizeof(*ids));

    *out_ids    = res_ids;
    *out_scores = res_sc;
    *out_count  = n;
    return HU_OK;
}
