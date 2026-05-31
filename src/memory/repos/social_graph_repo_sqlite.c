/* src/memory/repos/social_graph_repo_sqlite.c
 * The contact_relationships repository: the only place this table's SQL + the
 * raw sqlite3 handle live. Domain code (src/context/social_graph.c) depends on
 * hu_social_graph_repo_t, never on this file. SQL + schema reproduced VERBATIM
 * from the pre-migration social_graph.c so behavior is byte-identical.
 * DDD Phase 3 (one aggregate of ~21). */
#ifdef HU_ENABLE_SQLITE
#include "human/memory/social_graph_repo.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    sqlite3 *db;
    hu_allocator_t *alloc;
} repo_ctx_t;

static hu_error_t run_stmt(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE || rc == SQLITE_ROW) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

/* Schema reproduced verbatim from src/memory/engines/sqlite.c. */
static hu_error_t ensure_schema(sqlite3 *db) {
    return run_stmt(db, "CREATE TABLE IF NOT EXISTS contact_relationships("
                        "contact_id TEXT NOT NULL,"
                        "person_name TEXT NOT NULL,"
                        "role TEXT NOT NULL,"
                        "last_mentioned INTEGER,"
                        "notes TEXT,"
                        "PRIMARY KEY (contact_id, person_name))");
}

static hu_error_t repo_upsert(void *ctx, const char *contact_id, size_t cid_len, const char *name,
                              const char *role, int64_t last_mentioned, const char *notes) {
    repo_ctx_t *c = ctx;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(c->db,
                                "INSERT INTO contact_relationships(contact_id,person_name,role,"
                                "last_mentioned,notes) VALUES(?,?,?,?,?) "
                                "ON CONFLICT(contact_id,person_name) DO UPDATE SET "
                                "role=excluded.role, last_mentioned=excluded.last_mentioned, "
                                "notes=excluded.notes",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(stmt, 1, contact_id, (int)cid_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, name ? name : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, role ? role : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4, last_mentioned);
    sqlite3_bind_text(stmt, 5, notes ? notes : "", -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

static hu_error_t repo_get(void *ctx, hu_allocator_t *alloc, const char *contact_id, size_t cid_len,
                           hu_relationship_t **out, size_t *out_count) {
    repo_ctx_t *c = ctx;
    *out = NULL;
    *out_count = 0;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(c->db,
                                "SELECT person_name, role, notes "
                                "FROM contact_relationships WHERE contact_id=?",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(stmt, 1, contact_id, (int)cid_len, SQLITE_STATIC);

    size_t cap = 16;
    size_t count = 0;
    hu_relationship_t *arr =
        (hu_relationship_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_relationship_t));
    if (!arr) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            size_t old_cap = cap;
            cap *= 2;
            hu_relationship_t *n =
                (hu_relationship_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_relationship_t));
            if (!n) {
                alloc->free(alloc->ctx, arr, old_cap * sizeof(hu_relationship_t));
                sqlite3_finalize(stmt);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(n, arr, count * sizeof(hu_relationship_t));
            alloc->free(alloc->ctx, arr, old_cap * sizeof(hu_relationship_t));
            arr = n;
        }

        hu_relationship_t *r = &arr[count];
        memset(r, 0, sizeof(*r));

        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        if (name) {
            size_t len = (size_t)sqlite3_column_bytes(stmt, 0);
            if (len > 63)
                len = 63;
            snprintf(r->name, sizeof(r->name), "%.*s", (int)len, name);
        }
        const char *role = (const char *)sqlite3_column_text(stmt, 1);
        if (role) {
            size_t len = (size_t)sqlite3_column_bytes(stmt, 1);
            if (len > 31)
                len = 31;
            snprintf(r->role, sizeof(r->role), "%.*s", (int)len, role);
        }
        const char *notes = (const char *)sqlite3_column_text(stmt, 2);
        if (notes) {
            size_t len = (size_t)sqlite3_column_bytes(stmt, 2);
            if (len > 255)
                len = 255;
            snprintf(r->notes, sizeof(r->notes), "%.*s", (int)len, notes);
        }
        count++;
    }
    sqlite3_finalize(stmt);

    if (count == 0 && arr) {
        alloc->free(alloc->ctx, arr, cap * sizeof(hu_relationship_t));
        arr = NULL;
    }
    *out = arr;
    *out_count = count;
    return HU_OK;
}

static void repo_deinit(void *ctx) {
    repo_ctx_t *c = ctx;
    if (c)
        c->alloc->free(c->alloc->ctx, c, sizeof(*c));
}

static const hu_social_graph_repo_vtable_t k_vt = {
    .upsert = repo_upsert,
    .get = repo_get,
    .deinit = repo_deinit,
};

hu_error_t hu_social_graph_repo_create(hu_memory_t *mem, hu_allocator_t *alloc,
                                       hu_social_graph_repo_t *out) {
    if (!mem || !alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3 *db = hu_sqlite_memory_get_db(mem); /* legal HERE — engine layer */
    if (!db)
        return HU_ERR_NOT_SUPPORTED; /* non-sqlite backend */
    if (ensure_schema(db) != HU_OK)
        return HU_ERR_MEMORY_BACKEND;
    repo_ctx_t *c = alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    c->db = db;
    c->alloc = alloc;
    out->ctx = c;
    out->vtable = &k_vt;
    return HU_OK;
}
#endif /* HU_ENABLE_SQLITE */
