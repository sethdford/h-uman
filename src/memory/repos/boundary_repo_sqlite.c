/* src/memory/repos/boundary_repo_sqlite.c
 * The ONE place boundaries SQL + the raw sqlite3 handle live. Domain code
 * (protective.c) depends on hu_boundary_repo_t, never on this file. */
#ifdef HU_ENABLE_SQLITE
#include "human/memory/boundary_repo.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    sqlite3 *db;
    hu_allocator_t *alloc;
} repo_ctx_t;

/* Run a parameterless DDL/statement via prepare/step. */
static hu_error_t run_stmt(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE || rc == SQLITE_ROW) ? HU_OK : HU_ERR_IO;
}

static hu_error_t ensure_schema(sqlite3 *db) {
    return run_stmt(db, "CREATE TABLE IF NOT EXISTS boundaries ("
                        " id INTEGER PRIMARY KEY,"
                        " contact_id TEXT NOT NULL,"
                        " topic TEXT NOT NULL,"
                        " type TEXT NOT NULL,"
                        " set_at INTEGER NOT NULL,"
                        " source TEXT);");
}

static hu_error_t repo_is_boundary(void *ctx, const char *cid, size_t cid_len, const char *topic,
                                   size_t topic_len, bool *out) {
    repo_ctx_t *c = ctx;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(c->db,
                           "SELECT 1 FROM boundaries WHERE contact_id=? AND topic=? LIMIT 1;", -1,
                           &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(st, 1, cid, (int)cid_len, SQLITE_STATIC); /* never TRANSIENT */
    sqlite3_bind_text(st, 2, topic, (int)topic_len, SQLITE_STATIC);
    *out = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return HU_OK;
}

static hu_error_t repo_add(void *ctx, const hu_boundary_t *b) {
    repo_ctx_t *c = ctx;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(c->db,
                           "INSERT INTO boundaries (contact_id, topic, type, set_at, source) "
                           "VALUES (?,?,?,?,?);",
                           -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(st, 1, b->contact_id, (int)b->contact_id_len, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, b->topic, (int)b->topic_len, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, b->type, (int)b->type_len, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, b->created_at);
    sqlite3_bind_text(st, 5, b->source, (int)b->source_len, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_IO;
}

static void repo_deinit(void *ctx) {
    repo_ctx_t *c = ctx;
    if (c)
        c->alloc->free(c->alloc->ctx, c, sizeof(*c));
}

static const hu_boundary_repo_vtable_t k_vt = {
    .is_boundary = repo_is_boundary,
    .add = repo_add,
    .deinit = repo_deinit,
};

hu_error_t hu_boundary_repo_create(hu_memory_t *mem, hu_allocator_t *alloc,
                                   hu_boundary_repo_t *out) {
    if (!mem || !alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3 *db = hu_sqlite_memory_get_db(mem); /* legal HERE — engine layer */
    if (!db)
        return HU_ERR_NOT_SUPPORTED; /* non-sqlite backend */
    if (ensure_schema(db) != HU_OK)
        return HU_ERR_IO;
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
