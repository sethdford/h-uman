/* src/memory/repos/feed_items_repo_sqlite.c
 * The feed_items repository: the place this repo's feed_items SQL + the raw
 * sqlite3 handle live. Domain code (inbox.c) depends on
 * hu_feed_items_repo_t, never on this file. */
#ifdef HU_ENABLE_SQLITE
#include "human/memory/feed_items_repo.h"
#include <sqlite3.h>
#include <stdlib.h>

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

/* Idempotent: matches the canonical engine schema (src/memory/engines/sqlite.c)
 * for the columns this repo touches, plus the dedup unique index that makes the
 * INSERT OR IGNORE behave identically to the prior inline implementation. In
 * production the engine already created these (CREATE IF NOT EXISTS no-ops);
 * for an in-memory test db this bootstraps the table. */
static hu_error_t ensure_schema(sqlite3 *db) {
    hu_error_t e = run_stmt(db, "CREATE TABLE IF NOT EXISTS feed_items("
                                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                "source TEXT NOT NULL,"
                                "contact_id TEXT,"
                                "content_type TEXT NOT NULL,"
                                "content TEXT NOT NULL,"
                                "url TEXT,"
                                "ingested_at INTEGER NOT NULL,"
                                "referenced INTEGER DEFAULT 0,"
                                "cluster_id INTEGER DEFAULT NULL)");
    if (e != HU_OK)
        return e;
    return run_stmt(db, "CREATE UNIQUE INDEX IF NOT EXISTS idx_feed_items_dedup "
                        "ON feed_items(source, substr(content, 1, 200))");
}

static hu_error_t repo_record(void *ctx, const hu_feed_item_t *item) {
    if (!item || !item->source || item->source_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    repo_ctx_t *c = ctx;
    sqlite3_stmt *st = NULL;
    /* Verbatim SQL from the prior inbox.c inline helper. */
    if (sqlite3_prepare_v2(c->db,
                           "INSERT OR IGNORE INTO feed_items (source, contact_id, content_type, "
                           "content, url, ingested_at) VALUES (?1, '', 'text/plain', ?2, '', ?3)",
                           -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(st, 1, item->source, (int)item->source_len, SQLITE_STATIC);
    sqlite3_bind_text(
        st, 2, item->content && item->content_len > 0 ? item->content : item->source,
        (int)(item->content && item->content_len > 0 ? item->content_len : item->source_len),
        SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)item->ingested_at);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_IO;
}

static void repo_deinit(void *ctx) {
    repo_ctx_t *c = ctx;
    if (c)
        c->alloc->free(c->alloc->ctx, c, sizeof(*c));
}

static const hu_feed_items_repo_vtable_t k_vt = {
    .record = repo_record,
    .deinit = repo_deinit,
};

hu_error_t hu_feed_items_repo_create(hu_memory_t *mem, hu_allocator_t *alloc,
                                     hu_feed_items_repo_t *out) {
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
