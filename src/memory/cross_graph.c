#include "human/memory/cross_graph.h"

#ifdef HU_ENABLE_SQLITE
#include "human/core/string.h"
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
        "CREATE TABLE IF NOT EXISTS cross_edges ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "contact_id TEXT NOT NULL DEFAULT '',"
        "src_graph TEXT NOT NULL,"
        "src_id INTEGER NOT NULL,"
        "dst_graph TEXT NOT NULL,"
        "dst_id INTEGER NOT NULL,"
        "relation TEXT NOT NULL,"
        "confidence REAL NOT NULL DEFAULT 1.0,"
        "event_start INTEGER NOT NULL DEFAULT 0,"
        "event_end INTEGER NOT NULL DEFAULT 0,"
        "weight REAL NOT NULL DEFAULT 1.0,"
        "UNIQUE(contact_id, src_graph, src_id, dst_graph, dst_id, relation))",
        "CREATE INDEX IF NOT EXISTS idx_xedge_src ON cross_edges(contact_id, src_graph, src_id)",
        "CREATE INDEX IF NOT EXISTS idx_xedge_dst ON cross_edges(contact_id, dst_graph, dst_id)",
        "CREATE INDEX IF NOT EXISTS idx_xedge_window ON cross_edges(event_start, event_end)",
        NULL,
    };
    for (size_t i = 0; stmts[i]; i++) {
        if (run_ddl(db, stmts[i]) != SQLITE_OK)
            return HU_ERR_IO;
    }
    return HU_OK;
}

hu_error_t hu_cross_edge_upsert(hu_graph_t *g, const char *contact_id, size_t contact_id_len,
                                const char *src_graph, int64_t src_id, const char *dst_graph,
                                int64_t dst_id, const char *relation, float confidence,
                                int64_t event_start, int64_t event_end, float weight) {
    if (!g || !src_graph || !dst_graph || !relation || src_id <= 0 || dst_id <= 0)
        return HU_ERR_INVALID_ARGUMENT;
    struct sqlite3 *db = hu_graph_sqlite_connection(g);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    if (ensure_schema(db) != HU_OK)
        return HU_ERR_IO;

    sqlite3_stmt *st = NULL;
    const char *sql = "INSERT INTO cross_edges"
                      " (contact_id, src_graph, src_id, dst_graph, dst_id, relation, confidence,"
                      "  event_start, event_end, weight) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
                      "ON CONFLICT(contact_id, src_graph, src_id, dst_graph, dst_id, relation) "
                      "DO UPDATE SET confidence = excluded.confidence, "
                      "event_start = excluded.event_start, event_end = excluded.event_end, "
                      "weight = (weight + excluded.weight) / 2.0";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_text(st, 1, contact_id ? contact_id : "", (int)contact_id_len, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, src_graph, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, src_id);
    sqlite3_bind_text(st, 4, dst_graph, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 5, dst_id);
    sqlite3_bind_text(st, 6, relation, -1, SQLITE_STATIC);
    sqlite3_bind_double(st, 7, (double)(confidence < 0 ? 1.0f : confidence));
    sqlite3_bind_int64(st, 8, event_start);
    sqlite3_bind_int64(st, 9, event_end);
    sqlite3_bind_double(st, 10, (double)(weight <= 0 ? 1.0f : weight));

    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_IO;
}

/* Heap layout: each row's string fields point into a single trailing buffer
 * allocated alongside the array, so hu_cross_edges_free can free both with one
 * call per row. Implementation detail: we copy strings into per-row malloc'd
 * blocks for simplicity — the allocator handles per-block free. */
typedef struct row_strings {
    char *src_graph;
    char *dst_graph;
    char *relation;
    size_t src_graph_len;
    size_t dst_graph_len;
    size_t relation_len;
} row_strings_t;

static char *dup_(hu_allocator_t *alloc, const char *s, size_t len) {
    if (!s)
        return NULL;
    char *o = alloc->alloc(alloc->ctx, len + 1);
    if (!o)
        return NULL;
    memcpy(o, s, len);
    o[len] = '\0';
    return o;
}

hu_error_t hu_cross_graph_traverse(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                   size_t contact_id_len, const char *start_graph,
                                   int64_t start_id, size_t max_hops, size_t max_results,
                                   int64_t event_window_start, int64_t event_window_end,
                                   hu_cross_edge_t **out, size_t *out_count) {
    (void)max_hops; /* MVP: 1-hop only. Multi-hop adds bounded BFS in W3 follow-up. */
    if (!g || !alloc || !start_graph || start_id <= 0 || !out || !out_count || max_results == 0)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    struct sqlite3 *db = hu_graph_sqlite_connection(g);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    if (ensure_schema(db) != HU_OK)
        return HU_ERR_IO;

    /* Window predicate matches W1: event_end = 0 means "still true". A row
     * whose event window ended exactly at window_start no longer overlaps. */
    const bool windowed = (event_window_start > 0 || event_window_end > 0);
    const char *sql_unbounded =
        "SELECT id, src_graph, src_id, dst_graph, dst_id, relation, confidence, event_start,"
        " event_end, weight FROM cross_edges "
        "WHERE contact_id = ? AND src_graph = ? AND src_id = ? "
        "ORDER BY weight DESC, event_start DESC LIMIT ?";
    const char *sql_windowed =
        "SELECT id, src_graph, src_id, dst_graph, dst_id, relation, confidence, event_start,"
        " event_end, weight FROM cross_edges "
        "WHERE contact_id = ? AND src_graph = ? AND src_id = ? "
        "AND event_start <= ? AND (event_end = 0 OR event_end > ?) "
        "ORDER BY weight DESC, event_start DESC LIMIT ?";

    sqlite3_stmt *st = NULL;
    const char *sql = windowed ? sql_windowed : sql_unbounded;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_text(st, 1, contact_id ? contact_id : "", (int)contact_id_len, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, start_graph, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, start_id);
    if (windowed) {
        int64_t we = event_window_end > 0 ? event_window_end : INT64_MAX;
        int64_t ws = event_window_start > 0 ? event_window_start : 0;
        sqlite3_bind_int64(st, 4, we);
        sqlite3_bind_int64(st, 5, ws);
        sqlite3_bind_int64(st, 6, (int64_t)max_results);
    } else {
        sqlite3_bind_int64(st, 4, (int64_t)max_results);
    }

    hu_cross_edge_t *arr = alloc->alloc(alloc->ctx, max_results * sizeof(hu_cross_edge_t));
    if (!arr) {
        sqlite3_finalize(st);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(arr, 0, max_results * sizeof(hu_cross_edge_t));

    size_t n = 0;
    while (sqlite3_step(st) == SQLITE_ROW && n < max_results) {
        hu_cross_edge_t *e = &arr[n];
        e->id = sqlite3_column_int64(st, 0);
        const char *sg = (const char *)sqlite3_column_text(st, 1);
        size_t sg_len = sg ? (size_t)sqlite3_column_bytes(st, 1) : 0;
        e->src_id = sqlite3_column_int64(st, 2);
        const char *dg = (const char *)sqlite3_column_text(st, 3);
        size_t dg_len = dg ? (size_t)sqlite3_column_bytes(st, 3) : 0;
        e->dst_id = sqlite3_column_int64(st, 4);
        const char *rel = (const char *)sqlite3_column_text(st, 5);
        size_t rel_len = rel ? (size_t)sqlite3_column_bytes(st, 5) : 0;
        e->confidence = (float)sqlite3_column_double(st, 6);
        e->event_start = sqlite3_column_int64(st, 7);
        e->event_end = sqlite3_column_int64(st, 8);
        e->weight = (float)sqlite3_column_double(st, 9);
        e->src_graph = dup_(alloc, sg, sg_len);
        e->dst_graph = dup_(alloc, dg, dg_len);
        e->relation = dup_(alloc, rel, rel_len);
        if ((sg && !e->src_graph) || (dg && !e->dst_graph) || (rel && !e->relation)) {
            sqlite3_finalize(st);
            hu_cross_edges_free(alloc, arr, n + 1);
            return HU_ERR_OUT_OF_MEMORY;
        }
        n++;
    }
    sqlite3_finalize(st);

    *out = arr;
    *out_count = n;
    return HU_OK;
}

void hu_cross_edges_free(hu_allocator_t *alloc, hu_cross_edge_t *edges, size_t count) {
    if (!alloc || !edges)
        return;
    for (size_t i = 0; i < count; i++) {
        if (edges[i].src_graph)
            alloc->free(alloc->ctx, (void *)edges[i].src_graph, strlen(edges[i].src_graph) + 1);
        if (edges[i].dst_graph)
            alloc->free(alloc->ctx, (void *)edges[i].dst_graph, strlen(edges[i].dst_graph) + 1);
        if (edges[i].relation)
            alloc->free(alloc->ctx, (void *)edges[i].relation, strlen(edges[i].relation) + 1);
    }
    /* Caller passed max_results capacity into the original alloc; we don't
     * track that here, so pass 0 — system allocator ignores size hint. */
    alloc->free(alloc->ctx, edges, 0);
}

#else /* !HU_ENABLE_SQLITE */

hu_error_t hu_cross_edge_upsert(hu_graph_t *g, const char *contact_id, size_t contact_id_len,
                                const char *src_graph, int64_t src_id, const char *dst_graph,
                                int64_t dst_id, const char *relation, float confidence,
                                int64_t event_start, int64_t event_end, float weight) {
    (void)g;
    (void)contact_id;
    (void)contact_id_len;
    (void)src_graph;
    (void)src_id;
    (void)dst_graph;
    (void)dst_id;
    (void)relation;
    (void)confidence;
    (void)event_start;
    (void)event_end;
    (void)weight;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_cross_graph_traverse(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                   size_t contact_id_len, const char *start_graph,
                                   int64_t start_id, size_t max_hops, size_t max_results,
                                   int64_t event_window_start, int64_t event_window_end,
                                   hu_cross_edge_t **out, size_t *out_count) {
    (void)g;
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
    (void)start_graph;
    (void)start_id;
    (void)max_hops;
    (void)max_results;
    (void)event_window_start;
    (void)event_window_end;
    (void)out;
    (void)out_count;
    return HU_ERR_NOT_SUPPORTED;
}

void hu_cross_edges_free(hu_allocator_t *alloc, hu_cross_edge_t *edges, size_t count) {
    (void)alloc;
    (void)edges;
    (void)count;
}

#endif
