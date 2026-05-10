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
struct sqlite3 *hu_graph__db_handle(hu_graph_t *g);
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
    struct sqlite3 *db = hu_graph__db_handle(graph);
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
    struct sqlite3 *db = hu_graph__db_handle(graph);
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
    struct sqlite3 *db = hu_graph__db_handle(graph);
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

struct hu_memory_facade {
    hu_allocator_t *alloc;
    hu_graph_t *graph; /* not owned; provided at open() */
    void *v1_bundle_ctx; /* malloc'd v1 shared ctx; freed once in hu_memory_facade_close */
    struct hu_memory_facade_slot slots[HU_MEM_KIND_MAX];
    int64_t last_case_rowid; /* HU_MEM_CASE insert rowid; see hu_memory_facade_last_case_rowid */
    hu_memory_audit_fn audit_fn; /* optional write/erase audit hook */
    void *audit_ctx;
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

void hu_memory_facade_close(hu_memory_facade_t *m, hu_allocator_t *alloc) {
    if (m == NULL) return;
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

hu_error_t hu_memory_facade_register_backend(hu_memory_facade_t *m, hu_memory_kind_t kind,
                                             hu_memory_facade_vtable_t *vt, void *ctx) {
    if (m == NULL || vt == NULL || (int)kind < 0 || kind >= HU_MEM_KIND_MAX) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    struct hu_memory_facade_slot *s = &m->slots[kind];
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
    return s->vt->read(s->ctx, q, alloc, out, out_count);
}

hu_error_t hu_memory_facade_write(hu_memory_facade_t *m, const hu_memory_record_t *rec) {
    if (m == NULL || rec == NULL) return HU_ERR_INVALID_ARGUMENT;
    struct hu_memory_facade_slot *s = slot_for(m, rec->kind);
    if (s == NULL || s->vt->write == NULL) return HU_ERR_NOT_SUPPORTED;
    m->last_case_rowid = 0;
    hu_error_t e = s->vt->write(s->ctx, rec);
#ifdef HU_ENABLE_SQLITE
    if (e == HU_OK && rec->kind == HU_MEM_CASE && m->graph) {
        struct sqlite3 *db = hu_graph__db_handle(m->graph);
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
    /* All records in a single response come from the same backend, identified
     * by the kind of the first record. Mixing kinds in one read is not
     * permitted (the API only routes a single kind per call). */
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

#ifdef HU_ENABLE_SQLITE
struct sqlite3 *hu_memory_facade_sqlite_db(hu_memory_facade_t *m) {
    hu_graph_t *g = hu_memory_facade_graph_handle(m);
    return g ? hu_graph__db_handle(g) : NULL;
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
