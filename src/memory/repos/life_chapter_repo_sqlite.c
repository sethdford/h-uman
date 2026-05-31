/* src/memory/repos/life_chapter_repo_sqlite.c
 * The life_chapters repository: the only place life_chapters SQL + the raw
 * sqlite3 handle live. Domain code (src/memory/life_chapters.c) depends on
 * hu_life_chapter_repo_t, never on this file. SQL is reproduced VERBATIM from
 * the pre-migration life_chapters.c so behavior is byte-identical.
 * DDD Phase 3 (one aggregate of ~22). */
#ifdef HU_ENABLE_SQLITE
#include "human/core/string.h"
#include "human/memory/life_chapter_repo.h"
#include "human/memory/sql_transaction.h"
#include <sqlite3.h>
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

/* Schema reproduced verbatim from src/memory/engines/sqlite.c (the canonical
 * life_chapters table + index), so a fresh :memory: db is self-sufficient. */
static hu_error_t ensure_schema(sqlite3 *db) {
    hu_error_t e = run_stmt(db, "CREATE TABLE IF NOT EXISTS life_chapters("
                                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                "theme TEXT,"
                                "mood TEXT,"
                                "started_at INTEGER,"
                                "ended_at INTEGER,"
                                "key_threads TEXT,"
                                "active INTEGER)");
    if (e != HU_OK)
        return e;
    return run_stmt(db,
                    "CREATE INDEX IF NOT EXISTS idx_life_chapters_active ON life_chapters(active)");
}

static char *dup_col(hu_allocator_t *alloc, sqlite3_stmt *stmt, int col) {
    const char *txt = (const char *)sqlite3_column_text(stmt, col);
    if (!txt)
        return NULL;
    size_t len = (size_t)sqlite3_column_bytes(stmt, col);
    if (len == 0)
        return NULL;
    return hu_strndup(alloc, txt, len);
}

static hu_error_t repo_get_active(void *ctx, hu_allocator_t *alloc, bool *found,
                                  hu_life_chapter_row_t *out) {
    repo_ctx_t *c = ctx;
    if (found)
        *found = false;
    if (out)
        memset(out, 0, sizeof(*out));

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(c->db,
                                "SELECT theme, mood, started_at, key_threads "
                                "FROM life_chapters WHERE active=1 "
                                "ORDER BY started_at DESC LIMIT 1",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
    }

    if (out) {
        out->theme = dup_col(alloc, stmt, 0);
        out->mood = dup_col(alloc, stmt, 1);
        out->started_at = sqlite3_column_int64(stmt, 2);
        out->key_threads_json = dup_col(alloc, stmt, 3);
    }
    if (found)
        *found = true;
    sqlite3_finalize(stmt);
    return HU_OK;
}

static hu_error_t repo_store_active(void *ctx, const char *theme, const char *mood,
                                    int64_t started_at, const char *key_threads_json) {
    repo_ctx_t *c = ctx;
    hu_sql_txn_t txn = {0};
    if (hu_sql_txn_begin(&txn, c->db) != HU_OK)
        return HU_ERR_MEMORY_BACKEND;

    /* Deactivate all previous chapters (rolled back if the insert fails). */
    sqlite3_stmt *upd = NULL;
    int rc = sqlite3_prepare_v2(c->db, "UPDATE life_chapters SET active=0", -1, &upd, NULL);
    if (rc != SQLITE_OK) {
        hu_sql_txn_rollback(&txn);
        return HU_ERR_MEMORY_BACKEND;
    }
    rc = sqlite3_step(upd);
    sqlite3_finalize(upd);
    if (rc != SQLITE_DONE) {
        hu_sql_txn_rollback(&txn);
        return HU_ERR_MEMORY_BACKEND;
    }

    sqlite3_stmt *ins = NULL;
    rc = sqlite3_prepare_v2(c->db,
                            "INSERT INTO life_chapters(theme,mood,started_at,key_threads,active) "
                            "VALUES(?,?,?,?,1)",
                            -1, &ins, NULL);
    if (rc != SQLITE_OK) {
        hu_sql_txn_rollback(&txn);
        return HU_ERR_MEMORY_BACKEND;
    }
    sqlite3_bind_text(ins, 1, theme ? theme : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, mood ? mood : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 3, started_at);
    sqlite3_bind_text(ins, 4, key_threads_json ? key_threads_json : "[]", -1, SQLITE_STATIC);
    rc = sqlite3_step(ins);
    sqlite3_finalize(ins);
    if (rc != SQLITE_DONE) {
        hu_sql_txn_rollback(&txn);
        return HU_ERR_MEMORY_BACKEND;
    }
    if (hu_sql_txn_commit(&txn) != HU_OK) {
        hu_sql_txn_rollback(&txn);
        return HU_ERR_MEMORY_BACKEND;
    }
    return HU_OK;
}

static void repo_deinit(void *ctx) {
    repo_ctx_t *c = ctx;
    if (c)
        c->alloc->free(c->alloc->ctx, c, sizeof(*c));
}

static const hu_life_chapter_repo_vtable_t k_vt = {
    .get_active = repo_get_active,
    .store_active = repo_store_active,
    .deinit = repo_deinit,
};

hu_error_t hu_life_chapter_repo_create(hu_memory_t *mem, hu_allocator_t *alloc,
                                       hu_life_chapter_repo_t *out) {
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
