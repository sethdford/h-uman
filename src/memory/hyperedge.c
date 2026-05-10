#include "human/memory/hyperedge.h"
#include "human/memory/graph.h"

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

static int run_ddl(struct sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK)
        return rc;
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

static hu_error_t ensure_schema(struct sqlite3 *db) {
    static const char *const stmts[] = {
        "CREATE TABLE IF NOT EXISTS hyperedges ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "contact_id TEXT NOT NULL DEFAULT '',"
        "relation_label TEXT NOT NULL,"
        "confidence_mean REAL NOT NULL DEFAULT 1.0,"
        "confidence_variance REAL NOT NULL DEFAULT 0.0,"
        "event_start INTEGER NOT NULL DEFAULT 0,"
        "event_end INTEGER NOT NULL DEFAULT 0,"
        "provenance TEXT,"
        "created_at INTEGER NOT NULL"
        ")",
        "CREATE TABLE IF NOT EXISTS hyperedge_members ("
        "hyperedge_id INTEGER NOT NULL REFERENCES hyperedges(id),"
        "entity_id INTEGER NOT NULL,"
        "role TEXT NOT NULL,"
        "PRIMARY KEY (hyperedge_id, entity_id, role)"
        ")",
        "CREATE INDEX IF NOT EXISTS idx_he_contact ON hyperedges(contact_id)",
        "CREATE INDEX IF NOT EXISTS idx_he_member ON hyperedge_members(entity_id)",
        NULL,
    };
    for (size_t i = 0; stmts[i]; i++) {
        if (run_ddl(db, stmts[i]) != SQLITE_OK)
            return HU_ERR_IO;
    }
    return HU_OK;
}

hu_error_t hu_hyperedge_upsert(hu_memory_facade_t *m, const char *contact_id, size_t cid_len,
                               const hu_hyperedge_t *he, int64_t *out_id) {
    if (!m || !he || !out_id)
        return HU_ERR_INVALID_ARGUMENT;
    if (!he->members || he->members_count == 0)
        return HU_ERR_INVALID_ARGUMENT;

    hu_graph_t *g = hu_memory_facade_graph_handle(m);
    if (!g)
        return HU_ERR_INVALID_ARGUMENT;
    struct sqlite3 *db = hu_graph_sqlite_connection(g);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    if (ensure_schema(db) != HU_OK)
        return HU_ERR_IO;

    /* Insert the hyperedge row. */
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO hyperedges"
        " (contact_id, relation_label, confidence_mean, confidence_variance,"
        "  event_start, event_end, provenance, created_at)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_text(st, 1, contact_id ? contact_id : "", (int)cid_len, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, he->relation_label, -1, SQLITE_STATIC);
    sqlite3_bind_double(st, 3, (double)he->belief.mean);
    sqlite3_bind_double(st, 4, (double)he->belief.variance);
    sqlite3_bind_int64(st, 5, he->event_start);
    sqlite3_bind_int64(st, 6, he->event_end);
    if (he->provenance)
        sqlite3_bind_text(st, 7, he->provenance, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(st, 7);
    sqlite3_bind_int64(st, 8, he->belief.last_updated);

    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        return HU_ERR_IO;

    int64_t edge_id = sqlite3_last_insert_rowid(db);
    *out_id = edge_id;

    /* Insert members. */
    const char *msql =
        "INSERT OR REPLACE INTO hyperedge_members (hyperedge_id, entity_id, role)"
        " VALUES (?, ?, ?)";
    for (size_t i = 0; i < he->members_count; i++) {
        sqlite3_stmt *mst = NULL;
        if (sqlite3_prepare_v2(db, msql, -1, &mst, NULL) != SQLITE_OK)
            return HU_ERR_IO;
        sqlite3_bind_int64(mst, 1, edge_id);
        sqlite3_bind_int64(mst, 2, he->members[i].entity_id);
        sqlite3_bind_text(mst, 3, he->members[i].role, -1, SQLITE_STATIC);
        rc = sqlite3_step(mst);
        sqlite3_finalize(mst);
        if (rc != SQLITE_DONE)
            return HU_ERR_IO;
    }

    return HU_OK;
}

static char *dup_str(hu_allocator_t *alloc, const char *s, size_t len) {
    if (!s)
        return NULL;
    char *o = alloc->alloc(alloc->ctx, len + 1);
    if (!o)
        return NULL;
    memcpy(o, s, len);
    o[len] = '\0';
    return o;
}

hu_error_t hu_hyperedge_query_by_member(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                         int64_t entity_id,
                                         hu_hyperedge_t **out, size_t *out_count) {
    if (!m || !alloc || entity_id <= 0 || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    hu_graph_t *g = hu_memory_facade_graph_handle(m);
    if (!g)
        return HU_ERR_INVALID_ARGUMENT;
    struct sqlite3 *db = hu_graph_sqlite_connection(g);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    if (ensure_schema(db) != HU_OK)
        return HU_ERR_IO;

    /* Phase 1: collect distinct hyperedge IDs containing the member. */
    const char *id_sql =
        "SELECT DISTINCT h.id FROM hyperedges h"
        " JOIN hyperedge_members hm ON hm.hyperedge_id = h.id"
        " WHERE hm.entity_id = ?"
        " ORDER BY h.id";
    sqlite3_stmt *id_st = NULL;
    if (sqlite3_prepare_v2(db, id_sql, -1, &id_st, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_int64(id_st, 1, entity_id);

    /* Collect IDs into a small stack buffer first; grow if needed. */
#define MAX_EDGES 256
    int64_t edge_ids[MAX_EDGES];
    size_t n_edges = 0;
    while (sqlite3_step(id_st) == SQLITE_ROW && n_edges < MAX_EDGES) {
        edge_ids[n_edges++] = sqlite3_column_int64(id_st, 0);
    }
    sqlite3_finalize(id_st);

    if (n_edges == 0)
        return HU_OK;

    /* Allocate result array. */
    hu_hyperedge_t *arr = alloc->alloc(alloc->ctx, n_edges * sizeof(hu_hyperedge_t));
    if (!arr)
        return HU_ERR_OUT_OF_MEMORY;
    memset(arr, 0, n_edges * sizeof(hu_hyperedge_t));

    const char *he_sql =
        "SELECT id, relation_label, confidence_mean, confidence_variance,"
        " event_start, event_end, provenance, created_at"
        " FROM hyperedges WHERE id = ?";
    const char *mem_sql =
        "SELECT entity_id, role FROM hyperedge_members WHERE hyperedge_id = ? ORDER BY rowid";

    size_t loaded = 0;
    for (size_t i = 0; i < n_edges; i++) {
        hu_hyperedge_t *e = &arr[loaded];

        /* Load edge header. */
        sqlite3_stmt *he_st = NULL;
        if (sqlite3_prepare_v2(db, he_sql, -1, &he_st, NULL) != SQLITE_OK) {
            hu_hyperedges_free(alloc, arr, loaded);
            return HU_ERR_IO;
        }
        sqlite3_bind_int64(he_st, 1, edge_ids[i]);
        if (sqlite3_step(he_st) != SQLITE_ROW) {
            sqlite3_finalize(he_st);
            continue;
        }
        e->id = sqlite3_column_int64(he_st, 0);
        const char *rl = (const char *)sqlite3_column_text(he_st, 1);
        size_t rl_len = rl ? (size_t)sqlite3_column_bytes(he_st, 1) : 0;
        if (rl_len >= sizeof(e->relation_label))
            rl_len = sizeof(e->relation_label) - 1;
        if (rl) {
            memcpy(e->relation_label, rl, rl_len);
            e->relation_label[rl_len] = '\0';
        }
        e->belief.mean = (float)sqlite3_column_double(he_st, 2);
        e->belief.variance = (float)sqlite3_column_double(he_st, 3);
        e->event_start = sqlite3_column_int64(he_st, 4);
        e->event_end = sqlite3_column_int64(he_st, 5);
        const char *prov = (const char *)sqlite3_column_text(he_st, 6);
        size_t prov_len = prov ? (size_t)sqlite3_column_bytes(he_st, 6) : 0;
        e->belief.last_updated = sqlite3_column_int64(he_st, 7);
        if (prov) {
            e->provenance = dup_str(alloc, prov, prov_len);
            if (!e->provenance) {
                sqlite3_finalize(he_st);
                hu_hyperedges_free(alloc, arr, loaded);
                return HU_ERR_OUT_OF_MEMORY;
            }
        }
        sqlite3_finalize(he_st);

        /* Load members for this edge: count first. */
        sqlite3_stmt *cnt_st = NULL;
        const char *cnt_sql = "SELECT COUNT(*) FROM hyperedge_members WHERE hyperedge_id = ?";
        size_t mcount = 0;
        if (sqlite3_prepare_v2(db, cnt_sql, -1, &cnt_st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(cnt_st, 1, edge_ids[i]);
            if (sqlite3_step(cnt_st) == SQLITE_ROW)
                mcount = (size_t)sqlite3_column_int64(cnt_st, 0);
            sqlite3_finalize(cnt_st);
        }

        if (mcount > 0) {
            e->members = alloc->alloc(alloc->ctx, mcount * sizeof(hu_hyperedge_member_t));
            if (!e->members) {
                hu_hyperedges_free(alloc, arr, loaded + 1);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memset(e->members, 0, mcount * sizeof(hu_hyperedge_member_t));

            sqlite3_stmt *mem_st = NULL;
            if (sqlite3_prepare_v2(db, mem_sql, -1, &mem_st, NULL) != SQLITE_OK) {
                hu_hyperedges_free(alloc, arr, loaded + 1);
                return HU_ERR_IO;
            }
            sqlite3_bind_int64(mem_st, 1, edge_ids[i]);
            size_t mi = 0;
            while (sqlite3_step(mem_st) == SQLITE_ROW && mi < mcount) {
                e->members[mi].entity_id = sqlite3_column_int64(mem_st, 0);
                const char *role = (const char *)sqlite3_column_text(mem_st, 1);
                size_t rlen = role ? (size_t)sqlite3_column_bytes(mem_st, 1) : 0;
                if (rlen >= sizeof(e->members[mi].role))
                    rlen = sizeof(e->members[mi].role) - 1;
                if (role) {
                    memcpy(e->members[mi].role, role, rlen);
                    e->members[mi].role[rlen] = '\0';
                }
                mi++;
            }
            sqlite3_finalize(mem_st);
            e->members_count = mi;
        }

        loaded++;
    }
#undef MAX_EDGES

    *out = arr;
    *out_count = loaded;
    return HU_OK;
}

void hu_hyperedges_free(hu_allocator_t *alloc, hu_hyperedge_t *edges, size_t count) {
    if (!alloc || !edges)
        return;
    for (size_t i = 0; i < count; i++) {
        if (edges[i].members)
            alloc->free(alloc->ctx, edges[i].members,
                        edges[i].members_count * sizeof(hu_hyperedge_member_t));
        if (edges[i].provenance)
            alloc->free(alloc->ctx, edges[i].provenance, strlen(edges[i].provenance) + 1);
    }
    alloc->free(alloc->ctx, edges, 0);
}

#else /* !HU_ENABLE_SQLITE */

hu_error_t hu_hyperedge_upsert(hu_memory_facade_t *m, const char *contact_id, size_t cid_len,
                               const hu_hyperedge_t *he, int64_t *out_id) {
    (void)m; (void)contact_id; (void)cid_len; (void)he; (void)out_id;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_hyperedge_query_by_member(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                         int64_t entity_id,
                                         hu_hyperedge_t **out, size_t *out_count) {
    (void)m; (void)alloc; (void)entity_id; (void)out; (void)out_count;
    return HU_ERR_NOT_SUPPORTED;
}

void hu_hyperedges_free(hu_allocator_t *alloc, hu_hyperedge_t *edges, size_t count) {
    (void)alloc; (void)edges; (void)count;
}

#endif /* HU_ENABLE_SQLITE */
