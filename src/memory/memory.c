/* W7 Memory Facade — dispatcher.
 *
 * Owns nothing of substance: a vtable per kind, a pointer to the graph for the
 * v1 backend, and the routing-metadata SQLite table. All real work happens in
 * registered backends.
 */

#include "human/memory/memory.h"

#include "human/core/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

hu_error_t hu_memory__v1_backend_register(struct hu_memory_facade *m, hu_graph_t *graph);
void hu_memory__v1_backend_unregister_all(struct hu_memory_facade *m);

/* Single free point for the malloc'd ctx shared by v1 entity/relation/hyperedge.
 * Registered from memory_v1_backend.c after successful triple-bind. */
void hu_memory__v1_set_bundle_for_close(hu_memory_facade_t *m, void *ctx);

/* W7 P2C — memory_facade_routes ensure + upsert helpers. The table maps
 * each logical hu_memory_kind_t to the backend name currently bound to it.
 * When a backend changes (hu_memory_facade_register_backend replaces a slot),
 * we INSERT OR REPLACE so the on-disk route reflects the live state.
 *
 * Metadata only — the dispatcher still reads from the in-memory slot
 * table. The routes table exists for: (1) introspection at open time,
 * (2) detecting incompatible backend swaps across runs, and (3) operator
 * visibility ("which backend handled HU_MEM_KV_CACHE yesterday?"). Best-
 * effort: a missing/unwritable DB never fails open(). */
#ifdef HU_ENABLE_SQLITE
static void facade_routes_run(struct sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return;
    (void)sqlite3_step(st);
    sqlite3_finalize(st);
}

static void facade_routes_ensure(hu_graph_t *graph) {
    if (!graph) return;
    struct sqlite3 *db = hu_graph_sqlite_connection(graph);
    if (!db) return;
    facade_routes_run(db,
        "CREATE TABLE IF NOT EXISTS memory_facade_routes ("
        "kind INTEGER PRIMARY KEY,"
        "backend_name TEXT NOT NULL,"
        "registered_at INTEGER NOT NULL DEFAULT 0)");
}

static void facade_routes_upsert(hu_graph_t *graph, hu_memory_kind_t kind,
                                 const char *backend_name) {
    if (!graph || !backend_name) return;
    struct sqlite3 *db = hu_graph_sqlite_connection(graph);
    if (!db) return;
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO memory_facade_routes (kind, backend_name, registered_at)"
        " VALUES (?, ?, strftime('%s','now')*1000)"
        " ON CONFLICT(kind) DO UPDATE SET backend_name = excluded.backend_name,"
        " registered_at = strftime('%s','now')*1000";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int(st, 1, (int)kind);
    sqlite3_bind_text(st, 2, backend_name, -1, SQLITE_STATIC);
    (void)sqlite3_step(st);
    sqlite3_finalize(st);
}

static char *facade_routes_lookup(hu_graph_t *graph, hu_memory_kind_t kind,
                                   hu_allocator_t *alloc) {
    if (!graph || !alloc) return NULL;
    struct sqlite3 *db = hu_graph_sqlite_connection(graph);
    if (!db) return NULL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT backend_name FROM memory_facade_routes WHERE kind = ?",
            -1, &st, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int(st, 1, (int)kind);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *txt = (const char *)sqlite3_column_text(st, 0);
        size_t len = txt ? (size_t)sqlite3_column_bytes(st, 0) : 0;
        if (txt && len > 0) {
            out = alloc->alloc(alloc->ctx, len + 1);
            if (out) {
                memcpy(out, txt, len);
                out[len] = '\0';
            }
        }
    }
    sqlite3_finalize(st);
    return out;
}
#else
static void facade_routes_ensure(hu_graph_t *graph) { (void)graph; }
static void facade_routes_upsert(hu_graph_t *graph, hu_memory_kind_t kind,
                                 const char *backend_name) {
    (void)graph; (void)kind; (void)backend_name;
}
static char *facade_routes_lookup(hu_graph_t *graph, hu_memory_kind_t kind,
                                   hu_allocator_t *alloc) {
    (void)graph; (void)kind; (void)alloc;
    return NULL;
}
#endif

struct hu_memory_facade_slot {
    hu_memory_facade_vtable_t *vt;
    void *ctx;
};

/* W7 P14 — outstanding-read origin tracking. When a backend's `read` hook
 * returns records to a caller, we stash the (vt, ctx) pair that produced
 * them, keyed by the records pointer. `records_free` looks up this entry
 * and dispatches to the captured origin instead of the *currently bound*
 * slot. This closes a use-after-replace trap: previously a caller could
 * read from backend A, get records pointing into A's payloads, watch
 * backend B replace slot A, then call records_free — which would dispatch
 * to B's free hook and misinterpret A's payload layout.
 *
 * Lifetime contract: every successful facade_read pushes a node; every
 * facade_records_free pops the matching node by records pointer. A
 * facade_close with non-empty outstanding_reads is a programming error
 * (records leaked); we free the bookkeeping nodes and log a warning. */
struct hu_memory_facade_read_origin {
    hu_memory_record_t *records;      /* the array pointer the caller holds */
    size_t count;                     /* size of the records array */
    hu_memory_facade_vtable_t *vt;    /* backend vt that owned the array */
    void *ctx;                        /* backend ctx that owned the array */
    struct hu_memory_facade_read_origin *next;
};

struct hu_memory_facade {
    hu_allocator_t *alloc;
    hu_graph_t *graph; /* not owned; provided at open() */
    void *v1_bundle_ctx; /* malloc'd v1 shared ctx; freed once in hu_memory_facade_close */
    struct hu_memory_facade_slot slots[HU_MEM_KIND_MAX];
    int64_t last_case_rowid; /* HU_MEM_CASE insert rowid; see hu_memory_facade_last_case_rowid */
    hu_memory_audit_fn audit_fn; /* optional write/erase audit hook */
    void *audit_ctx;
    /* W7 P14: head of the outstanding-read origin list. NULL when no
     * caller currently holds records from any backend. */
    struct hu_memory_facade_read_origin *outstanding_reads;
};

void hu_memory__v1_set_bundle_for_close(hu_memory_facade_t *m, void *ctx) {
    if (m)
        m->v1_bundle_ctx = ctx;
}

static bool memory_slot_ctx_shared_elsewhere(hu_memory_facade_t *m, hu_memory_kind_t kind, void *ctx) {
    if (m == NULL || ctx == NULL)
        return false;
    for (int i = 0; i < HU_MEM_KIND_MAX; i++) {
        if (i == (int)kind)
            continue;
        if (m->slots[i].vt != NULL && m->slots[i].ctx == ctx)
            return true;
    }
    return false;
}

/* Only the v1 entity vtable's historical deinit freed the shared bundle; we
 * now free via v1_bundle_ctx in close(), so this identifies the slot whose
 * deinit must be suppressed while sibling kinds still reference the ctx. */
static bool memory_vt_is_v1_entity_owner(const hu_memory_facade_vtable_t *vt) {
    return vt != NULL && vt->name != NULL && strcmp(vt->name, "v1-entity") == 0;
}

hu_error_t hu_memory_facade_open(hu_allocator_t *alloc, hu_graph_t *graph, hu_memory_facade_t **out) {
    if (alloc == NULL || graph == NULL || out == NULL) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    hu_memory_facade_t *m = alloc->alloc(alloc->ctx, sizeof(hu_memory_facade_t));
    if (m == NULL) {
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(m, 0, sizeof(*m));
    m->alloc = alloc;
    m->graph = graph;

    /* Best-effort schema bootstrap before any backend registers. The
     * v1 backend register path will then UPSERT a row per kind it
     * binds, so by the time open() returns, the routes table reflects
     * the live in-memory dispatch table. */
    facade_routes_ensure(graph);

    hu_error_t e = hu_memory__v1_backend_register(m, graph);
    if (e != HU_OK) {
        hu_memory_facade_close(m, alloc);
        return e;
    }
    *out = m;
    return HU_OK;
}

hu_error_t hu_memory_facade_open_on_graph(hu_allocator_t *alloc, struct hu_graph *graph,
                                          hu_memory_facade_t **out) {
    return hu_memory_facade_open(alloc, (hu_graph_t *)graph, out);
}

void hu_memory_facade_close(hu_memory_facade_t *m, hu_allocator_t *alloc) {
    if (m == NULL) return;
    /* W7 P14: drain the outstanding-reads bookkeeping. Each leftover
     * node means a caller held records past the facade lifetime — that's
     * a programming bug on the caller (records would dangle anyway once
     * the backend ctxes are deinit'd below). We free only the tracking
     * nodes here, not the records themselves; the caller's records
     * pointer is now garbage but freeing it is its own concern. */
    while (m->outstanding_reads != NULL) {
        struct hu_memory_facade_read_origin *o = m->outstanding_reads;
        m->outstanding_reads = o->next;
        m->alloc->free(m->alloc->ctx, o, sizeof(*o));
    }
    hu_memory__v1_backend_unregister_all(m);
    for (int i = 0; i < HU_MEM_KIND_MAX; i++) {
        struct hu_memory_facade_slot *s = &m->slots[i];
        if (s->vt && s->vt->deinit && s->ctx) {
            if (memory_slot_ctx_shared_elsewhere(m, (hu_memory_kind_t)i, s->ctx) &&
                memory_vt_is_v1_entity_owner(s->vt)) {
                /* Relation/hyperedge still point at this ctx — owner free runs at end. */
            } else {
                s->vt->deinit(s->ctx);
            }
        }
        s->vt = NULL;
        s->ctx = NULL;
    }
    if (m->v1_bundle_ctx) {
        free(m->v1_bundle_ctx);
        m->v1_bundle_ctx = NULL;
    }
    alloc->free(alloc->ctx, m, sizeof(*m));
}

void hu_memory_facade_set_audit_hook(hu_memory_facade_t *m,
                                     hu_memory_audit_fn fn, void *ctx) {
    if (m) {
        m->audit_fn = fn;
        m->audit_ctx = ctx;
    }
}

/* W7 P14 helpers — outstanding-read origin bookkeeping. All three are
 * called only with `m != NULL`. They are deliberately allocation-light:
 * the node count is bounded by "callers holding live records right now",
 * which in practice is small (1-3 simultaneous reads). */
static bool facade_outstanding_for_ctx(const hu_memory_facade_t *m,
                                       const hu_memory_facade_vtable_t *vt,
                                       const void *ctx) {
    for (const struct hu_memory_facade_read_origin *o = m->outstanding_reads;
         o != NULL; o = o->next) {
        if (o->vt == vt && o->ctx == ctx) return true;
    }
    return false;
}

static void facade_push_outstanding(hu_memory_facade_t *m, hu_memory_record_t *records,
                                    size_t count, hu_memory_facade_vtable_t *vt, void *ctx) {
    if (records == NULL || count == 0) return; /* nothing to track */
    struct hu_memory_facade_read_origin *node =
        m->alloc->alloc(m->alloc->ctx, sizeof(*node));
    if (node == NULL) {
        /* Out-of-memory on the bookkeeping node is non-fatal: fall back
         * to the legacy "route by current slot" behavior. records_free
         * will just not find an origin entry and use slot_for(). */
        return;
    }
    node->records = records;
    node->count = count;
    node->vt = vt;
    node->ctx = ctx;
    node->next = m->outstanding_reads;
    m->outstanding_reads = node;
}

/* Pop the origin matching `records`. Returns true and fills out_vt/out_ctx
 * if found; returns false if the records pointer is not tracked (caller
 * obtained records via a path that bypassed facade_read, e.g. directly
 * from a v1 helper before W7 wiring). */
static bool facade_pop_outstanding(hu_memory_facade_t *m, hu_memory_record_t *records,
                                   hu_memory_facade_vtable_t **out_vt, void **out_ctx) {
    struct hu_memory_facade_read_origin **prev = &m->outstanding_reads;
    for (struct hu_memory_facade_read_origin *o = *prev; o != NULL;
         prev = &o->next, o = o->next) {
        if (o->records == records) {
            if (out_vt) *out_vt = o->vt;
            if (out_ctx) *out_ctx = o->ctx;
            *prev = o->next;
            m->alloc->free(m->alloc->ctx, o, sizeof(*o));
            return true;
        }
    }
    return false;
}

hu_error_t hu_memory_facade_register_backend(hu_memory_facade_t *m, hu_memory_kind_t kind,
                                             hu_memory_facade_vtable_t *vt, void *ctx) {
    if (m == NULL || vt == NULL || (int)kind < 0 || kind >= HU_MEM_KIND_MAX) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    struct hu_memory_facade_slot *s = &m->slots[kind];
    /* W7 P14: refuse to replace a backend that has outstanding reads —
     * deinit'ing its ctx would dangle the (vt, ctx) pair captured in
     * outstanding_reads, and records_free would then call into freed
     * memory. The caller must drain its reads (via records_free) first.
     *
     * We check by (vt, ctx) rather than by kind because a single ctx
     * can be bound to multiple kinds (the v1 entity/relation/hyperedge
     * triple-bind), and reads on any of those kinds count against the
     * replacement. */
    if (s->vt != NULL && s->ctx != NULL &&
        facade_outstanding_for_ctx(m, s->vt, s->ctx)) {
        return HU_ERR_MEMORY_BACKEND;
    }
    if (s->vt && s->vt->deinit && s->ctx) {
        if (!(memory_slot_ctx_shared_elsewhere(m, kind, s->ctx) &&
              memory_vt_is_v1_entity_owner(s->vt))) {
            s->vt->deinit(s->ctx);
        }
    }
    s->vt = vt;
    s->ctx = ctx;
    /* Persist the new (kind -> backend_name) edge. Best-effort: if the
     * routes table doesn't exist yet (caller didn't open via the public
     * facade) the UPSERT is a no-op. We DO NOT block registration on a
     * write failure; in-memory dispatch is the source of truth. */
    if (vt->name) {
        facade_routes_upsert(m->graph, kind, vt->name);
    }
    return HU_OK;
}

static inline struct hu_memory_facade_slot *slot_for(hu_memory_facade_t *m, hu_memory_kind_t kind) {
    if (m == NULL || (int)kind < 0 || kind >= HU_MEM_KIND_MAX) return NULL;
    struct hu_memory_facade_slot *s = &m->slots[kind];
    if (s->vt == NULL) return NULL;
    return s;
}

hu_error_t hu_memory_facade_read(hu_memory_facade_t *m, const hu_memory_query_t *q, hu_allocator_t *alloc,
                                hu_memory_record_t **out, size_t *out_count) {
    if (m == NULL || q == NULL || alloc == NULL || out == NULL || out_count == NULL) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    struct hu_memory_facade_slot *s = slot_for(m, q->kind);
    if (s == NULL || s->vt->read == NULL) return HU_ERR_NOT_SUPPORTED;
    hu_error_t e = s->vt->read(s->ctx, q, alloc, out, out_count);
    if (e == HU_OK && *out != NULL && *out_count > 0) {
        /* W7 P14: capture the (vt, ctx) that produced these records so
         * records_free routes back to the same backend even if the slot
         * is later replaced. push_outstanding is allocation-light and
         * silently no-ops on OOM (fallback path in records_free still
         * works for untracked records). */
        facade_push_outstanding(m, *out, *out_count, s->vt, s->ctx);
    }
    return e;
}

hu_error_t hu_memory_facade_write(hu_memory_facade_t *m, const hu_memory_record_t *rec) {
    if (m == NULL || rec == NULL) return HU_ERR_INVALID_ARGUMENT;
    struct hu_memory_facade_slot *s = slot_for(m, rec->kind);
    if (s == NULL || s->vt->write == NULL) return HU_ERR_NOT_SUPPORTED;
    m->last_case_rowid = 0;
    hu_error_t e = s->vt->write(s->ctx, rec);
#ifdef HU_ENABLE_SQLITE
    if (e == HU_OK && rec->kind == HU_MEM_CASE && m->graph) {
        struct sqlite3 *db = hu_graph_sqlite_connection(m->graph);
        if (db)
            m->last_case_rowid = sqlite3_last_insert_rowid(db);
    }
#endif
    if (e == HU_OK && m->audit_fn)
        m->audit_fn(m->audit_ctx, HU_MEMORY_AUDIT_WRITE, rec->kind, rec->id);
    return e;
}

int64_t hu_memory_facade_last_case_rowid(const hu_memory_facade_t *m) {
    return m ? m->last_case_rowid : 0;
}

hu_error_t hu_memory_facade_erase(hu_memory_facade_t *m, hu_memory_kind_t kind, int64_t id) {
    if (m == NULL) return HU_ERR_INVALID_ARGUMENT;
    struct hu_memory_facade_slot *s = slot_for(m, kind);
    if (s == NULL || s->vt->erase == NULL) return HU_ERR_NOT_SUPPORTED;
    hu_error_t e = s->vt->erase(s->ctx, kind, id);
    if (e == HU_OK && m->audit_fn)
        m->audit_fn(m->audit_ctx, HU_MEMORY_AUDIT_ERASE, kind, id);
    return e;
}

hu_error_t hu_memory_facade_purge_by_provenance(hu_memory_facade_t *m, const char *substring, size_t len) {
    if (m == NULL || substring == NULL || len == 0) return HU_ERR_INVALID_ARGUMENT;
    /* Fan out to every registered backend; first error wins, but we still
     * call the rest so that erasure is best-effort across backends. The W4
     * v1 helper hu_memory_erase_by_provenance does this for the graph; here
     * we extend it to every backend that implements the hook. */
    hu_error_t first_err = HU_OK;
    bool any_attempted = false;
    for (int i = 0; i < HU_MEM_KIND_MAX; i++) {
        struct hu_memory_facade_slot *s = &m->slots[i];
        if (s->vt == NULL || s->vt->erase_by_provenance == NULL) continue;
        any_attempted = true;
        hu_error_t e = s->vt->erase_by_provenance(s->ctx, substring, len);
        if (e != HU_OK && first_err == HU_OK) first_err = e;
    }
    if (!any_attempted) return HU_ERR_NOT_SUPPORTED;
    return first_err;
}

void hu_memory_facade_records_free(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                   hu_memory_record_t *r, size_t n) {
    if (m == NULL || r == NULL || n == 0) return;
    /* W7 P14: prefer the captured origin from facade_read. This routes
     * the free back to the backend that actually allocated these records,
     * even if a different backend now occupies the slot. Tracked records
     * (the common path) always hit this branch. */
    hu_memory_facade_vtable_t *origin_vt = NULL;
    void *origin_ctx = NULL;
    if (facade_pop_outstanding(m, r, &origin_vt, &origin_ctx) && origin_vt != NULL &&
        origin_vt->records_free != NULL) {
        origin_vt->records_free(origin_ctx, alloc, r, n);
        return;
    }
    /* Untracked records (legacy callers that allocate records without
     * going through facade_read, or OOM during origin-push): fall back
     * to the kind-based dispatch. Mixing kinds in one read is not
     * permitted, so r[0].kind identifies the backend. */
    struct hu_memory_facade_slot *s = slot_for(m, r[0].kind);
    if (s == NULL || s->vt->records_free == NULL) return;
    s->vt->records_free(s->ctx, alloc, r, n);
}

const char *hu_memory_facade_backend_name(hu_memory_facade_t *m, hu_memory_kind_t kind) {
    if (m == NULL || (int)kind < 0 || kind >= HU_MEM_KIND_MAX) return NULL;
    struct hu_memory_facade_slot *s = &m->slots[kind];
    return (s->vt != NULL) ? s->vt->name : NULL;
}

char *hu_memory_facade_route_lookup(hu_memory_facade_t *m, hu_memory_kind_t kind,
                                    hu_allocator_t *alloc) {
    if (m == NULL || alloc == NULL || (int)kind < 0 || kind >= HU_MEM_KIND_MAX)
        return NULL;
    return facade_routes_lookup(m->graph, kind, alloc);
}

hu_graph_t *hu_memory_facade_graph_handle(hu_memory_facade_t *m) {
    return (m != NULL) ? m->graph : NULL;
}

hu_error_t hu_memory_v1_graph_open(hu_allocator_t *alloc, const char *db_path, size_t db_path_len,
                                   struct hu_graph **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    return hu_graph_open(alloc, db_path, db_path_len, (hu_graph_t **)out);
}

void hu_memory_v1_graph_close(struct hu_graph *g, hu_allocator_t *alloc) {
    if (!g || !alloc)
        return;
    hu_graph_close((hu_graph_t *)g, alloc);
}

hu_error_t hu_memory_v1_upsert_relation_with_belief(
    struct hu_graph *g, const char *contact_id, size_t contact_id_len,
    int64_t source_id, int64_t target_id, hu_relation_type_t type,
    float weight, int64_t event_start, int64_t event_end,
    float belief_mean, float belief_variance,
    const char *context, size_t context_len,
    const char *provenance, size_t provenance_len,
    int64_t *out_id) {
    return hu_graph_upsert_relation_with_belief(
        (hu_graph_t *)g, contact_id, contact_id_len, source_id, target_id, type, weight,
        event_start, event_end, belief_mean, belief_variance, context, context_len, provenance,
        provenance_len, out_id);
}

#ifdef HU_ENABLE_SQLITE
struct sqlite3 *hu_memory_facade_sqlite_db(hu_memory_facade_t *m) {
    hu_graph_t *g = hu_memory_facade_graph_handle(m);
    return g ? hu_graph_sqlite_connection(g) : NULL;
}

struct sqlite3 *hu_memory_sqlite_from_graph(struct hu_graph *g) {
    return hu_graph_sqlite_connection((hu_graph_t *)g);
}
#endif

hu_error_t hu_memory_facade_list_entities(hu_memory_facade_t *m,
                                          hu_allocator_t *alloc,
                                          const char *contact_id,
                                          size_t cid_len,
                                          size_t limit,
                                          hu_graph_entity_t **out,
                                          size_t *out_count) {
    if (!m || !alloc || !contact_id || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    hu_graph_t *g = m->graph;
    if (!g) return HU_ERR_NOT_SUPPORTED;
    return hu_graph_list_entities(g, alloc, contact_id, cid_len, limit, out,
                                 out_count);
}

void hu_memory_facade_free_listed_entities(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                           hu_memory_entity_row_t *entities, size_t count) {
    (void)m;
    if (!alloc || !entities || count == 0)
        return;
    hu_graph_entities_free(alloc, entities, count);
}

hu_error_t hu_memory_facade_query_temporal(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                           const char *contact_id, size_t contact_id_len,
                                           int64_t from_ts, int64_t to_ts, size_t limit,
                                           char **out, size_t *out_len) {
    if (!m || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    hu_graph_t *g = m->graph;
    if (!g)
        return HU_ERR_NOT_SUPPORTED;
    const char *cid = contact_id ? contact_id : "";
    size_t cid_len = contact_id ? contact_id_len : 0;
    return hu_graph_query_temporal(g, alloc, cid, cid_len, from_ts, to_ts, limit, out, out_len);
}

hu_error_t hu_memory_facade_query_causal(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                         const char *contact_id, size_t contact_id_len,
                                         int64_t entity_id, size_t max_results, char **out,
                                         size_t *out_len) {
    if (!m || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    hu_graph_t *g = m->graph;
    if (!g)
        return HU_ERR_NOT_SUPPORTED;
    const char *cid = contact_id ? contact_id : "";
    size_t cid_len = contact_id ? contact_id_len : 0;
    return hu_graph_query_causal(g, alloc, cid, cid_len, entity_id, max_results, out, out_len);
}

hu_error_t hu_memory_facade_get_relation_belief(hu_memory_facade_t *m, int64_t relation_id,
                                                float *out_mean, float *out_variance) {
    if (!m)
        return HU_ERR_INVALID_ARGUMENT;
    hu_graph_t *g = m->graph;
    if (!g)
        return HU_ERR_NOT_SUPPORTED;
    return hu_graph_get_relation_belief(g, relation_id, out_mean, out_variance);
}

hu_error_t hu_memory_facade_set_relation_belief(hu_memory_facade_t *m, int64_t relation_id,
                                                float mean, float variance,
                                                int64_t last_seen_now_ms) {
    if (!m)
        return HU_ERR_INVALID_ARGUMENT;
    hu_graph_t *g = m->graph;
    if (!g)
        return HU_ERR_NOT_SUPPORTED;
    return hu_graph_set_relation_belief(g, relation_id, mean, variance, last_seen_now_ms);
}

/* W15 GDPR export — iterate every registered kind, read all records via
 * a window query, and write one JSON line per record. Best-effort: kinds
 * that fail to read are skipped with a diagnostic line in the output. */

static const char *kind_name(hu_memory_kind_t k) {
    switch (k) {
    case HU_MEM_ENTITY:          return "entity";
    case HU_MEM_RELATION:        return "relation";
    case HU_MEM_HYPEREDGE:       return "hyperedge";
    case HU_MEM_PERSONA_DELTA:   return "persona_delta";
    case HU_MEM_CASE:            return "case";
    case HU_MEM_CROSS_EDGE:      return "cross_edge";
    case HU_MEM_QUARANTINE:      return "quarantine";
    case HU_MEM_KV_CACHE:        return "kv_cache";
    case HU_MEM_REASONING_TRACE: return "reasoning_trace";
    case HU_MEM_BLOB:            return "blob";
    default:                     return "unknown";
    }
}

static void write_escaped(FILE *fp, const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  fputs("\\\"", fp); break;
        case '\\': fputs("\\\\", fp); break;
        case '\n': fputs("\\n", fp);  break;
        case '\r': fputs("\\r", fp);  break;
        case '\t': fputs("\\t", fp);  break;
        default:
            if (c < 0x20)
                fprintf(fp, "\\u%04x", c);
            else
                fputc(c, fp);
        }
    }
}

hu_error_t hu_memory_facade_export_json(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                        const char *output_path) {
    if (!m || !alloc || !output_path || !output_path[0])
        return HU_ERR_INVALID_ARGUMENT;

    FILE *fp = fopen(output_path, "w");
    if (!fp)
        return HU_ERR_IO;

    size_t total_exported = 0;
    /* Entity + relation rows are contact-scoped in v1 SQLite. A zeroed
     * `hu_memory_query_t` is invalid for entities and misses relations when
     * `contact_id` is unset, so enumerate DISTINCT contacts from the graph DB
     * and export those kinds per contact when SQLite is available. */
#ifdef HU_ENABLE_SQLITE
    bool exported_entity_relation_per_contact = false;
    struct sqlite3 *exdb = hu_memory_facade_sqlite_db(m);
    if (exdb) {
        static const char *const dist_sql =
            "SELECT DISTINCT contact_id FROM ("
            "SELECT contact_id FROM entities UNION SELECT contact_id FROM relations) "
            "ORDER BY 1 LIMIT 500";
        sqlite3_stmt *dst = NULL;
        if (sqlite3_prepare_v2(exdb, dist_sql, -1, &dst, NULL) != SQLITE_OK) {
            fclose(fp);
            return HU_ERR_IO;
        }
        size_t contact_passes = 0;
        while (sqlite3_step(dst) == SQLITE_ROW) {
            contact_passes++;
            const char *cid = (const char *)sqlite3_column_text(dst, 0);
            size_t cid_len = (size_t)sqlite3_column_bytes(dst, 0);
            if (cid == NULL) {
                cid = "";
                cid_len = 0;
            }

            struct hu_memory_facade_slot *se = slot_for(m, HU_MEM_ENTITY);
            if (se != NULL) {
                hu_graph_entity_t *ents = NULL;
                size_t en = 0;
                if (hu_memory_facade_list_entities(m, alloc, cid, cid_len, 10000, &ents, &en) ==
                        HU_OK &&
                    en > 0) {
                    for (size_t i = 0; i < en; i++) {
                        fprintf(fp, "{\"kind\":\"%s\",\"id\":%lld,\"confidence\":1.000",
                                kind_name(HU_MEM_ENTITY), (long long)ents[i].id);
                        fprintf(fp, ",\"event_start\":%lld,\"event_end\":0",
                                (long long)ents[i].first_seen);
                        fprintf(fp, ",\"payload_len\":%zu}\n", sizeof(hu_graph_entity_t));
                        total_exported++;
                    }
                    hu_graph_entities_free(alloc, ents, en);
                }
            }

            hu_memory_query_t rq;
            memset(&rq, 0, sizeof(rq));
            rq.kind = HU_MEM_RELATION;
            /* AUTO falls through to top-N (window timestamps both zero). */
            rq.variant = HU_MEMORY_QUERY_AUTO;
            rq.contact_id = cid;
            rq.contact_id_len = cid_len;
            rq.as.window.limit = 10000;
            hu_memory_record_t *recs = NULL;
            size_t rn = 0;
            hu_error_t re = hu_memory_facade_read(m, &rq, alloc, &recs, &rn);
            if (re == HU_OK && rn > 0) {
                for (size_t i = 0; i < rn; i++) {
                    const hu_memory_record_t *r = &recs[i];
                    fprintf(fp, "{\"kind\":\"%s\",\"id\":%lld,\"confidence\":%.3f",
                            kind_name(r->kind), (long long)r->id, (double)r->confidence);
                    if (r->provenance && r->provenance_len > 0) {
                        fputs(",\"provenance\":\"", fp);
                        write_escaped(fp, r->provenance, r->provenance_len);
                        fputc('"', fp);
                    }
                    fprintf(fp, ",\"event_start\":%lld,\"event_end\":%lld",
                            (long long)r->event_start, (long long)r->event_end);
                    fprintf(fp, ",\"payload_len\":%zu}\n", r->payload_len);
                    total_exported++;
                }
                hu_memory_facade_records_free(m, alloc, recs, rn);
            } else if (recs != NULL) {
                hu_memory_facade_records_free(m, alloc, recs, rn);
            }
        }
        sqlite3_finalize(dst);
        /* Only skip the generic entity/relation pass when this path actually
         * emitted rows. Otherwise (zero DISTINCT rows, step errors, empty
         * reads) the flag must stay false or GDPR export becomes an empty file. */
        exported_entity_relation_per_contact =
            (contact_passes > 0 && total_exported > 0);
    }
#else
    bool exported_entity_relation_per_contact = false;
#endif

    for (int ki = 0; ki < HU_MEM_KIND_MAX; ki++) {
        hu_memory_kind_t kind = (hu_memory_kind_t)ki;
        if (exported_entity_relation_per_contact &&
            (kind == HU_MEM_ENTITY || kind == HU_MEM_RELATION)) {
            continue;
        }
        struct hu_memory_facade_slot *s = slot_for(m, kind);
        if (!s || !s->vt->read)
            continue;

        hu_memory_query_t q;
        memset(&q, 0, sizeof(q));
        q.kind = kind;
        q.variant = HU_MEMORY_QUERY_WINDOW;
        q.as.window.from_ts = 0;
        q.as.window.to_ts = INT64_MAX;
        q.as.window.limit = 10000;

        hu_memory_record_t *recs = NULL;
        size_t count = 0;
        hu_error_t e = s->vt->read(s->ctx, &q, alloc, &recs, &count);
        if (e != HU_OK || count == 0) {
            if (recs)
                hu_memory_facade_records_free(m, alloc, recs, count);
            continue;
        }

        for (size_t i = 0; i < count; i++) {
            const hu_memory_record_t *r = &recs[i];
            fprintf(fp, "{\"kind\":\"%s\",\"id\":%lld,\"confidence\":%.3f",
                    kind_name(r->kind), (long long)r->id, (double)r->confidence);
            if (r->provenance && r->provenance_len > 0) {
                fputs(",\"provenance\":\"", fp);
                write_escaped(fp, r->provenance, r->provenance_len);
                fputc('"', fp);
            }
            fprintf(fp, ",\"event_start\":%lld,\"event_end\":%lld",
                    (long long)r->event_start, (long long)r->event_end);
            fprintf(fp, ",\"payload_len\":%zu}\n", r->payload_len);
            total_exported++;
        }
        hu_memory_facade_records_free(m, alloc, recs, count);
    }
    fclose(fp);
    (void)total_exported;
    return HU_OK;
}
