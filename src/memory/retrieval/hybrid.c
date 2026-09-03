#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/memory/graph.h"
#include "human/memory/rerank.h"
#include "human/memory/retrieval.h"
#include "human/memory/retrieval/rrf.h"
#include "human/memory/semantic_recall.h"
#include "human/memory/vector.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_RRF_K 60.0f

/* Contract C2 (2026-09): reconstructive hybrid retrieval tunables.
 * FLOOR: below this top rerank score, the answer is too weak to trust.
 * MIN_SCENES: fewer distinct scenes in the pool means there is nothing to
 * reconstruct from (a single conversation blob, not a set of memories).
 * MAX_NEIGHBORS: neighbour rows added per anchor within its selected scene.
 * POOL_CAP: candidate pool ceiling, mirrors the plain path's max_merged cap. */
#define HU_RECON_SCORE_FLOOR   0.34
#define HU_RECON_MIN_SCENES    2
#define HU_RECON_MAX_NEIGHBORS 2
#define HU_RECON_POOL_CAP      128
#define HU_RECON_KEY_BUF       160

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

/* ──────────────────────────────────────────────────────────────────────────
 * Contract C2: reconstructive hybrid retrieval (EverMemOS shape)
 *   scene-select -> neighbour expansion -> rerank -> time-bounded filter ->
 *   sufficiency check.
 * ────────────────────────────────────────────────────────────────────────── */

/* Free an array of owned hu_memory_entry_t (fields, then the array itself). */
static void free_entry_array(hu_allocator_t *alloc, hu_memory_entry_t *arr, size_t count) {
    if (!arr)
        return;
    for (size_t i = 0; i < count; i++)
        hu_memory_entry_free_fields(alloc, &arr[i]);
    alloc->free(alloc->ctx, arr, count * sizeof(hu_memory_entry_t));
}

/* "YYYY-MM-DD" prefix of an ISO-8601 timestamp, or "" when unavailable. */
static void recon_day_bucket(const char *ts, size_t ts_len, char *buf, size_t buf_cap) {
    if (ts && ts_len >= 10 && buf_cap > 10) {
        memcpy(buf, ts, 10);
        buf[10] = '\0';
    } else if (buf_cap > 0) {
        buf[0] = '\0';
    }
}

/* Scene key: session_id + day bucket when a session is known (this IS the
 * "conversation" the reconstructive design groups by); otherwise a singleton
 * scene keyed by the entry's own key/id so session-less rows (e.g. semantic
 * hits, which carry no session_id) still participate without being merged
 * into an unrelated scene. */
static void recon_scene_key(const hu_memory_entry_t *e, char *buf, size_t buf_cap) {
    char day[16];
    recon_day_bucket(e->timestamp, e->timestamp_len, day, sizeof(day));
    if (e->session_id && e->session_id_len > 0) {
        size_t sid_len = e->session_id_len < 96 ? e->session_id_len : 96;
        snprintf(buf, buf_cap, "s:%.*s:%s", (int)sid_len, e->session_id, day[0] ? day : "-");
    } else {
        const char *k = (e->key && e->key_len > 0) ? e->key : (e->id ? e->id : "");
        size_t klen = (e->key && e->key_len > 0) ? e->key_len : (e->id ? e->id_len : 0);
        if (klen > 140)
            klen = 140;
        snprintf(buf, buf_cap, "u:%.*s", (int)klen, k);
    }
}

/* Key prefix: the entry's key up to (excluding) its last ':' — the shared
 * root of a versioned/superseding key family (e.g. "profile:city:2026-06-01"
 * -> "profile:city"). Keys with no ':' have no supersede family. */
static void recon_key_prefix(const hu_memory_entry_t *e, char *buf, size_t buf_cap) {
    const char *k = (e->key && e->key_len > 0) ? e->key : (e->id ? e->id : "");
    size_t klen = (e->key && e->key_len > 0) ? e->key_len : (e->id ? e->id_len : 0);
    if (klen == 0 || buf_cap == 0) {
        if (buf_cap > 0)
            buf[0] = '\0';
        return;
    }
    size_t last_colon = (size_t)-1;
    for (size_t i = 0; i < klen; i++)
        if (k[i] == ':')
            last_colon = i;
    if (last_colon == (size_t)-1) {
        buf[0] = '\0'; /* no ':' -> no supersede family */
        return;
    }
    size_t plen = last_colon;
    if (plen >= buf_cap)
        plen = buf_cap - 1;
    memcpy(buf, k, plen);
    buf[plen] = '\0';
}

/* Word-boundary temporal-cue detection. Uses hu_str_contains_word_ci_n (not a
 * plain substring search) per the substring-classifier-pitfalls lesson: naive
 * substring matching on "ago"/"when" would false-positive on "ago" inside
 * "diagonal" or "when" inside "somewhen"-style compounds. */
static bool recon_query_has_temporal_cue(const char *query, size_t query_len) {
    static const char *cues[] = {"last", "recent", "yesterday", "ago",
                                 "when", "before", "after",     "still"};
    for (size_t i = 0; i < sizeof(cues) / sizeof(cues[0]); i++)
        if (hu_str_contains_word_ci_n(query, query_len, cues[i]))
            return true;
    return false;
}

/* Test-only stage ablation for measuring the C2 reconstructive pipeline
 * (docs/plans/2026-08-02-semantic-retrieval/memory-benchmarks-c2-ablation.json).
 * HU_RECON_ABLATE=<comma-separated tokens> disables or varies one stage at a
 * time so each can be scored in isolation on the same benchmark split. Unset
 * -- the state every real caller runs under today -- reproduces Contract C2
 * exactly (opts->reconstructive itself already defaults off; this env var
 * only changes behavior further inside that already-opt-in path). Recognized
 * tokens: no_scene, no_neighbors, no_rerank, no_temporal, force_sufficient,
 * scene_coverage_first. Unrecognized tokens are ignored (no typo silently
 * changes behavior in a way that would go unnoticed either). */
static bool recon_ablate(const char *token) {
    const char *env = getenv("HU_RECON_ABLATE");
    if (!env || !*env)
        return false;
    size_t tlen = strlen(token);
    const char *p = env;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t seglen = comma ? (size_t)(comma - p) : strlen(p);
        if (seglen == tlen && strncmp(p, token, tlen) == 0)
            return true;
        if (!comma)
            break;
        p = comma + 1;
    }
    return false;
}

/* Session-group key for scene_coverage_first: strips the day-bucket suffix
 * off a "s:<session_id>:<day>" scene key so scenes from the same session on
 * different days still count as one coverage unit; a "u:<key>" singleton
 * scene (no session_id) is already its own group. */
static void recon_session_group(const char *scene_key, char *buf, size_t buf_cap) {
    if (buf_cap == 0)
        return;
    size_t len = strlen(scene_key);
    if (scene_key[0] == 's' && scene_key[1] == ':') {
        const char *last_colon = strrchr(scene_key, ':');
        if (last_colon)
            len = (size_t)(last_colon - scene_key);
    }
    if (len >= buf_cap)
        len = buf_cap - 1;
    memcpy(buf, scene_key, len);
    buf[len] = '\0';
}

/* Reconstructive hybrid retrieval. Builds an RRF-fused candidate pool from
 * keyword/semantic/graph (entries, not hu_search_result_t, so session_id and
 * timestamp survive the merge — see hu_rrf_merge), then:
 *   (1) scene-select: group by session_id+day, pick top scenes by summed
 *       RRF score until their rows cover `limit`.
 *   (2) neighbour expansion: within selected scenes, add up to
 *       HU_RECON_MAX_NEIGHBORS session-adjacent rows per anchor.
 *   (3) rerank: cross-encoder term-overlap over the working set.
 *   (4) time-bounded filter: when the query carries a temporal cue, bias
 *       toward recency and drop rows superseded by a newer same-key-prefix row.
 *   (5) sufficiency check: caller falls back to the plain hybrid result when
 *       *sufficient comes back false — this function never claims success on
 *       a degenerate reconstruction.
 *
 * Does NOT free keyword_result/semantic_result/graph_result — the caller
 * keeps ownership of those regardless of outcome, so it can fall back to its
 * own plain merge when *sufficient is false. On success (*sufficient=true),
 * `out` is filled with reconstructed, caller-owned entries/scores. */
static hu_error_t hybrid_reconstruct(hu_allocator_t *alloc, hu_memory_t *backend, const char *query,
                                     size_t query_len, size_t limit,
                                     const hu_retrieval_result_t *keyword_result,
                                     const hu_retrieval_result_t *semantic_result,
                                     const hu_retrieval_result_t *graph_result /* may be NULL */,
                                     hu_retrieval_result_t *out, bool *sufficient) {
    *sufficient = false;

    size_t num_sources = 0;
    const hu_memory_entry_t *src_lists[3];
    size_t src_lens[3];
    if (graph_result && graph_result->count > 0) {
        src_lists[num_sources] = graph_result->entries;
        src_lens[num_sources] = graph_result->count;
        num_sources++;
    }
    if (keyword_result->count > 0) {
        src_lists[num_sources] = keyword_result->entries;
        src_lens[num_sources] = keyword_result->count;
        num_sources++;
    }
    if (semantic_result->count > 0) {
        src_lists[num_sources] = semantic_result->entries;
        src_lens[num_sources] = semantic_result->count;
        num_sources++;
    }
    if (num_sources == 0)
        return HU_OK; /* nothing to reconstruct; caller's plain path is equally empty */

    size_t total_in = 0;
    for (size_t i = 0; i < num_sources; i++)
        total_in += src_lens[i];
    size_t pool_cap = total_in > limit ? total_in : limit;
    if (pool_cap > HU_RECON_POOL_CAP)
        pool_cap = HU_RECON_POOL_CAP;
    if (pool_cap == 0)
        pool_cap = 1;

    hu_memory_entry_t *pool = NULL;
    size_t pool_count = 0;
    hu_error_t err = hu_rrf_merge(alloc, src_lists, src_lens, num_sources, (unsigned)HU_RRF_K,
                                  pool_cap, &pool, &pool_count);
    if (err != HU_OK || pool_count == 0)
        return err;
    /* ---- (1) scene-select ---- */
    char (*scene_keys)[HU_RECON_KEY_BUF] =
        alloc->alloc(alloc->ctx, pool_count * sizeof(*scene_keys));
    if (!scene_keys) {
        hu_rrf_free_result(alloc, pool, pool_count);
        return HU_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < pool_count; i++)
        recon_scene_key(&pool[i], scene_keys[i], sizeof(scene_keys[i]));

    typedef struct {
        const char *key;
        double summed;
        size_t row_count;
    } recon_scene_t;
    recon_scene_t *scenes = alloc->alloc(alloc->ctx, pool_count * sizeof(recon_scene_t));
    if (!scenes) {
        alloc->free(alloc->ctx, scene_keys, pool_count * sizeof(*scene_keys));
        hu_rrf_free_result(alloc, pool, pool_count);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t scene_count = 0;
    for (size_t i = 0; i < pool_count; i++) {
        bool found = false;
        for (size_t s = 0; s < scene_count; s++) {
            if (strcmp(scenes[s].key, scene_keys[i]) == 0) {
                scenes[s].summed += pool[i].score;
                scenes[s].row_count++;
                found = true;
                break;
            }
        }
        if (!found) {
            scenes[scene_count].key = scene_keys[i];
            scenes[scene_count].summed = pool[i].score;
            scenes[scene_count].row_count = 1;
            scene_count++;
        }
    }
    /* distinct scenes IN THE POOL (before selection) is the sufficiency
     * signal: a single scene means there was nothing to reconstruct among —
     * selecting the top scene is expected to often leave just one scene in
     * the final working set, and that is success, not insufficiency. */
    size_t distinct_scenes_in_pool = scene_count;

    /* Sort scenes by summed RRF score descending (scene_count is small). */
    for (size_t i = 0; i < scene_count; i++)
        for (size_t j = i + 1; j < scene_count; j++)
            if (scenes[j].summed > scenes[i].summed) {
                recon_scene_t t = scenes[i];
                scenes[i] = scenes[j];
                scenes[j] = t;
            }

    /* scene_coverage_first (ablation g): reorder `scenes` so the best-scoring
     * scene of EACH distinct session comes first (guaranteeing session
     * coverage), then the remaining scenes in their original score order --
     * before the standard "cover `limit` rows" selection below runs. This
     * tests the hypothesis that plain top-scene selection trades session
     * coverage for within-scene precision (memory-benchmarks-c2.json:
     * temporal-reasoning 0.4, multi-session 0.8 vs 1.0 for plain hybrid). */
    if (recon_ablate("scene_coverage_first") && scene_count > 1) {
        recon_scene_t *ordered = alloc->alloc(alloc->ctx, scene_count * sizeof(recon_scene_t));
        bool *picked = alloc->alloc(alloc->ctx, scene_count * sizeof(bool));
        char (*groups)[HU_RECON_KEY_BUF] = alloc->alloc(alloc->ctx, scene_count * sizeof(*groups));
        char (*seen)[HU_RECON_KEY_BUF] = alloc->alloc(alloc->ctx, scene_count * sizeof(*seen));
        if (ordered && picked && groups && seen) {
            memset(picked, 0, scene_count * sizeof(bool));
            for (size_t i = 0; i < scene_count; i++)
                recon_session_group(scenes[i].key, groups[i], sizeof(groups[i]));
            size_t oc = 0, seen_count = 0;
            for (size_t i = 0; i < scene_count; i++) {
                bool dup = false;
                for (size_t k = 0; k < seen_count; k++)
                    if (strcmp(seen[k], groups[i]) == 0) {
                        dup = true;
                        break;
                    }
                if (dup)
                    continue;
                ordered[oc++] = scenes[i];
                picked[i] = true;
                memcpy(seen[seen_count], groups[i], sizeof(seen[seen_count]));
                seen_count++;
            }
            for (size_t i = 0; i < scene_count; i++)
                if (!picked[i])
                    ordered[oc++] = scenes[i];
            memcpy(scenes, ordered, scene_count * sizeof(recon_scene_t));
        }
        if (ordered)
            alloc->free(alloc->ctx, ordered, scene_count * sizeof(recon_scene_t));
        if (picked)
            alloc->free(alloc->ctx, picked, scene_count * sizeof(bool));
        if (groups)
            alloc->free(alloc->ctx, groups, scene_count * sizeof(*groups));
        if (seen)
            alloc->free(alloc->ctx, seen, scene_count * sizeof(*seen));
    }

    /* Pick top scenes until their rows cover `limit` (at least the top one).
     * no_scene (ablation b) disables the cap entirely -- every scene in the
     * pool is "selected", which is equivalent to skipping scene-select and
     * handing the whole RRF pool straight to neighbour expansion/rerank. */
    size_t selected_scene_count = 0;
    size_t rows_covered = 0;
    if (recon_ablate("no_scene")) {
        selected_scene_count = scene_count;
    } else {
        for (size_t s = 0; s < scene_count; s++) {
            selected_scene_count++;
            rows_covered += scenes[s].row_count;
            if (rows_covered >= limit)
                break;
        }
    }

    size_t work_cap = pool_count * (1 + HU_RECON_MAX_NEIGHBORS) + 8;
    hu_memory_entry_t *work = alloc->alloc(alloc->ctx, work_cap * sizeof(hu_memory_entry_t));
    double *work_scores = alloc->alloc(alloc->ctx, work_cap * sizeof(double));
    if (!work || !work_scores) {
        if (work)
            alloc->free(alloc->ctx, work, work_cap * sizeof(hu_memory_entry_t));
        if (work_scores)
            alloc->free(alloc->ctx, work_scores, work_cap * sizeof(double));
        alloc->free(alloc->ctx, scenes, pool_count * sizeof(recon_scene_t));
        alloc->free(alloc->ctx, scene_keys, pool_count * sizeof(*scene_keys));
        hu_rrf_free_result(alloc, pool, pool_count);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t work_count = 0;

    /* Move pool rows whose scene was selected into `work` (ownership
     * transfer: zero the pool slot so the later free_entry_array(pool, ...)
     * is a no-op for moved rows). */
    for (size_t i = 0; i < pool_count; i++) {
        bool selected = false;
        for (size_t s = 0; s < selected_scene_count; s++)
            if (strcmp(scenes[s].key, scene_keys[i]) == 0) {
                selected = true;
                break;
            }
        if (!selected)
            continue;
        work[work_count] = pool[i];
        work_scores[work_count] = pool[i].score;
        memset(&pool[i], 0, sizeof(pool[i]));
        work_count++;
    }

    /* ---- (2) neighbour expansion within selected scenes ---- */
    size_t anchor_count = work_count;
    bool ablate_neighbors = recon_ablate("no_neighbors");
    for (size_t a = 0; !ablate_neighbors && a < anchor_count && backend && backend->vtable &&
                       backend->vtable->list;
         a++) {
        const hu_memory_entry_t *anchor = &work[a];
        if (!anchor->session_id || anchor->session_id_len == 0)
            continue;
        hu_memory_entry_t *sess_entries = NULL;
        size_t sess_count = 0;
        if (backend->vtable->list(backend->ctx, alloc, NULL, anchor->session_id,
                                  anchor->session_id_len, &sess_entries, &sess_count) != HU_OK ||
            !sess_entries || sess_count == 0)
            continue;

        /* Sort session rows by timestamp ascending; missing timestamps last. */
        for (size_t i = 0; i < sess_count; i++) {
            for (size_t j = i + 1; j < sess_count; j++) {
                double ti = hu_retrieval_parse_timestamp_hours(sess_entries[i].timestamp,
                                                               sess_entries[i].timestamp_len);
                double tj = hu_retrieval_parse_timestamp_hours(sess_entries[j].timestamp,
                                                               sess_entries[j].timestamp_len);
                if (ti < 0)
                    ti = 1e18;
                if (tj < 0)
                    tj = 1e18;
                if (tj < ti) {
                    hu_memory_entry_t t = sess_entries[i];
                    sess_entries[i] = sess_entries[j];
                    sess_entries[j] = t;
                }
            }
        }
        long anchor_pos = -1;
        for (size_t i = 0; i < sess_count; i++) {
            if (sess_entries[i].key && anchor->key && sess_entries[i].key_len == anchor->key_len &&
                memcmp(sess_entries[i].key, anchor->key, anchor->key_len) == 0) {
                anchor_pos = (long)i;
                break;
            }
        }
        size_t added_for_anchor = 0;
        if (anchor_pos >= 0) {
            long offsets[4] = {-1, 1, -2, 2}; /* nearest predecessor/successor first */
            for (size_t oi = 0; oi < 4 && added_for_anchor < HU_RECON_MAX_NEIGHBORS; oi++) {
                long p = anchor_pos + offsets[oi];
                if (p < 0 || (size_t)p >= sess_count || !sess_entries[(size_t)p].key)
                    continue;
                bool dup = false;
                for (size_t w = 0; w < work_count; w++) {
                    if (work[w].key && work[w].key_len == sess_entries[(size_t)p].key_len &&
                        memcmp(work[w].key, sess_entries[(size_t)p].key,
                               sess_entries[(size_t)p].key_len) == 0) {
                        dup = true;
                        break;
                    }
                }
                if (dup || work_count >= work_cap)
                    continue;
                work[work_count] = sess_entries[(size_t)p];
                /* Neighbours are supporting context, not primary hits. */
                work_scores[work_count] = work_scores[a] * 0.5;
                memset(&sess_entries[(size_t)p], 0, sizeof(sess_entries[(size_t)p]));
                work_count++;
                added_for_anchor++;
            }
        }
        free_entry_array(alloc, sess_entries, sess_count);
    }

    /* ---- (3) rerank: cross-encoder term overlap over the working set ---- */
    if (work_count > 0 && !recon_ablate("no_rerank")) {
        hu_search_result_t *sr = alloc->alloc(alloc->ctx, work_count * sizeof(hu_search_result_t));
        if (sr) {
            memset(sr, 0, work_count * sizeof(hu_search_result_t));
            hu_allocator_t sys = hu_system_allocator();
            entries_to_search_results(&sys, work, work_scores, work_count, sr);
            for (size_t i = 0; i < work_count; i++)
                sr[i].candidate_idx = i;
            if (hu_rerank_cross_encoder(query, sr, work_count) == HU_OK) {
                hu_memory_entry_t *reordered =
                    alloc->alloc(alloc->ctx, work_count * sizeof(hu_memory_entry_t));
                double *reordered_scores = alloc->alloc(alloc->ctx, work_count * sizeof(double));
                if (reordered && reordered_scores) {
                    for (size_t i = 0; i < work_count; i++) {
                        reordered[i] = work[sr[i].candidate_idx];
                        reordered_scores[i] = (double)sr[i].rerank_score;
                    }
                    alloc->free(alloc->ctx, work, work_cap * sizeof(hu_memory_entry_t));
                    alloc->free(alloc->ctx, work_scores, work_cap * sizeof(double));
                    work = reordered;
                    work_scores = reordered_scores;
                    work_cap = work_count;
                } else {
                    if (reordered)
                        alloc->free(alloc->ctx, reordered, work_count * sizeof(hu_memory_entry_t));
                    if (reordered_scores)
                        alloc->free(alloc->ctx, reordered_scores, work_count * sizeof(double));
                }
            }
            hu_rerank_free_results(sr, work_count);
            alloc->free(alloc->ctx, sr, work_count * sizeof(hu_search_result_t));
        }
    }

    /* ---- (4) time-bounded filter ---- */
    if (work_count > 0 && !recon_ablate("no_temporal") &&
        recon_query_has_temporal_cue(query, query_len)) {
        /* (a) prefer rows closest to now: reuse the existing temporal-decay
         * helper as a generic recency bias (it already computes age against
         * time(NULL) internally). */
        for (size_t i = 0; i < work_count; i++)
            work_scores[i] = hu_temporal_decay_score(work_scores[i], 1.0, work[i].timestamp,
                                                     work[i].timestamp_len);

        /* (b) drop rows superseded by a later row sharing the same key prefix. */
        size_t drop_alloc_count = work_count;
        bool *drop = alloc->alloc(alloc->ctx, drop_alloc_count * sizeof(bool));
        if (drop) {
            memset(drop, 0, drop_alloc_count * sizeof(bool));
            char (*prefixes)[HU_RECON_KEY_BUF] =
                alloc->alloc(alloc->ctx, drop_alloc_count * sizeof(*prefixes));
            if (prefixes) {
                for (size_t i = 0; i < work_count; i++)
                    recon_key_prefix(&work[i], prefixes[i], sizeof(prefixes[i]));
                for (size_t i = 0; i < work_count; i++) {
                    if (drop[i] || prefixes[i][0] == '\0')
                        continue;
                    for (size_t j = i + 1; j < work_count; j++) {
                        if (drop[j] || prefixes[j][0] == '\0')
                            continue;
                        if (strcmp(prefixes[i], prefixes[j]) != 0)
                            continue;
                        double ti = hu_retrieval_parse_timestamp_hours(work[i].timestamp,
                                                                       work[i].timestamp_len);
                        double tj = hu_retrieval_parse_timestamp_hours(work[j].timestamp,
                                                                       work[j].timestamp_len);
                        if (ti < 0 && tj < 0)
                            continue;
                        if (tj >= ti)
                            drop[i] = true;
                        else
                            drop[j] = true;
                    }
                }
                alloc->free(alloc->ctx, prefixes, drop_alloc_count * sizeof(*prefixes));
            }
            size_t kept = 0;
            for (size_t i = 0; i < work_count; i++) {
                if (drop[i]) {
                    hu_memory_entry_free_fields(alloc, &work[i]);
                    continue;
                }
                if (kept != i) {
                    work[kept] = work[i];
                    work_scores[kept] = work_scores[i];
                }
                kept++;
            }
            work_count = kept;
            alloc->free(alloc->ctx, drop, drop_alloc_count * sizeof(bool));
        }

        /* Re-sort by score descending after the recency bias/drops. */
        for (size_t i = 0; i < work_count; i++)
            for (size_t j = i + 1; j < work_count; j++)
                if (work_scores[j] > work_scores[i]) {
                    hu_memory_entry_t te = work[i];
                    work[i] = work[j];
                    work[j] = te;
                    double ts = work_scores[i];
                    work_scores[i] = work_scores[j];
                    work_scores[j] = ts;
                }
    }

    /* ---- (5) sufficiency check ---- */
    double top_score = work_count > 0 ? work_scores[0] : 0.0;
    bool recon_insufficient = (work_count == 0 || top_score < HU_RECON_SCORE_FLOOR ||
                               distinct_scenes_in_pool < HU_RECON_MIN_SCENES);
    /* force_sufficient (ablation f): never fall back to the plain hybrid
     * merge, even when the floor/min-scenes checks would normally decline.
     * Only meaningful when there is at least one row to return. */
    if (recon_insufficient && work_count > 0 && recon_ablate("force_sufficient"))
        recon_insufficient = false;
    if (recon_insufficient) {
        /* Free the live [0, work_count) fields, then the full backing arrays
         * (sized work_cap -- may exceed work_count after the temporal-filter
         * compaction shrank the logical count without reallocating). */
        for (size_t i = 0; i < work_count; i++)
            hu_memory_entry_free_fields(alloc, &work[i]);
        alloc->free(alloc->ctx, work, work_cap * sizeof(hu_memory_entry_t));
        alloc->free(alloc->ctx, work_scores, work_cap * sizeof(double));
        hu_rrf_free_result(alloc, pool, pool_count);
        alloc->free(alloc->ctx, scenes, pool_count * sizeof(recon_scene_t));
        alloc->free(alloc->ctx, scene_keys, pool_count * sizeof(*scene_keys));
        *sufficient = false;
        return HU_OK;
    }

    size_t final_count = work_count > limit ? limit : work_count;
    for (size_t i = final_count; i < work_count; i++)
        hu_memory_entry_free_fields(alloc, &work[i]);
    hu_memory_entry_t *final_entries = work;
    double *final_scores = work_scores;
    if (final_count != work_cap) {
        hu_memory_entry_t *trim_e =
            alloc->realloc(alloc->ctx, work, work_cap * sizeof(hu_memory_entry_t),
                           final_count * sizeof(hu_memory_entry_t));
        double *trim_s = alloc->realloc(alloc->ctx, work_scores, work_cap * sizeof(double),
                                        final_count * sizeof(double));
        if (trim_e)
            final_entries = trim_e;
        if (trim_s)
            final_scores = trim_s;
    }
    out->entries = final_entries;
    out->count = final_count;
    out->scores = final_scores;

    hu_rrf_free_result(alloc, pool, pool_count);
    alloc->free(alloc->ctx, scenes, pool_count * sizeof(recon_scene_t));
    alloc->free(alloc->ctx, scene_keys, pool_count * sizeof(*scene_keys));
    *sufficient = true;
    return HU_OK;
}

/* Commit a successful reconstruction: free the source result sets (semantic
 * may be an all-zero placeholder when there was no vector store to query --
 * hu_retrieval_result_free no-ops on a zeroed result; graph may be NULL when
 * SQLite is off or no graph context was built), install `recon` into `out`,
 * and apply the namespace filter. Shared by both hybrid_reconstruct() call
 * sites in hu_hybrid_retrieve() below so the success tail isn't duplicated. */
static hu_error_t hybrid_reconstruct_commit(
    hu_allocator_t *alloc, hu_retrieval_result_t *keyword_result,
    hu_retrieval_result_t *semantic_result, hu_retrieval_result_t *graph_result /* may be NULL */,
    hu_retrieval_result_t *recon, const hu_retrieval_options_t *opts, hu_retrieval_result_t *out) {
    hu_retrieval_result_free(alloc, keyword_result);
    hu_retrieval_result_free(alloc, semantic_result);
    if (graph_result)
        hu_retrieval_result_free(alloc, graph_result);
    *out = *recon;
    return hu_retrieval_filter_by_namespace(alloc, out, opts);
}

hu_error_t hu_hybrid_retrieve(hu_allocator_t *alloc, hu_memory_t *backend, hu_embedder_t *embedder,
                              hu_vector_store_t *vector_store, hu_graph_t *graph, const char *query,
                              size_t query_len, const hu_retrieval_options_t *opts,
                              hu_retrieval_result_t *out) {
    out->entries = NULL;
    out->count = 0;
    out->scores = NULL;

    {
        hu_error_t nerr = hu_retrieval_check_namespace(opts);
        if (nerr != HU_OK)
            return nerr;
    }

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

    /* Graph retrieval: add as extra source when graph is set.
     * Skip when a contact namespace is active — graph context is not
     * contact-keyed and would reintroduce cross-contact leakage. */
#ifdef HU_ENABLE_SQLITE
    hu_retrieval_result_t graph_result = {0};
    if (graph && !(opts && opts->contact_id && opts->contact_id_len > 0)) {
        char *graph_ctx = NULL;
        size_t graph_ctx_len = 0;
        if (hu_graph_build_context(graph, alloc, "", 0, query, query_len, 2, 2048, &graph_ctx,
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

    /* Single #ifdef site for "is there a graph result to free/merge" so the
     * two hybrid_reconstruct_commit() call sites below don't each repeat the
     * HU_ENABLE_SQLITE guard around &graph_result vs NULL. */
#ifdef HU_ENABLE_SQLITE
    hu_retrieval_result_t *graph_result_ptr = &graph_result;
#else
    hu_retrieval_result_t *graph_result_ptr = NULL;
#endif

    bool has_vector = embedder && embedder->vtable && vector_store && vector_store->vtable;

    if (!has_vector) {
        /* Contract C2: attempt reconstruction with whatever is available
         * (keyword + graph, no semantic). Falls through to the existing
         * concatenation path below when insufficient -- reconstructive=false
         * callers never reach this branch's body, so their behavior is
         * byte-for-byte unchanged. */
        if (opts && opts->reconstructive) {
            hu_retrieval_result_t empty_sem = {0};
            hu_retrieval_result_t recon = {0};
            bool sufficient = false;
            hu_error_t rerr =
                hybrid_reconstruct(alloc, backend, query, query_len, limit, &keyword_result,
                                   &empty_sem, graph_result_ptr, &recon, &sufficient);
            if (rerr == HU_OK && sufficient)
                return hybrid_reconstruct_commit(alloc, &keyword_result, &empty_sem,
                                                 graph_result_ptr, &recon, opts, out);
        }
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
                return hu_retrieval_filter_by_namespace(alloc, out, opts);
            }
            if (merged)
                alloc->free(alloc->ctx, merged, total * sizeof(hu_memory_entry_t));
            if (scores)
                alloc->free(alloc->ctx, scores, total * sizeof(double));
        }
#endif
        *out = keyword_result;
        return hu_retrieval_filter_by_namespace(alloc, out, opts);
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

    /* Semantic recall gate (Phase 2). SHADOW: report what semantic WOULD have
     * added — count, overlap with keyword hits, content fingerprint — then drop
     * it so the reply is unchanged. LIVE merges. OFF never reaches here with a
     * real store (bootstrap keeps the empty in-memory one). */
    if (hu_semantic_recall_mode() == HU_GATE_SHADOW && semantic_result.count > 0) {
        size_t overlap = 0;
        uint32_t fp = 2166136261u;
        for (size_t i = 0; i < semantic_result.count; i++) {
            const hu_memory_entry_t *e = &semantic_result.entries[i];
            for (size_t k = 0; k < e->key_len; k++)
                fp = (fp ^ (uint32_t)(unsigned char)e->key[k]) * 16777619u;
            for (size_t j = 0; j < keyword_result.count; j++) {
                const hu_memory_entry_t *kw = &keyword_result.entries[j];
                if (kw->key_len == e->key_len && kw->key && e->key &&
                    memcmp(kw->key, e->key, e->key_len) == 0) {
                    overlap++;
                    break;
                }
            }
        }
        hu_log_info("semantic_recall", NULL, "shadow: kw=%zu sem=%zu overlap=%zu fp=%08x (dropped)",
                    keyword_result.count, semantic_result.count, overlap, (unsigned)fp);
        hu_retrieval_result_free(alloc, &semantic_result);
        semantic_result.entries = NULL;
        semantic_result.count = 0;
        semantic_result.scores = NULL;
    }

    /* Contract C2: attempt reconstruction with keyword + semantic (+ graph).
     * Falls through to the plain RRF+cross-encoder merge below when
     * insufficient -- reconstructive=false callers never take this branch. */
    if (opts && opts->reconstructive) {
        hu_retrieval_result_t recon = {0};
        bool sufficient = false;
        /* hybrid_reconstruct() itself guards on graph_result->count > 0, so
         * graph_result_ptr (possibly an empty result) is safe to pass as-is. */
        hu_error_t rerr =
            hybrid_reconstruct(alloc, backend, query, query_len, limit, &keyword_result,
                               &semantic_result, graph_result_ptr, &recon, &sufficient);
        if (rerr == HU_OK && sufficient)
            return hybrid_reconstruct_commit(alloc, &keyword_result, &semantic_result,
                                             graph_result_ptr, &recon, opts, out);
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

    size_t max_merged =
        (kw_count + sem_count + gr_count) > limit ? (kw_count + sem_count + gr_count) : limit;
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
    if (err != HU_OK)
        return err;
    /* Keyword/semantic legs already namespace-filtered; re-apply for safety. */
    return hu_retrieval_filter_by_namespace(alloc, out, opts);
}
