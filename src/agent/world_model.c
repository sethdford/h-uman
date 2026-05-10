/* W9 — Per-contact world model: synthesizer, cache, negative-memory CRUD.
 *
 * The world model is a lightweight derived view: entities (top-K by mention),
 * their relations, current emotional snapshot, active goals, negative memory,
 * theory-of-mind, and recent topics. Built lazily from the W7 facade and
 * cached behind a small process-local LRU. Every hu_memory_write that
 * targets a contact_id calls hu_world_model_invalidate so the next load
 * rebuilds.
 *
 * Persona integration (full ToM synthesis from persona traits + deltas) is
 * deliberately deferred to a follow-up commit within W9 — the persona-
 * synthesizer is best added once the per-contact persona overlay loader is
 * exposed in a more uniform shape (see docs/plans/.../w9-world-model.md).
 */

#include "human/agent/world_model.h"

#include "human/core/error.h"
#include "human/memory/graph.h"
#include "human/memory/memory.h"

#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
extern struct sqlite3 *hu_graph__db_handle(hu_graph_t *g);
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

#endif /* HU_ENABLE_SQLITE */

hu_error_t hu_negative_memory_add(hu_graph_t *g, const char *contact_id, size_t cid_len,
                                   const hu_negative_memory_t *nm, int64_t *out_id) {
#ifndef HU_ENABLE_SQLITE
    (void)g; (void)contact_id; (void)cid_len; (void)nm; (void)out_id;
    return HU_ERR_NOT_SUPPORTED;
#else
    if (!g || !nm) return HU_ERR_INVALID_ARGUMENT;
    if (nm->text[0] == '\0') return HU_ERR_INVALID_ARGUMENT;
    struct sqlite3 *db = hu_graph__db_handle(g);
    if (!db) return HU_ERR_INVALID_ARGUMENT;
    if (ensure_negative_memory_schema(db) != HU_OK) return HU_ERR_IO;

    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO negative_memory(contact_id, text, scope, reason, "
        " confidence_mean, confidence_variance, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return HU_ERR_IO;
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
    if (rc != SQLITE_DONE) return HU_ERR_IO;
    if (out_id) *out_id = id;
    return HU_OK;
#endif
}

hu_error_t hu_negative_memory_list(hu_graph_t *g, hu_allocator_t *alloc,
                                    const char *contact_id, size_t cid_len,
                                    size_t limit, hu_negative_memory_t **out,
                                    size_t *out_count) {
#ifndef HU_ENABLE_SQLITE
    (void)g; (void)alloc; (void)contact_id; (void)cid_len;
    (void)limit; (void)out; (void)out_count;
    return HU_ERR_NOT_SUPPORTED;
#else
    if (!g || !alloc || !out || !out_count) return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    struct sqlite3 *db = hu_graph__db_handle(g);
    if (!db) return HU_ERR_INVALID_ARGUMENT;
    if (ensure_negative_memory_schema(db) != HU_OK) return HU_ERR_IO;

    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT id, text, scope, reason, confidence_mean, confidence_variance, "
        "       created_at FROM negative_memory "
        "WHERE contact_id = ? ORDER BY created_at DESC LIMIT ?";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return HU_ERR_IO;
    sqlite3_bind_text(st, 1, contact_id ? contact_id : "", (int)cid_len, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, (int)(limit > 0 ? limit : 32));

    /* Collect rows into a growable array. We cap at the requested limit so
     * we know the upper bound; for simplicity allocate to the cap. */
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

static hu_error_t copy_entities(hu_allocator_t *alloc, const hu_graph_entity_t *src,
                                 size_t n, hu_graph_entity_t **out) {
    if (n == 0) {
        *out = NULL;
        return HU_OK;
    }
    hu_graph_entity_t *arr = xalloc(alloc, n * sizeof(*arr));
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

static void free_entities_local(hu_allocator_t *alloc, hu_graph_entity_t *arr, size_t n) {
    if (!arr) return;
    for (size_t i = 0; i < n; i++) xfree(alloc, arr[i].name, arr[i].name_len + 1);
    xfree(alloc, arr, n * sizeof(*arr));
}

hu_error_t hu_world_model_build(hu_memory_t *m, hu_allocator_t *alloc,
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
    wm->valid_until = now_ms + 60 * 1000; /* 60s default TTL */

    /* Top-K entities via graph helper (list_entities). */
    hu_graph_t *g = hu_memory_graph_handle(m);
    if (g) {
        hu_graph_entity_t *ents = NULL;
        size_t n = 0;
        if (hu_graph_list_entities(g, alloc, contact_id, cid_len, 16, &ents, &n) == HU_OK
            && n > 0) {
            hu_graph_entity_t *cloned = NULL;
            hu_error_t err = copy_entities(alloc, ents, n, &cloned);
            hu_graph_entities_free(alloc, ents, n);
            if (err != HU_OK) {
                hu_world_model_free(alloc, wm);
                return err;
            }
            wm->entities = cloned;
            wm->entities_count = n;
        }

        /* Top-K relations via list_relations. */
        hu_graph_relation_t *rels = NULL;
        size_t rn = 0;
        if (hu_graph_list_relations(g, alloc, contact_id, cid_len, 32, &rels, &rn) == HU_OK
            && rn > 0) {
            /* Clone relations the same way as entities (we own the text). */
            hu_graph_relation_t *arr = xalloc(alloc, rn * sizeof(*arr));
            if (!arr) {
                hu_graph_relations_free(alloc, rels, rn);
                hu_world_model_free(alloc, wm);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memset(arr, 0, rn * sizeof(*arr));
            for (size_t i = 0; i < rn; i++) {
                arr[i] = rels[i];
                /* We do NOT carry context/provenance through into the world
                 * model — keep this snapshot lean. Callers that need them
                 * fetch directly via hu_memory_read. */
                arr[i].context = NULL;
                arr[i].context_len = 0;
                arr[i].provenance = NULL;
                arr[i].provenance_len = 0;
            }
            hu_graph_relations_free(alloc, rels, rn);
            wm->relations = arr;
            wm->relations_count = rn;
        }

        /* Negative memory list. */
        hu_negative_memory_t *negs = NULL;
        size_t neg_n = 0;
        if (hu_negative_memory_list(g, alloc, contact_id, cid_len, 32, &negs, &neg_n) == HU_OK
            && neg_n > 0) {
            wm->negatives = negs;
            wm->negatives_count = neg_n;
        }
    }

    /* Emotional state, goals, ToM, recent topics: stub for now. Persona-
     * driven synthesis is the W9 follow-up. */
    strcpy(wm->dominant_emotion, "neutral");
    wm->arousal = 0.5f;
    wm->valence = 0.0f;

    /* ToM placeholder so consumers can read fields safely. */
    wm->tom.confidence = hu_belief_init(0.5f, "stub", now_ms);
    strcpy(wm->tom.user_thinks_we_are, "");
    strcpy(wm->tom.user_expects_we_can, "");
    strcpy(wm->tom.user_expects_we_cannot, "");

    *out = wm;
    return HU_OK;
}

void hu_world_model_free(hu_allocator_t *alloc, hu_world_model_t *wm) {
    if (!alloc || !wm) return;
    free_entities_local(alloc, wm->entities, wm->entities_count);
    xfree(alloc, wm->relations, wm->relations_count * sizeof(*wm->relations));
    if (wm->negatives) hu_negative_memory_free(alloc, wm->negatives, wm->negatives_count);
    xfree(alloc, wm, sizeof(*wm));
}

/* ---- LRU cache ---------------------------------------------------- */

#define HU_WM_CACHE_SLOTS 32

struct wm_cache_entry {
    char contact_id[64];
    int64_t valid_until;
    /* Cached snapshot: stored as a deep clone so we can hand callers their
     * own copy without aliasing. */
    hu_world_model_t *wm;
    hu_allocator_t *alloc;       /* alloc used to build this entry's clone */
    int64_t last_access;
};

/* Process-local cache. Mutex omitted: agent loop is single-threaded
 * today; tests are single-threaded. Add a mutex when concurrency lands. */
static struct wm_cache_entry s_cache[HU_WM_CACHE_SLOTS];

static struct wm_cache_entry *cache_lookup(const char *contact_id, size_t cid_len) {
    for (size_t i = 0; i < HU_WM_CACHE_SLOTS; i++) {
        struct wm_cache_entry *e = &s_cache[i];
        if (!e->wm) continue;
        if (strncmp(e->contact_id, contact_id, cid_len) == 0
            && e->contact_id[cid_len] == '\0') {
            return e;
        }
    }
    return NULL;
}

static struct wm_cache_entry *cache_evict_slot(void) {
    /* Evict the least-recently-accessed empty-or-oldest slot. Empty wins. */
    struct wm_cache_entry *oldest = &s_cache[0];
    for (size_t i = 0; i < HU_WM_CACHE_SLOTS; i++) {
        struct wm_cache_entry *e = &s_cache[i];
        if (!e->wm) return e;
        if (e->last_access < oldest->last_access) oldest = e;
    }
    if (oldest->wm) {
        hu_world_model_free(oldest->alloc, oldest->wm);
        oldest->wm = NULL;
    }
    return oldest;
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

hu_error_t hu_world_model_load(hu_memory_t *m, hu_allocator_t *alloc,
                                const char *contact_id, size_t cid_len,
                                int64_t now_ms, hu_world_model_t **out) {
    if (!m || !alloc || !contact_id || !out) return HU_ERR_INVALID_ARGUMENT;
    if (cid_len >= sizeof(s_cache[0].contact_id)) return HU_ERR_INVALID_ARGUMENT;

    struct wm_cache_entry *entry = cache_lookup(contact_id, cid_len);
    if (entry && entry->valid_until > now_ms) {
        entry->last_access = now_ms;
        hu_world_model_t *cloned = clone_wm(alloc, entry->wm);
        if (!cloned) return HU_ERR_OUT_OF_MEMORY;
        *out = cloned;
        return HU_OK;
    }

    /* Miss or expired: build a fresh one. */
    hu_world_model_t *fresh = NULL;
    hu_error_t err = hu_world_model_build(m, alloc, contact_id, cid_len, now_ms, &fresh);
    if (err != HU_OK) return err;

    /* Store a clone in the cache. We keep the cache's allocator independent
     * of the caller's so caller can free without affecting the cache. For
     * v1 simplicity, the cache uses the same allocator the build was done
     * with — fine in single-threaded use. */
    struct wm_cache_entry *slot = entry ? entry : cache_evict_slot();
    if (slot->wm) {
        hu_world_model_free(slot->alloc, slot->wm);
        slot->wm = NULL;
    }
    slot->wm = clone_wm(alloc, fresh);
    if (slot->wm) {
        slot->alloc = alloc;
        memcpy(slot->contact_id, contact_id, cid_len);
        slot->contact_id[cid_len] = '\0';
        slot->valid_until = fresh->valid_until;
        slot->last_access = now_ms;
    }
    /* If clone failed for caching, we still hand the fresh build to caller. */
    *out = fresh;
    return HU_OK;
}

void hu_world_model_invalidate(const char *contact_id, size_t cid_len) {
    if (!contact_id || cid_len == 0) {
        /* Wildcard invalidate: drop everything. */
        for (size_t i = 0; i < HU_WM_CACHE_SLOTS; i++) {
            if (s_cache[i].wm) {
                hu_world_model_free(s_cache[i].alloc, s_cache[i].wm);
                s_cache[i].wm = NULL;
            }
        }
        return;
    }
    struct wm_cache_entry *entry = cache_lookup(contact_id, cid_len);
    if (entry && entry->wm) {
        hu_world_model_free(entry->alloc, entry->wm);
        entry->wm = NULL;
    }
}
