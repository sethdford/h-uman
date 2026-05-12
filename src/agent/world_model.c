/* W9 — Per-contact world model: synthesizer, cache, negative-memory CRUD.
 *
 * The world model is a lightweight derived view: entities (top-K by mention),
 * their relations, current emotional snapshot, active goals, negative memory,
 * theory-of-mind, and recent topics. Built lazily from the W7 facade and
 * cached behind a small process-local LRU. Every hu_memory_facade_write that
 * targets a contact_id calls hu_world_model_invalidate so the next load
 * rebuilds.
 *
 * Persona integration: the build path stays persona-blind so the cache can
 * stay keyed on `(contact_id)` without persona-version churn. Persona is
 * folded in on top of the cached snapshot via `hu_world_model_merge_persona`
 * (P1.1-P1.3). The bridge in `world_model_bridge.c` calls it after load so
 * every consumer sees a persona-grounded ToM block.
 */

#include "human/agent/world_model.h"

#include "human/agent/goals.h"
#include "human/core/error.h"
#include "human/memory/graph.h"
#include "human/memory/memory.h"
#include "human/persona.h"
#include "human/persona/persona_deltas.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#ifdef HU_ENABLE_SQLITE
#include "human/memory/emotional_residue.h"
#include <sqlite3.h>
#endif

/* P2.6 — POSIX mutex around the LRU cache. The agent loop is single-
 * threaded today but W14 (counterfactual rehearsal) is permitted to
 * run on an idle background scheduler concurrent with a turn; without
 * a lock the LRU's last_access updates and slot evictions race. We
 * scope the lock narrowly: held during cache_lookup / cache_evict /
 * install / invalidate; NEVER held while building a fresh snapshot
 * (which can hit SQLite for tens of ms). */
#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#define HU_WM_HAVE_PTHREAD 1
static pthread_mutex_t s_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
#define WM_CACHE_LOCK()   pthread_mutex_lock(&s_cache_mutex)
#define WM_CACHE_UNLOCK() pthread_mutex_unlock(&s_cache_mutex)
#else
#define HU_WM_HAVE_PTHREAD 0
#define WM_CACHE_LOCK()   ((void)0)
#define WM_CACHE_UNLOCK() ((void)0)
#endif

/* ---- alloc helpers (method-pointer style) -------------------------- */

static inline void *xalloc(hu_allocator_t *a, size_t n) {
    return a->alloc(a->ctx, n);
}
static inline void xfree(hu_allocator_t *a, void *p, size_t n) {
    if (p) a->free(a->ctx, p, n);
}

/* ---- negative memory CRUD ----------------------------------------- */

#ifdef HU_ENABLE_SQLITE

static int run_ddl_(struct sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

static hu_error_t ensure_negative_memory_schema(struct sqlite3 *db) {
    static const char *const stmts[] = {
        "CREATE TABLE IF NOT EXISTS negative_memory ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "contact_id TEXT NOT NULL DEFAULT '',"
        "text TEXT NOT NULL,"
        "scope TEXT NOT NULL DEFAULT '',"
        "reason TEXT,"
        "confidence_mean REAL NOT NULL DEFAULT 1.0,"
        "confidence_variance REAL NOT NULL DEFAULT 0.0,"
        "created_at INTEGER NOT NULL)",
        "CREATE INDEX IF NOT EXISTS idx_neg_mem_contact ON negative_memory(contact_id)",
        NULL,
    };
    for (size_t i = 0; stmts[i]; i++) {
        if (run_ddl_(db, stmts[i]) != SQLITE_OK) return HU_ERR_IO;
    }
    return HU_OK;
}

static hu_error_t negative_memory_list_sqlite(struct sqlite3 *db, hu_allocator_t *alloc,
                                               const char *contact_id, size_t cid_len, size_t limit,
                                               hu_negative_memory_t **out, size_t *out_count) {
    if (!db || !alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    if (ensure_negative_memory_schema(db) != HU_OK)
        return HU_ERR_IO;

    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT id, text, scope, reason, confidence_mean, confidence_variance, "
        "       created_at FROM negative_memory "
        "WHERE contact_id = ? ORDER BY created_at DESC LIMIT ?";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(st, 1, contact_id ? contact_id : "", (int)cid_len, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, (int)(limit > 0 ? limit : 32));

    size_t cap = (limit > 0 ? limit : 32);
    hu_negative_memory_t *arr = xalloc(alloc, cap * sizeof(*arr));
    if (!arr) {
        sqlite3_finalize(st);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(arr, 0, cap * sizeof(*arr));

    size_t n = 0;
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        hu_negative_memory_t *r = &arr[n];
        r->id = sqlite3_column_int64(st, 0);
        const unsigned char *t = sqlite3_column_text(st, 1);
        if (t) {
            strncpy(r->text, (const char *)t, sizeof(r->text) - 1);
            r->text[sizeof(r->text) - 1] = '\0';
        }
        const unsigned char *s = sqlite3_column_text(st, 2);
        if (s) {
            strncpy(r->scope, (const char *)s, sizeof(r->scope) - 1);
            r->scope[sizeof(r->scope) - 1] = '\0';
        }
        const unsigned char *rs = sqlite3_column_text(st, 3);
        if (rs) {
            strncpy(r->reason, (const char *)rs, sizeof(r->reason) - 1);
            r->reason[sizeof(r->reason) - 1] = '\0';
        }
        r->belief.mean = (float)sqlite3_column_double(st, 4);
        r->belief.variance = (float)sqlite3_column_double(st, 5);
        r->created_at = sqlite3_column_int64(st, 6);
        n++;
    }
    sqlite3_finalize(st);

    if (n == 0) {
        xfree(alloc, arr, cap * sizeof(*arr));
        return HU_OK;
    }
    *out = arr;
    *out_count = n;
    return HU_OK;
}

static hu_error_t negative_memory_add_sqlite(struct sqlite3 *db, const char *contact_id,
                                             size_t cid_len, const hu_negative_memory_t *nm,
                                             int64_t *out_id) {
    if (!db || !nm)
        return HU_ERR_INVALID_ARGUMENT;
    if (nm->text[0] == '\0')
        return HU_ERR_INVALID_ARGUMENT;
    if (ensure_negative_memory_schema(db) != HU_OK)
        return HU_ERR_IO;

    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO negative_memory(contact_id, text, scope, reason, "
        " confidence_mean, confidence_variance, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(st, 1, contact_id ? contact_id : "", (int)cid_len, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, nm->text, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, nm->scope, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, nm->reason, -1, SQLITE_STATIC);
    sqlite3_bind_double(st, 5, (double)nm->belief.mean);
    sqlite3_bind_double(st, 6, (double)nm->belief.variance);
    sqlite3_bind_int64(st, 7, nm->created_at);
    int rc = sqlite3_step(st);
    int64_t id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        return HU_ERR_IO;
    if (out_id)
        *out_id = id;
    return HU_OK;
}

#endif /* HU_ENABLE_SQLITE */

hu_error_t hu_negative_memory_add(struct hu_graph *g, const char *contact_id, size_t cid_len,
                                   const hu_negative_memory_t *nm, int64_t *out_id) {
#ifndef HU_ENABLE_SQLITE
    (void)g; (void)contact_id; (void)cid_len; (void)nm; (void)out_id;
    return HU_ERR_NOT_SUPPORTED;
#else
    if (!g || !nm)
        return HU_ERR_INVALID_ARGUMENT;
    struct sqlite3 *db = hu_memory_sqlite_from_graph(g);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    hu_error_t e = negative_memory_add_sqlite(db, contact_id, cid_len, nm, out_id);
    /* P2.1 — invalidate the world-model cache so a freshly inserted
     * "do not say X" lands on the next read instead of waiting up to
     * 60s for the TTL. Negative-memory writes don't go through the W7
     * facade write hook (which is what graph entity/relation writes use
     * via `hu_world_model_invalidate` in `src/memory/graph.c`), so the
     * invalidate has to happen here at the point of write. */
    if (e == HU_OK && contact_id && cid_len > 0)
        hu_world_model_invalidate(contact_id, cid_len);
    return e;
#endif
}

hu_error_t hu_negative_memory_add_facade(hu_memory_facade_t *m, const char *contact_id,
                                         size_t cid_len, const hu_negative_memory_t *nm,
                                         int64_t *out_id) {
#ifndef HU_ENABLE_SQLITE
    (void)m;
    (void)contact_id;
    (void)cid_len;
    (void)nm;
    (void)out_id;
    return HU_ERR_NOT_SUPPORTED;
#else
    if (!m)
        return HU_ERR_INVALID_ARGUMENT;
    struct sqlite3 *db = hu_memory_facade_sqlite_db(m);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    hu_error_t e = negative_memory_add_sqlite(db, contact_id, cid_len, nm, out_id);
    /* P2.1 — same invalidation contract as the graph variant above. */
    if (e == HU_OK && contact_id && cid_len > 0)
        hu_world_model_invalidate(contact_id, cid_len);
    return e;
#endif
}

hu_error_t hu_negative_memory_add_facade_gated(hu_memory_facade_t *m, const char *contact_id,
                                                size_t cid_len, const hu_negative_memory_t *nm,
                                                hu_write_source_t source, int64_t now_ms,
                                                int64_t *out_id) {
#ifndef HU_ENABLE_SQLITE
    (void)m; (void)contact_id; (void)cid_len; (void)nm;
    (void)source; (void)now_ms; (void)out_id;
    return HU_ERR_NOT_SUPPORTED;
#else
    if (out_id) *out_id = 0;
    if (!m || !nm) return HU_ERR_INVALID_ARGUMENT;
    if (nm->text[0] == '\0') return HU_ERR_INVALID_ARGUMENT;

    /* Score the proposed insert. We don't have a contradiction signal at
     * this layer (the conflict_resolver runs over relations, not over
     * negative-memory text), so contradiction_flag/supersession stay false.
     * recent_writes is also unavailable here without threading the W1 ring
     * through the facade; pass 0 so anomaly score stays 1.0 — the source
     * weight does the heavy lifting. The point of the gate is to reject
     * untrusted-channel writes, which the source-score band alone catches. */
    int64_t now = now_ms > 0 ? now_ms : (int64_t)time(NULL) * 1000;
    hu_write_trust_input_t in = {
        .source = source,
        .observed_at = nm->created_at > 0 ? nm->created_at : now,
        .now = now,
        .contradiction_flag = false,
        .supersession = false,
        .recent_writes = 0,
        .rate_limit = 10,
    };
    hu_write_trust_decision_t dec = hu_write_trust_score(&in);
    hu_write_outcome_t outcome = dec.outcome;

    /* P3.1 hardening (source-allowlist on top of write_trust band) — the
     * spec risk row makes this asymmetric: a false-positive lets a noisy
     * but benign open-channel hint into the negatives table at low belief;
     * a false-negative lets an attacker silence the agent on a safety
     * topic. The generic write_trust band can score CHANNEL_OPEN as LIVE
     * (source 0.55 × 0.40 + clean cs/as ≈ 0.72), so we layer a per-source
     * allowlist on top: only USER, AGENT (self-rag), and CHANNEL_TRUSTED
     * (paired/authenticated) sources may insert negative memory at LIVE
     * belief. Anything else demotes to QUARANTINE so the planner reads it
     * as a hint, not a hard rule. DROP stays DROP. */
    bool source_trusted_for_negmem = (source == HU_WRITE_SOURCE_USER ||
                                      source == HU_WRITE_SOURCE_AGENT ||
                                      source == HU_WRITE_SOURCE_CHANNEL_TRUSTED);
    if (!source_trusted_for_negmem && outcome == HU_WRITE_OUTCOME_LIVE)
        outcome = HU_WRITE_OUTCOME_QUARANTINE;

    if (outcome == HU_WRITE_OUTCOME_DROP) {
        /* Adversarial source — refuse silently. The caller (channel /
         * bridge) decides whether to log; we don't surface attacker
         * details on the data path. */
        return HU_ERR_PERMISSION_DENIED;
    }

    /* QUARANTINE band: insert but clamp belief.mean so the planner reads
     * this as a soft hint, not a hard rule. We mutate a local copy — the
     * caller's nm stays unchanged. */
    hu_negative_memory_t to_insert = *nm;
    if (outcome == HU_WRITE_OUTCOME_QUARANTINE) {
        if (to_insert.belief.mean > 0.5f) to_insert.belief.mean = 0.5f;
        if (to_insert.belief.variance < 0.25f) to_insert.belief.variance = 0.25f;
    }

    struct sqlite3 *db = hu_memory_facade_sqlite_db(m);
    if (!db) return HU_ERR_INVALID_ARGUMENT;
    hu_error_t e = negative_memory_add_sqlite(db, contact_id, cid_len, &to_insert, out_id);
    if (e == HU_OK && contact_id && cid_len > 0)
        hu_world_model_invalidate(contact_id, cid_len);
    return e;
#endif
}

hu_error_t hu_negative_memory_list(struct hu_graph *g, hu_allocator_t *alloc,
                                    const char *contact_id, size_t cid_len,
                                    size_t limit, hu_negative_memory_t **out,
                                    size_t *out_count) {
#ifndef HU_ENABLE_SQLITE
    (void)g; (void)alloc; (void)contact_id; (void)cid_len;
    (void)limit; (void)out; (void)out_count;
    return HU_ERR_NOT_SUPPORTED;
#else
    if (!g || !alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    struct sqlite3 *db = hu_memory_sqlite_from_graph(g);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    return negative_memory_list_sqlite(db, alloc, contact_id, cid_len, limit, out, out_count);
#endif
}

hu_error_t hu_negative_memory_list_facade(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                           const char *contact_id, size_t cid_len, size_t limit,
                                           hu_negative_memory_t **out, size_t *out_count) {
#ifndef HU_ENABLE_SQLITE
    (void)m;
    (void)alloc;
    (void)contact_id;
    (void)cid_len;
    (void)limit;
    (void)out;
    (void)out_count;
    return HU_ERR_NOT_SUPPORTED;
#else
    if (!m || !alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    struct sqlite3 *db = hu_memory_facade_sqlite_db(m);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    return negative_memory_list_sqlite(db, alloc, contact_id, cid_len, limit, out, out_count);
#endif
}

void hu_negative_memory_free(hu_allocator_t *alloc, hu_negative_memory_t *nm, size_t count) {
    /* Negative memories are POD now (text/scope/reason are inline char arrays).
     * Just free the backing array. count is informational; we use it to size
     * the free hint for the tracking allocator. */
    if (!alloc || !nm) return;
    xfree(alloc, nm, count * sizeof(*nm));
}

/* ---- world-model builder ------------------------------------------ */

static hu_error_t copy_entities(hu_allocator_t *alloc, const hu_memory_entity_row_t *src,
                                 size_t n, hu_memory_entity_row_t **out) {
    if (n == 0) {
        *out = NULL;
        return HU_OK;
    }
    hu_memory_entity_row_t *arr = xalloc(alloc, n * sizeof(*arr));
    if (!arr) return HU_ERR_OUT_OF_MEMORY;
    memset(arr, 0, n * sizeof(*arr));
    for (size_t i = 0; i < n; i++) {
        arr[i] = src[i];
        if (src[i].name && src[i].name_len > 0) {
            char *dup = xalloc(alloc, src[i].name_len + 1);
            if (!dup) {
                /* Roll back. */
                for (size_t j = 0; j < i; j++) xfree(alloc, arr[j].name, arr[j].name_len + 1);
                xfree(alloc, arr, n * sizeof(*arr));
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(dup, src[i].name, src[i].name_len);
            dup[src[i].name_len] = '\0';
            arr[i].name = dup;
        } else {
            arr[i].name = NULL;
        }
        /* metadata_json is optional; we don't carry it across (callers
         * should re-fetch entity if they need raw metadata). */
        arr[i].metadata_json = NULL;
    }
    *out = arr;
    return HU_OK;
}

static void free_entities_local(hu_allocator_t *alloc, hu_memory_entity_row_t *arr, size_t n) {
    if (!arr) return;
    for (size_t i = 0; i < n; i++) xfree(alloc, arr[i].name, arr[i].name_len + 1);
    xfree(alloc, arr, n * sizeof(*arr));
}

hu_error_t hu_world_model_build(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                 const char *contact_id, size_t cid_len,
                                 int64_t now_ms,
                                 hu_world_model_t **out) {
    if (!m || !alloc || !contact_id || !out) return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;

    hu_world_model_t *wm = xalloc(alloc, sizeof(*wm));
    if (!wm) return HU_ERR_OUT_OF_MEMORY;
    memset(wm, 0, sizeof(*wm));

    size_t copy_len = cid_len < sizeof(wm->contact_id) - 1 ? cid_len
                                                            : sizeof(wm->contact_id) - 1;
    memcpy(wm->contact_id, contact_id, copy_len);
    wm->contact_id[copy_len] = '\0';
    wm->built_at = now_ms;
    int64_t ttl_ms = 60000;
    const char *ttl_env = getenv("HU_WORLD_MODEL_TTL_MS");
    if (ttl_env) {
        long v = strtol(ttl_env, NULL, 10);
        if (v > 0) ttl_ms = v;
    }
    wm->valid_until = now_ms + ttl_ms;

    /* Top-K entities through the facade. Goes through the v1 backend's
     * `v1_entity_read` with the BY_NAME variant unset → falls through to
     * `list_entities`. We keep using `hu_memory_facade_list_entities`
     * because it returns plain entity row pointers (no record wrap)
     * — slightly cheaper than going through the union read path.
     *
     * P4: prior version pulled the graph handle directly via
     * `hu_memory_facade_graph_handle`, which dodged the facade. The
     * convenience helper is the proper L1 seam.
     *
     * Negative memory listing and auxiliary tables (emotional residue,
     * goals) use `hu_memory_facade_sqlite_db` / `hu_negative_memory_list_facade`
     * so reads align with the W7 surface even when SQL is not yet modeled
     * as a distinct facade kind. */
    {
        hu_memory_entity_row_t *ents = NULL;
        size_t n = 0;
        /* Entity ceiling: 64. Empirically the W12 P5 planner anchor-matching
         * needs every named entity in the conversation reachable; 16 was too
         * tight and silently dropped the long-tail of mentions (the W16
         * facade-recall benchmark exposed this — "who prefers vim?" failed
         * because Vim fell off the cutoff). 64 keeps the WM snapshot small
         * (~3KB even with full row payloads) while comfortably covering
         * realistic per-contact graphs. The W9 mention-count sort still
         * applies, so the top entities are first. */
        /* W12 P9 — entity cap is now env-overridable. The default 64
         * keeps production snapshots small; benchmarks that seed many
         * named characters under one contact (locomo-facade with 1.5k
         * prompts touching ~200 unique people) need a larger window
         * because the planner's named-anchor heuristic can only see
         * entities that made the cut. Override with
         * HU_WORLD_MODEL_ENTITY_LIMIT (range [16, 8192]). */
        size_t wm_entity_limit = 64;
        const char *wm_limit_env = getenv("HU_WORLD_MODEL_ENTITY_LIMIT");
        if (wm_limit_env && *wm_limit_env) {
            long v = strtol(wm_limit_env, NULL, 10);
            if (v >= 16 && v <= 8192) wm_entity_limit = (size_t)v;
        }
        if (hu_memory_facade_list_entities(m, alloc, contact_id, cid_len, wm_entity_limit,
                                           &ents, &n) == HU_OK && n > 0) {
            hu_memory_entity_row_t *cloned = NULL;
            hu_error_t err = copy_entities(alloc, ents, n, &cloned);
            hu_memory_facade_free_listed_entities(m, alloc, ents, n);
            if (err != HU_OK) {
                hu_world_model_free(alloc, wm);
                return err;
            }
            wm->entities = cloned;
            wm->entities_count = n;
        }

        /* Top-K relations through `hu_memory_facade_read` with the AUTO
         * variant (timestamps both zero → backend dispatches to the v1
         * relation top-N listing path). */
        hu_memory_query_t rq;
        memset(&rq, 0, sizeof(rq));
        rq.kind = HU_MEM_RELATION;
        rq.variant = HU_MEMORY_QUERY_AUTO;
        rq.contact_id = contact_id;
        rq.contact_id_len = cid_len;
        rq.as.window.limit = 32;

        hu_memory_record_t *recs = NULL;
        size_t rn = 0;
        if (hu_memory_facade_read(m, &rq, alloc, &recs, &rn) == HU_OK && rn > 0) {
            hu_memory_relation_row_t *arr = xalloc(alloc, rn * sizeof(*arr));
            if (!arr) {
                hu_memory_facade_records_free(m, alloc, recs, rn);
                hu_world_model_free(alloc, wm);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memset(arr, 0, rn * sizeof(*arr));
            for (size_t i = 0; i < rn; i++) {
                hu_memory_relation_row_t *src = (hu_memory_relation_row_t *)recs[i].payload;
                if (src) arr[i] = *src;
                /* We do NOT carry context/provenance through into the world
                 * model — keep this snapshot lean. Callers that need them
                 * fetch directly via hu_memory_facade_read. */
                arr[i].context = NULL;
                arr[i].context_len = 0;
                arr[i].provenance = NULL;
                arr[i].provenance_len = 0;
            }
            hu_memory_facade_records_free(m, alloc, recs, rn);
            wm->relations = arr;
            wm->relations_count = rn;
        }

        /* Negative memory list. */
        hu_negative_memory_t *negs = NULL;
        size_t neg_n = 0;
        if (hu_negative_memory_list_facade(m, alloc, contact_id, cid_len, 32, &negs, &neg_n) ==
                HU_OK
            && neg_n > 0) {
            wm->negatives = negs;
            wm->negatives_count = neg_n;
        }
    }

    /* P2D — Emotion: pull active emotional residues for this contact and
     * fold them into a single (valence, intensity) summary. The residue
     * table is populated by F76 (`src/memory/emotional_residue.c`) every
     * turn, so by the time the world-model is built, the recent
     * emotional arc is already on disk. We weight by intensity, so a
     * single intense distress signal out-shouts a string of mild
     * positives. dominant_emotion comes from valence-banding (same
     * buckets the prompt builder uses). */
    snprintf(wm->dominant_emotion, sizeof(wm->dominant_emotion), "neutral");
    wm->arousal = 0.5f;
    wm->valence = 0.0f;
#ifdef HU_ENABLE_SQLITE
    {
        struct sqlite3 *db = hu_memory_facade_sqlite_db(m);
        if (db) {
            hu_emotional_residue_t *residues = NULL;
            size_t residue_n = 0;
            int64_t now_ts = now_ms / 1000;
            if (hu_emotional_residue_get_active(alloc, db, contact_id, cid_len,
                                                now_ts, &residues, &residue_n) == HU_OK
                && residue_n > 0) {
                double total_weight = 0.0;
                double weighted_valence = 0.0;
                double max_intensity = 0.0;
                for (size_t i = 0; i < residue_n; i++) {
                    double w = residues[i].intensity > 0.0 ? residues[i].intensity : 0.1;
                    weighted_valence += residues[i].valence * w;
                    total_weight += w;
                    if (residues[i].intensity > max_intensity)
                        max_intensity = residues[i].intensity;
                }
                if (total_weight > 0.0)
                    wm->valence = (float)(weighted_valence / total_weight);
                wm->arousal = (float)max_intensity;
                if (wm->valence >= 0.5f)
                    snprintf(wm->dominant_emotion, sizeof(wm->dominant_emotion), "joy");
                else if (wm->valence >= 0.15f)
                    snprintf(wm->dominant_emotion, sizeof(wm->dominant_emotion), "calm");
                else if (wm->valence > -0.15f)
                    snprintf(wm->dominant_emotion, sizeof(wm->dominant_emotion), "neutral");
                else if (wm->valence > -0.5f)
                    snprintf(wm->dominant_emotion, sizeof(wm->dominant_emotion), "concerned");
                else
                    snprintf(wm->dominant_emotion, sizeof(wm->dominant_emotion), "distressed");
                alloc->free(alloc->ctx, residues, residue_n * sizeof(*residues));
            }
        }
    }
#endif

    /* Active goals from the autonomous goal engine. Best-effort: if the
     * goals table doesn't exist yet the engine returns 0 rows. */
#ifdef HU_ENABLE_SQLITE
    {
        struct sqlite3 *db = hu_memory_facade_sqlite_db(m);
        if (db) {
            hu_goal_engine_t ge;
            if (hu_goal_engine_create(alloc, db, &ge) == HU_OK) {
                hu_goal_t *active = NULL;
                size_t active_n = 0;
                if (hu_goal_list_active(&ge, &active, &active_n) == HU_OK && active_n > 0) {
                    size_t cap = active_n > 8 ? 8 : active_n;
                    for (size_t i = 0; i < cap; i++) {
                        hu_active_goal_t *ag = &wm->goals[i];
                        size_t dlen = active[i].description_len;
                        if (dlen >= sizeof(ag->text))
                            dlen = sizeof(ag->text) - 1;
                        memcpy(ag->text, active[i].description, dlen);
                        ag->text[dlen] = '\0';
                        ag->salience = (float)active[i].priority;
                        ag->expressed_at = active[i].created_at;
                        ag->expires_at = active[i].deadline;
                    }
                    wm->goals_count = cap;
                    hu_goal_free(alloc, active, active_n);
                }
                hu_goal_engine_deinit(&ge);
            }
        }
    }
#endif

    /* Recent topics: derive from the top entities already loaded. Entity
     * names are a reasonable proxy for "what we've been talking about"
     * until a dedicated topic-extraction pass is added. */
    if (wm->entities && wm->entities_count > 0) {
        size_t topic_cap = wm->entities_count > 10 ? 10 : wm->entities_count;
        for (size_t i = 0; i < topic_cap; i++) {
            if (!wm->entities[i].name || wm->entities[i].name_len == 0)
                continue;
            size_t nlen = wm->entities[i].name_len;
            if (nlen >= sizeof(wm->recent_topics[0]))
                nlen = sizeof(wm->recent_topics[0]) - 1;
            memcpy(wm->recent_topics[wm->recent_topics_count], wm->entities[i].name, nlen);
            wm->recent_topics[wm->recent_topics_count][nlen] = '\0';
            wm->recent_topics_count++;
        }
    }

    /* P2D — Theory-of-mind: synthesize from negatives + top-mention
     * entities. Best-effort heuristic; goal here is RIGHT MORE OFTEN
     * THAN WRONG so the planner can sidestep known sore spots.
     *
     *   user_expects_we_cannot = '; '-joined negative-memory text
     *   user_thinks_we_are     = name of the most-mentioned entity
     *   user_expects_we_can    = empty (W12 planner territory)
     *
     * Confidence rises with corroboration: 0 signals → 0.4 floor,
     * each adds 0.1, capped at 0.7 — we never claim certainty here. */
    int signals = 0;
    {
        size_t cap = sizeof(wm->tom.user_expects_we_cannot) - 1;
        size_t off = 0;
        for (size_t i = 0; i < wm->negatives_count && off < cap; i++) {
            const char *t = wm->negatives[i].text;
            if (!t || !t[0]) continue;
            size_t tl = strlen(t);
            if (tl == 0) continue;
            if (off > 0 && off + 2 < cap) {
                wm->tom.user_expects_we_cannot[off++] = ';';
                wm->tom.user_expects_we_cannot[off++] = ' ';
            }
            size_t copy = (cap - off) < tl ? (cap - off) : tl;
            memcpy(wm->tom.user_expects_we_cannot + off, t, copy);
            off += copy;
            signals++;
        }
        wm->tom.user_expects_we_cannot[off < cap ? off : cap] = '\0';
    }
    /* P1.1 fix: user_thinks_we_are stays empty in the build path. The
     * pre-P1.1 heuristic put the most-mentioned entity name here, which
     * was wrong-by-design (that's the most-mentioned third party, not
     * the user's mental model of *us*). Persona-grounded fill happens
     * in `hu_world_model_merge_persona` so the cache stays persona-
     * version-agnostic. Same for user_expects_we_can and the new
     * interaction_style field. */
    wm->tom.user_thinks_we_are[0] = '\0';
    wm->tom.user_expects_we_can[0] = '\0';
    wm->tom.interaction_style[0] = '\0';

    float tom_mean = 0.4f + 0.1f * (float)(signals < 3 ? signals : 3);
    if (tom_mean > 0.7f) tom_mean = 0.7f;
    wm->tom.confidence = hu_belief_init(tom_mean, "wm-synth", now_ms);

    *out = wm;
    return HU_OK;
}

/* ---- M2 ↔ W9 bridge: personal model merge ----------------------- */

static const char *pm_formality_label(float f) {
    if (f < 0.33f) return "casual";
    if (f < 0.66f) return "balanced";
    return "formal";
}

static const char *pm_verbosity_label(float v) {
    if (v < 0.33f) return "terse";
    if (v < 0.66f) return "moderate";
    return "verbose";
}

void hu_world_model_merge_personal(hu_world_model_t *wm,
                                   const hu_personal_model_t *pm) {
    if (!wm || !pm) return;
    if (!hu_personal_model_has_content(pm)) return;

    /* Style summary — always overwrite from the PM since it's the
     * authoritative source for communication style. */
    if (pm->style.sample_count > 0) {
        snprintf(wm->style_summary, sizeof(wm->style_summary),
                 "%s, %s, %s emoji, avg %u chars/msg",
                 pm_formality_label(pm->style.formality),
                 pm_verbosity_label(pm->style.verbosity),
                 pm->style.emoji_frequency > 0.3f ? "uses" : "rare",
                 (unsigned)pm->style.avg_message_length);
    }

    /* Goals — append PM goals the world model doesn't already have.
     * Simple substring match avoids exact duplicates. */
    for (size_t i = 0; i < pm->goal_count && wm->goals_count < 8; i++) {
        if (!pm->goals[i].active || pm->goals[i].description[0] == '\0')
            continue;
        bool dup = false;
        for (size_t j = 0; j < wm->goals_count; j++) {
            if (strstr(wm->goals[j].text, pm->goals[i].description) ||
                strstr(pm->goals[i].description, wm->goals[j].text)) {
                dup = true;
                break;
            }
        }
        if (dup) continue;
        hu_active_goal_t *ag = &wm->goals[wm->goals_count];
        size_t dlen = strlen(pm->goals[i].description);
        if (dlen >= sizeof(ag->text)) dlen = sizeof(ag->text) - 1;
        memcpy(ag->text, pm->goals[i].description, dlen);
        ag->text[dlen] = '\0';
        ag->salience = pm->goals[i].progress > 0.0f ? pm->goals[i].progress : 0.5f;
        ag->expressed_at = pm->goals[i].created_at;
        ag->expires_at = pm->goals[i].deadline;
        wm->goals_count++;
    }

    /* Topics — merge PM topics into recent_topics (skip duplicates). */
    for (size_t i = 0; i < pm->topic_count && wm->recent_topics_count < 10; i++) {
        if (pm->topics[i].name[0] == '\0') continue;
        bool dup = false;
        for (size_t j = 0; j < wm->recent_topics_count; j++) {
            if (strcasecmp(wm->recent_topics[j], pm->topics[i].name) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) continue;
        size_t nlen = strlen(pm->topics[i].name);
        if (nlen >= sizeof(wm->recent_topics[0]))
            nlen = sizeof(wm->recent_topics[0]) - 1;
        memcpy(wm->recent_topics[wm->recent_topics_count], pm->topics[i].name, nlen);
        wm->recent_topics[wm->recent_topics_count][nlen] = '\0';
        wm->recent_topics_count++;
    }

    /* Dominant emotion — only fill when the graph left the default "neutral". */
    if (strcmp(wm->dominant_emotion, "neutral") == 0) {
        if (pm->style.humor_receptivity > 0.6f)
            snprintf(wm->dominant_emotion, sizeof(wm->dominant_emotion), "playful");
    }
}

/* ---- P1.1 + P1.2 + P1.3 — persona-grounded ToM synthesis ---------- */

/* Append `s` (length `n`) to `dst` with a "; " separator if `dst` is
 * non-empty. Truncates silently — never overflows. */
static size_t tom_append_(char *dst, size_t cap, const char *s, size_t n) {
    if (cap == 0 || !s || n == 0) return strlen(dst);
    size_t off = strlen(dst);
    if (off + 1 >= cap) return off;
    if (off > 0 && off + 2 < cap) {
        dst[off++] = ';';
        dst[off++] = ' ';
    }
    size_t copy = (cap - off - 1) < n ? (cap - off - 1) : n;
    if (copy > 0) {
        memcpy(dst + off, s, copy);
        off += copy;
    }
    dst[off] = '\0';
    return off;
}

static size_t tom_append_str_(char *dst, size_t cap, const char *s) {
    if (!s || !s[0]) return strlen(dst);
    return tom_append_(dst, cap, s, strlen(s));
}

/* Map an overlay axis to a "they expect I cannot ..." clause when the
 * axis indicates a low-tolerance posture. NULL when neutral. */
static const char *overlay_cannot_clause_(const char *axis, const char *value) {
    if (!axis || !value || !value[0]) return NULL;
    if (strcasecmp(axis, "face_saving") == 0) {
        if (strcasecmp(value, "high") == 0) return "challenge them in front of others";
    } else if (strcasecmp(axis, "disagreement_style") == 0) {
        if (strcasecmp(value, "indirect") == 0) return "disagree bluntly";
        if (strcasecmp(value, "avoidant") == 0) return "force the disagreement to a head";
    } else if (strcasecmp(axis, "silence_tolerance") == 0) {
        if (strcasecmp(value, "low") == 0) return "leave long silences without checking in";
    } else if (strcasecmp(axis, "vulnerability_tier") == 0) {
        if (strcasecmp(value, "low") == 0)  return "open with heavy emotional content";
        if (strcasecmp(value, "none") == 0) return "share vulnerable material at all";
    }
    return NULL;
}

static const char *overlay_can_clause_(const char *axis, const char *value) {
    if (!axis || !value || !value[0]) return NULL;
    if (strcasecmp(axis, "directness") == 0) {
        if (strcasecmp(value, "direct") == 0) return "give direct, unhedged answers";
        if (strcasecmp(value, "blunt") == 0)  return "be blunt without softening";
    } else if (strcasecmp(axis, "vulnerability_tier") == 0) {
        if (strcasecmp(value, "high") == 0) return "engage with vulnerable material";
        if (strcasecmp(value, "deep") == 0) return "hold space for deep emotional content";
    } else if (strcasecmp(axis, "disagreement_style") == 0) {
        if (strcasecmp(value, "direct") == 0) return "push back when I see it differently";
    }
    return NULL;
}

void hu_world_model_merge_persona(hu_world_model_t *wm,
                                  const struct hu_persona *persona,
                                  const char *channel, size_t channel_len,
                                  const struct hu_persona_delta *deltas,
                                  size_t deltas_count) {
    if (!wm || !persona) return;

    int new_signals = 0;

    /* Step 1 — user_thinks_we_are from persona identity (P1.1). */
    const char *identity = (persona->identity && persona->identity[0])
                               ? persona->identity
                               : (persona->name && persona->name[0] ? persona->name : NULL);
    if (identity) {
        size_t cap = sizeof(wm->tom.user_thinks_we_are);
        size_t ilen = strlen(identity);
        if (ilen >= cap) ilen = cap - 1;
        memcpy(wm->tom.user_thinks_we_are, identity, ilen);
        wm->tom.user_thinks_we_are[ilen] = '\0';
        new_signals++;
    }

    /* Step 2 — channel overlay (P1.2). */
    const hu_persona_overlay_t *ov = NULL;
    if (channel && channel_len > 0)
        ov = hu_persona_find_overlay(persona, channel, channel_len);
    if (ov) {
        char buf[256];
        size_t off = 0;
        size_t cap = sizeof(buf) - 1;
        if (ov->channel) {
            int n = snprintf(buf + off, cap - off, "%s", ov->channel);
            if (n > 0 && (size_t)n < cap - off) off += (size_t)n;
        }
        if (ov->formality && ov->formality[0]) {
            int n = snprintf(buf + off, cap - off, "%s%s", off ? ": " : "", ov->formality);
            if (n > 0 && (size_t)n < cap - off) off += (size_t)n;
        }
        if (ov->avg_length && ov->avg_length[0]) {
            int n = snprintf(buf + off, cap - off, "%s%s length", off ? ", " : "", ov->avg_length);
            if (n > 0 && (size_t)n < cap - off) off += (size_t)n;
        }
        if (ov->emoji_usage && ov->emoji_usage[0]) {
            int n = snprintf(buf + off, cap - off, "%semoji: %s", off ? ", " : "", ov->emoji_usage);
            if (n > 0 && (size_t)n < cap - off) off += (size_t)n;
        }
        if (ov->typing_quirks_count > 0 && ov->typing_quirks && ov->typing_quirks[0]) {
            int n = snprintf(buf + off, cap - off, "%squirk: %s", off ? ", " : "",
                             ov->typing_quirks[0]);
            if (n > 0 && (size_t)n < cap - off) off += (size_t)n;
        }
        buf[off] = '\0';
        if (off > 0) {
            size_t istyle_cap = sizeof(wm->tom.interaction_style);
            size_t copy = off < istyle_cap - 1 ? off : istyle_cap - 1;
            memcpy(wm->tom.interaction_style, buf, copy);
            wm->tom.interaction_style[copy] = '\0';
            new_signals++;
        }

        const char *cn1 = overlay_cannot_clause_("face_saving", ov->face_saving);
        const char *cn2 = overlay_cannot_clause_("disagreement_style", ov->disagreement_style);
        const char *cn3 = overlay_cannot_clause_("silence_tolerance", ov->silence_tolerance);
        const char *cn4 = overlay_cannot_clause_("vulnerability_tier", ov->vulnerability_tier);
        if (cn1) tom_append_str_(wm->tom.user_expects_we_cannot,
                                 sizeof(wm->tom.user_expects_we_cannot), cn1);
        if (cn2) tom_append_str_(wm->tom.user_expects_we_cannot,
                                 sizeof(wm->tom.user_expects_we_cannot), cn2);
        if (cn3) tom_append_str_(wm->tom.user_expects_we_cannot,
                                 sizeof(wm->tom.user_expects_we_cannot), cn3);
        if (cn4) tom_append_str_(wm->tom.user_expects_we_cannot,
                                 sizeof(wm->tom.user_expects_we_cannot), cn4);
        if (cn1 || cn2 || cn3 || cn4) new_signals++;

        const char *cp1 = overlay_can_clause_("directness", ov->directness);
        const char *cp2 = overlay_can_clause_("vulnerability_tier", ov->vulnerability_tier);
        const char *cp3 = overlay_can_clause_("disagreement_style", ov->disagreement_style);
        if (cp1) tom_append_str_(wm->tom.user_expects_we_can,
                                 sizeof(wm->tom.user_expects_we_can), cp1);
        if (cp2) tom_append_str_(wm->tom.user_expects_we_can,
                                 sizeof(wm->tom.user_expects_we_can), cp2);
        if (cp3) tom_append_str_(wm->tom.user_expects_we_can,
                                 sizeof(wm->tom.user_expects_we_can), cp3);
        if (cp1 || cp2 || cp3) new_signals++;
    }

    /* Step 3 — recent persona deltas (P1.3). Only APPLIED deltas with
     * confidence >= 0.6 are folded; pending/dropped/quarantined deltas
     * carry too little signal to drive ToM. */
    for (size_t i = 0; i < deltas_count; i++) {
        const hu_persona_delta_t *d = &deltas[i];
        if (d->status != HU_DELTA_STATUS_APPLIED) continue;
        if (d->confidence < 0.6f) continue;
        if (!d->value[0]) continue;
        switch (d->kind) {
        case HU_PERSONA_DELTA_BOUNDARY:
        case HU_PERSONA_DELTA_VOCAB_AVOID:
            tom_append_str_(wm->tom.user_expects_we_cannot,
                            sizeof(wm->tom.user_expects_we_cannot), d->value);
            new_signals++;
            break;
        case HU_PERSONA_DELTA_FORMALITY:
        case HU_PERSONA_DELTA_TONE:
        case HU_PERSONA_DELTA_LENGTH:
            tom_append_str_(wm->tom.interaction_style,
                            sizeof(wm->tom.interaction_style), d->value);
            new_signals++;
            break;
        default:
            break;
        }
    }

    /* Confidence — bump by 0.05 per new signal source, cap 0.85. */
    if (new_signals > 0) {
        float bumped = wm->tom.confidence.mean + 0.05f * (float)new_signals;
        if (bumped > 0.85f) bumped = 0.85f;
        wm->tom.confidence.mean = bumped;
    }
}

void hu_world_model_free(hu_allocator_t *alloc, hu_world_model_t *wm) {
    if (!alloc || !wm) return;
    free_entities_local(alloc, wm->entities, wm->entities_count);
    xfree(alloc, wm->relations, wm->relations_count * sizeof(*wm->relations));
    if (wm->negatives) hu_negative_memory_free(alloc, wm->negatives, wm->negatives_count);
    xfree(alloc, wm, sizeof(*wm));
}

/* ---- LRU cache ---------------------------------------------------- */

/* P2.5 — slot count is process-startup tunable via the HU_WM_CACHE_SLOTS env
 * var. Default 32 fits one household (one user + a handful of contacts);
 * group-chat / multi-tenant deployments can lift to 128/256/512. Hard cap of
 * 1024 keeps the static memory budget bounded and the linear-scan lookup
 * sub-microsecond on commodity hardware. */
#define HU_WM_CACHE_SLOTS_DEFAULT 32u
#define HU_WM_CACHE_SLOTS_MAX     1024u

/* P2.4 — cache key is `(contact_id, channel)`. The same person on Slack vs
 * SMS now gets distinct snapshots so the per-channel persona overlay
 * (P1.2) actually drives behavior. `channel == ""` (len 0) is the "any
 * channel" default and matches legacy single-key callers. */
struct wm_cache_entry {
    char contact_id[64];
    char channel[32];           /* P2.4 — channel-aware key */
    size_t channel_len;
    int64_t valid_until;
    /* Cached snapshot: stored as a deep clone so we can hand callers their
     * own copy without aliasing. */
    hu_world_model_t *wm;
    hu_allocator_t *alloc;       /* alloc used to build this entry's clone */
    int64_t last_access;
};

/* P2.5 — heap-allocated cache + telemetry counters. Initialized lazily
 * on first lookup (cache_init_once) so the env var is consulted before
 * any traffic. The init is mutex-guarded so concurrent first-loads from
 * W14 + agent loop don't double-allocate. */
static struct wm_cache_entry *s_cache = NULL;
static size_t s_cache_slots = 0;
static uint64_t s_cache_loads = 0;       /* total hu_world_model_load calls */
static uint64_t s_cache_hits = 0;        /* fresh-within-TTL hits */
static uint64_t s_cache_evictions = 0;   /* slots reclaimed under pressure */

static size_t resolve_cache_slots_(void) {
    const char *env = getenv("HU_WM_CACHE_SLOTS");
    if (!env || !env[0])
        return HU_WM_CACHE_SLOTS_DEFAULT;
    long v = strtol(env, NULL, 10);
    if (v <= 0)
        return HU_WM_CACHE_SLOTS_DEFAULT;
    if ((size_t)v > HU_WM_CACHE_SLOTS_MAX)
        return HU_WM_CACHE_SLOTS_MAX;
    return (size_t)v;
}

/* MUST be called with WM_CACHE_LOCK held. Returns true if cache is ready. */
static bool cache_init_locked_(void) {
    if (s_cache) return true;
    size_t slots = resolve_cache_slots_();
    s_cache = (struct wm_cache_entry *)calloc(slots, sizeof(*s_cache));
    if (!s_cache) return false;
    s_cache_slots = slots;
    return true;
}

static struct wm_cache_entry *cache_lookup_locked_(const char *contact_id, size_t cid_len,
                                                    const char *channel, size_t channel_len) {
    if (!s_cache) return NULL;
    for (size_t i = 0; i < s_cache_slots; i++) {
        struct wm_cache_entry *e = &s_cache[i];
        if (!e->wm) continue;
        if (strncmp(e->contact_id, contact_id, cid_len) != 0) continue;
        if (e->contact_id[cid_len] != '\0') continue;
        /* Channel must match exactly (including the empty-string default). */
        if (e->channel_len != channel_len) continue;
        if (channel_len > 0 && memcmp(e->channel, channel, channel_len) != 0) continue;
        return e;
    }
    return NULL;
}

static struct wm_cache_entry *cache_evict_slot_locked_(void) {
    /* Evict the least-recently-accessed slot. Empty slot wins; otherwise
     * the oldest occupied entry is freed in place. */
    if (!s_cache || s_cache_slots == 0) return NULL;
    struct wm_cache_entry *oldest = &s_cache[0];
    for (size_t i = 0; i < s_cache_slots; i++) {
        struct wm_cache_entry *e = &s_cache[i];
        if (!e->wm) return e;
        if (e->last_access < oldest->last_access) oldest = e;
    }
    if (oldest->wm) {
        hu_world_model_free(oldest->alloc, oldest->wm);
        oldest->wm = NULL;
        s_cache_evictions++;
    }
    return oldest;
}

/* Public telemetry getter. Any pointer can be NULL. Safe to call before
 * the cache has been touched (returns zeros + the configured slot
 * capacity). */
void hu_world_model_cache_stats(size_t *slots, uint64_t *loads, uint64_t *hits,
                                uint64_t *evictions) {
    WM_CACHE_LOCK();
    if (slots)
        *slots = s_cache ? s_cache_slots : resolve_cache_slots_();
    if (loads) *loads = s_cache_loads;
    if (hits) *hits = s_cache_hits;
    if (evictions) *evictions = s_cache_evictions;
    WM_CACHE_UNLOCK();
}

/* Test-only reset: drop all cached entries AND zero the telemetry
 * counters. Production callers should use hu_world_model_invalidate
 * (which keeps counters intact for trend monitoring). */
void hu_world_model_cache_reset_for_tests(void) {
    WM_CACHE_LOCK();
    if (s_cache) {
        for (size_t i = 0; i < s_cache_slots; i++) {
            if (s_cache[i].wm) {
                hu_world_model_free(s_cache[i].alloc, s_cache[i].wm);
                s_cache[i].wm = NULL;
            }
        }
    }
    s_cache_loads = 0;
    s_cache_hits = 0;
    s_cache_evictions = 0;
    WM_CACHE_UNLOCK();
}

static hu_world_model_t *clone_wm(hu_allocator_t *alloc, const hu_world_model_t *src) {
    hu_world_model_t *wm = xalloc(alloc, sizeof(*wm));
    if (!wm) return NULL;
    *wm = *src; /* shallow */
    /* Deep-clone owned pointers. */
    if (src->entities && src->entities_count > 0) {
        if (copy_entities(alloc, src->entities, src->entities_count, &wm->entities)
            != HU_OK) {
            xfree(alloc, wm, sizeof(*wm));
            return NULL;
        }
    } else {
        wm->entities = NULL;
    }
    if (src->relations && src->relations_count > 0) {
        size_t bytes = src->relations_count * sizeof(*src->relations);
        wm->relations = xalloc(alloc, bytes);
        if (!wm->relations) {
            free_entities_local(alloc, wm->entities, wm->entities_count);
            xfree(alloc, wm, sizeof(*wm));
            return NULL;
        }
        memcpy(wm->relations, src->relations, bytes);
        /* Relations are POD-shaped in the world model (no owned strings) so
         * a memcpy is sufficient. */
    } else {
        wm->relations = NULL;
    }
    if (src->negatives && src->negatives_count > 0) {
        size_t bytes = src->negatives_count * sizeof(*src->negatives);
        wm->negatives = xalloc(alloc, bytes);
        if (!wm->negatives) {
            free_entities_local(alloc, wm->entities, wm->entities_count);
            xfree(alloc, wm->relations, src->relations_count * sizeof(*src->relations));
            xfree(alloc, wm, sizeof(*wm));
            return NULL;
        }
        memcpy(wm->negatives, src->negatives, bytes);
    } else {
        wm->negatives = NULL;
    }
    return wm;
}

hu_error_t hu_world_model_load_with_channel(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                            const char *contact_id, size_t cid_len,
                                            const char *channel, size_t channel_len,
                                            int64_t now_ms, hu_world_model_t **out) {
    if (!m || !alloc || !contact_id || !out) return HU_ERR_INVALID_ARGUMENT;
    /* Static-key sizes must contain the contact + channel including NUL. */
    if (cid_len == 0 || cid_len >= 64) return HU_ERR_INVALID_ARGUMENT;
    if (channel_len >= 32) return HU_ERR_INVALID_ARGUMENT;
    if (!channel) channel_len = 0; /* NULL channel == "any-channel" key */

    /* Cache phase: lookup under the lock. We bump load+hit counters
     * here. Clone happens inside the lock too because the cached entry's
     * deep-clone reads its alloc/wm fields, which could race with an
     * eviction otherwise. */
    hu_world_model_t *clone = NULL;
    int64_t cached_valid_until = 0;
    bool hit = false;

    WM_CACHE_LOCK();
    if (!cache_init_locked_()) {
        WM_CACHE_UNLOCK();
        return HU_ERR_OUT_OF_MEMORY;
    }
    s_cache_loads++;
    {
        struct wm_cache_entry *entry =
            cache_lookup_locked_(contact_id, cid_len, channel, channel_len);
        if (entry && entry->valid_until > now_ms) {
            entry->last_access = now_ms;
            clone = clone_wm(alloc, entry->wm);
            cached_valid_until = entry->valid_until;
            hit = true;
            if (clone) s_cache_hits++;
        }
    }
    WM_CACHE_UNLOCK();

    if (hit && clone) {
        (void)cached_valid_until;
        *out = clone;
        return HU_OK;
    }
    if (hit && !clone) {
        /* Cache hit but clone OOM. Treat as miss — fall through to a
         * fresh build so the caller still gets data. */
    }

    /* Miss or expired: build outside the lock (build can hit SQLite for
     * tens of ms; never hold the cache lock that long). */
    hu_world_model_t *fresh = NULL;
    hu_error_t err = hu_world_model_build(m, alloc, contact_id, cid_len, now_ms, &fresh);
    if (err != HU_OK) return err;

    /* Install phase: clone the fresh build into a cache slot. We re-
     * lookup in case another thread populated the same key while we were
     * building; if so we replace it with our newer clone (newer wins on
     * `valid_until` since builds use the same TTL). */
    WM_CACHE_LOCK();
    if (cache_init_locked_()) {
        struct wm_cache_entry *entry =
            cache_lookup_locked_(contact_id, cid_len, channel, channel_len);
        struct wm_cache_entry *slot = entry ? entry : cache_evict_slot_locked_();
        if (slot) {
            if (slot->wm) {
                hu_world_model_free(slot->alloc, slot->wm);
                slot->wm = NULL;
            }
            slot->wm = clone_wm(alloc, fresh);
            if (slot->wm) {
                slot->alloc = alloc;
                memcpy(slot->contact_id, contact_id, cid_len);
                slot->contact_id[cid_len] = '\0';
                slot->channel_len = channel_len;
                if (channel_len > 0) memcpy(slot->channel, channel, channel_len);
                slot->channel[channel_len] = '\0';
                slot->valid_until = fresh->valid_until;
                slot->last_access = now_ms;
            }
        }
    }
    WM_CACHE_UNLOCK();

    /* If clone failed for caching, we still hand the fresh build to caller. */
    *out = fresh;
    return HU_OK;
}

hu_error_t hu_world_model_load(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                const char *contact_id, size_t cid_len,
                                int64_t now_ms, hu_world_model_t **out) {
    /* Back-compat: legacy callers without a channel use the empty-channel
     * default key. The bridge passes a real channel via the
     * `_with_channel` variant so per-channel persona overlays drive
     * distinct snapshots. */
    return hu_world_model_load_with_channel(m, alloc, contact_id, cid_len, NULL, 0, now_ms, out);
}

void hu_world_model_invalidate_channel(const char *contact_id, size_t cid_len,
                                       const char *channel, size_t channel_len) {
    if (!contact_id || cid_len == 0) return;
    if (!channel) channel_len = 0;
    WM_CACHE_LOCK();
    if (s_cache) {
        struct wm_cache_entry *entry =
            cache_lookup_locked_(contact_id, cid_len, channel, channel_len);
        if (entry && entry->wm) {
            hu_world_model_free(entry->alloc, entry->wm);
            entry->wm = NULL;
        }
    }
    WM_CACHE_UNLOCK();
}

void hu_world_model_invalidate(const char *contact_id, size_t cid_len) {
    /* Global flush is spelled (NULL, 0) — see graph.c teardown and tests.
     * Do NOT treat ("", 0) as global: empty-string contact_id is a valid
     * graph scope for some callers; flushing every slot with their mixed
     * allocators would corrupt the cache.
     *
     * P2.4 — for a non-NULL contact_id, this invalidates ALL channels
     * for that contact. Use hu_world_model_invalidate_channel for finer
     * granularity. The wide invalidation is the right default because
     * most writes (graph upsert, negative memory, residue) are not
     * channel-scoped at the data layer. */
    WM_CACHE_LOCK();
    if (s_cache) {
        if (!contact_id && cid_len == 0) {
            for (size_t i = 0; i < s_cache_slots; i++) {
                if (s_cache[i].wm) {
                    hu_world_model_free(s_cache[i].alloc, s_cache[i].wm);
                    s_cache[i].wm = NULL;
                }
            }
        } else if (contact_id) {
            for (size_t i = 0; i < s_cache_slots; i++) {
                struct wm_cache_entry *e = &s_cache[i];
                if (!e->wm) continue;
                if (strncmp(e->contact_id, contact_id, cid_len) != 0) continue;
                if (e->contact_id[cid_len] != '\0') continue;
                hu_world_model_free(e->alloc, e->wm);
                e->wm = NULL;
            }
        }
    }
    WM_CACHE_UNLOCK();
}
