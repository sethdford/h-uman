/* src/memory/repos/memories_repo_sqlite.c
 * The memories repository: the place this repo's `memories` SQL + the raw
 * sqlite3 handle live. Domain code (promotion.c) depends on
 * hu_memories_repo_t, never on this file. The `memories` table itself is
 * owned/created by the sqlite engine, so this repo issues no DDL. */
#ifdef HU_ENABLE_SQLITE
#include "human/memory/memories_repo.h"
#include <sqlite3.h>
#include <stdlib.h>

typedef struct {
    sqlite3 *db;
    hu_allocator_t *alloc;
} repo_ctx_t;

static hu_error_t repo_promote_tier(void *ctx, const char *from_category, size_t from_len,
                                    const char *to_category, size_t to_len, size_t max_count) {
    if (!from_category || !to_category)
        return HU_ERR_INVALID_ARGUMENT;
    repo_ctx_t *c = ctx;
    /* Verbatim SQL from the prior promotion.c inline implementation. */
    const char *sql = "UPDATE memories SET category = ?1, updated_at = datetime('now') "
                      "WHERE rowid IN (SELECT rowid FROM memories WHERE category = ?2 "
                      "ORDER BY updated_at DESC LIMIT ?3)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(c->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(stmt, 1, to_category, (int)to_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, from_category, (int)from_len, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, max_count > 0 ? (sqlite3_int64)max_count : 999999);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

static void repo_deinit(void *ctx) {
    repo_ctx_t *c = ctx;
    if (c)
        c->alloc->free(c->alloc->ctx, c, sizeof(*c));
}

static const hu_memories_repo_vtable_t k_vt = {
    .promote_tier = repo_promote_tier,
    .deinit = repo_deinit,
};

hu_error_t hu_memories_repo_create(hu_memory_t *mem, hu_allocator_t *alloc,
                                   hu_memories_repo_t *out) {
    if (!mem || !alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3 *db = hu_sqlite_memory_get_db(mem); /* legal HERE — engine layer */
    if (!db)
        return HU_ERR_NOT_SUPPORTED; /* non-sqlite backend */
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
