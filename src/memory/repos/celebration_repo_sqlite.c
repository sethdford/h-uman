/* src/memory/repos/celebration_repo_sqlite.c
 * The celebrations repository: where this repo's SQL + the raw sqlite3 handle
 * live. Domain code (persona/celebration.c, the daemon wire) depends on
 * hu_celebration_repo_t, never on this file. Mirrors boundary_repo_sqlite.c. */
#ifdef HU_ENABLE_SQLITE
#include "human/memory/celebration_repo.h"
#include <sqlite3.h>
#include <string.h>

typedef struct {
    sqlite3 *db;
    hu_allocator_t *alloc;
} repo_ctx_t;

static hu_error_t run_stmt(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE || rc == SQLITE_ROW) ? HU_OK : HU_ERR_IO;
}

static hu_error_t ensure_schema(sqlite3 *db) {
    return run_stmt(db, "CREATE TABLE IF NOT EXISTS celebrations ("
                        " contact_id TEXT NOT NULL,"
                        " win_key TEXT NOT NULL,"
                        " kind INTEGER NOT NULL,"
                        " celebrated_at INTEGER NOT NULL,"
                        " PRIMARY KEY(contact_id, win_key));");
}

static hu_error_t repo_was_recent(void *ctx, const char *cid, size_t cid_len, const char *wk,
                                  size_t wk_len, int64_t now, int64_t window_secs, bool *out) {
    repo_ctx_t *c = ctx;
    if (out)
        *out = false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(c->db,
                           "SELECT 1 FROM celebrations WHERE contact_id=? AND win_key=? "
                           "AND celebrated_at >= ? LIMIT 1;",
                           -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(st, 1, cid, (int)cid_len, SQLITE_STATIC); /* never TRANSIENT */
    sqlite3_bind_text(st, 2, wk, (int)wk_len, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, now - window_secs);
    if (out)
        *out = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return HU_OK;
}

static hu_error_t repo_record(void *ctx, const hu_celebration_t *c) {
    repo_ctx_t *rc = ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(rc->db,
                           "INSERT INTO celebrations (contact_id, win_key, kind, celebrated_at) "
                           "VALUES (?,?,?,?) ON CONFLICT(contact_id, win_key) DO UPDATE SET "
                           "kind=excluded.kind, celebrated_at=excluded.celebrated_at;",
                           -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(st, 1, c->contact_id, (int)c->contact_id_len, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, c->win_key, (int)c->win_key_len, SQLITE_STATIC);
    sqlite3_bind_int(st, 3, c->kind);
    sqlite3_bind_int64(st, 4, c->celebrated_at);
    int rc2 = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc2 == SQLITE_DONE ? HU_OK : HU_ERR_IO;
}

static void repo_deinit(void *ctx) {
    repo_ctx_t *c = ctx;
    if (c)
        c->alloc->free(c->alloc->ctx, c, sizeof(*c));
}

static const hu_celebration_repo_vtable_t k_vt = {
    .was_recent = repo_was_recent,
    .record = repo_record,
    .deinit = repo_deinit,
};

hu_error_t hu_celebration_repo_create(hu_memory_t *mem, hu_allocator_t *alloc,
                                      hu_celebration_repo_t *out) {
    if (!mem || !alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3 *db = hu_sqlite_memory_get_db(mem); /* legal HERE — engine/repo layer */
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
