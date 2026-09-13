/* src/memory/repos/contact_insights_repo_sqlite.c — see the header.
 * The one place the contact_insights SQL and the raw sqlite3 handle live. */
#include "human/memory/contact_insights_repo.h"

#include "human/memory/graph_state.h" /* hu_graph_state_format_month */

#include <stdio.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include "human/memory/engines.h"
#include <sqlite3.h>

static const char *k_schema =
    "CREATE TABLE IF NOT EXISTS contact_insights ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  contact_id TEXT NOT NULL,"
    "  kind TEXT NOT NULL DEFAULT 'fact',"
    "  insight TEXT NOT NULL,"
    "  confidence REAL NOT NULL DEFAULT 0.7,"
    "  as_of_ms INTEGER NOT NULL DEFAULT 0,"
    "  source TEXT,"
    "  created_at_ms INTEGER NOT NULL,"
    "  retired_at_ms INTEGER NOT NULL DEFAULT 0"
    ");"
    /* Natural key. The INSERT OR IGNORE below relies on it: without
     * a UNIQUE index OR IGNORE inserts unconditionally (the opinions
     * table grew to 9.5 M rows that way). Kept as an explicit
     * CREATE UNIQUE INDEX so scripts/check-silent-success.sh can see
     * it; a constraint inside the split CREATE TABLE literal is
     * invisible to that line-oriented guard. */
    "CREATE UNIQUE INDEX IF NOT EXISTS ci_natural ON contact_insights(contact_id, insight);"
    "CREATE INDEX IF NOT EXISTS idx_contact_insights_contact "
    "  ON contact_insights(contact_id, retired_at_ms, as_of_ms DESC);";

static sqlite3 *db_of(hu_memory_t *mem) {
    return mem ? hu_sqlite_memory_get_db(mem) : NULL; /* legal HERE — engine layer */
}

hu_error_t hu_contact_insights_ensure_schema(hu_memory_t *mem) {
    sqlite3 *db = db_of(mem);
    if (!db)
        return mem ? HU_ERR_NOT_SUPPORTED : HU_ERR_INVALID_ARGUMENT;
    return sqlite3_exec(db, k_schema, NULL, NULL, NULL) == SQLITE_OK ? HU_OK
                                                                     : HU_ERR_MEMORY_BACKEND;
}

hu_error_t hu_contact_insights_add(hu_memory_t *mem, const char *contact_id, size_t contact_id_len,
                                   const char *kind, const char *insight, double confidence,
                                   int64_t as_of_ms, const char *source, int64_t *out_id) {
    if (!contact_id || contact_id_len == 0 || !insight || !insight[0])
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3 *db = db_of(mem);
    if (!db)
        return mem ? HU_ERR_NOT_SUPPORTED : HU_ERR_INVALID_ARGUMENT;
    hu_error_t serr = hu_contact_insights_ensure_schema(mem);
    if (serr != HU_OK)
        return serr;
    /* The UNIQUE(contact_id, insight) index makes the re-extraction of an
     * unchanged note a no-op instead of a duplicate row (the opinions table
     * reached 9.5 M rows without one — .claude/rules/reports-success-does-nothing.md). */
    const char *sql = "INSERT OR IGNORE INTO contact_insights"
                      " (contact_id, kind, insight, confidence, as_of_ms, source, created_at_ms)"
                      " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(st, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, kind && kind[0] ? kind : "fact", -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, insight, -1, SQLITE_STATIC);
    sqlite3_bind_double(st, 4, confidence);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)as_of_ms);
    if (source)
        sqlite3_bind_text(st, 6, source, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(st, 6);
    sqlite3_bind_int64(st, 7, (sqlite3_int64)(as_of_ms > 0 ? as_of_ms : 0));
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_BACKEND;
    if (out_id)
        *out_id = (int64_t)sqlite3_last_insert_rowid(db);
    return HU_OK;
}

hu_error_t hu_contact_insights_retire(hu_memory_t *mem, int64_t id, int64_t retired_at_ms) {
    sqlite3 *db = db_of(mem);
    if (!db)
        return mem ? HU_ERR_NOT_SUPPORTED : HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "UPDATE contact_insights SET retired_at_ms = ?1 WHERE id = ?2", -1,
                           &st, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)(retired_at_ms > 0 ? retired_at_ms : 1));
    sqlite3_bind_int64(st, 2, (sqlite3_int64)id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

hu_error_t hu_contact_insights_render(hu_memory_t *mem, hu_allocator_t *alloc,
                                      const char *contact_id, size_t contact_id_len,
                                      size_t max_items, size_t max_bytes, double min_confidence,
                                      char **out, size_t *out_len) {
    if (!alloc || !out || !contact_id || contact_id_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    if (out_len)
        *out_len = 0;
    sqlite3 *db = db_of(mem);
    if (!db)
        return mem ? HU_ERR_NOT_SUPPORTED : HU_ERR_INVALID_ARGUMENT;
    if (max_items == 0 || max_bytes == 0)
        return HU_OK;
    /* Missing table = nothing extracted yet, not an error. */
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT insight, as_of_ms FROM contact_insights"
                      " WHERE contact_id = ?1 AND retired_at_ms = 0 AND confidence >= ?2"
                      " ORDER BY as_of_ms DESC, id DESC LIMIT ?3";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_OK;
    sqlite3_bind_text(st, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);
    sqlite3_bind_double(st, 2, min_confidence);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)max_items);

    char *buf = (char *)alloc->alloc(alloc->ctx, max_bytes + 1);
    if (!buf) {
        sqlite3_finalize(st);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t len = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *ins = (const char *)sqlite3_column_text(st, 0);
        int64_t as_of = (int64_t)sqlite3_column_int64(st, 1);
        if (!ins || !ins[0])
            continue;
        char month[16];
        size_t mlen = as_of > 0 ? hu_graph_state_format_month(as_of, month, sizeof(month)) : 0;
        char line[512];
        int n = mlen ? snprintf(line, sizeof(line), "- %s (as of %s)\n", ins, month)
                     : snprintf(line, sizeof(line), "- %s\n", ins);
        if (n <= 0 || (size_t)n >= sizeof(line))
            continue; /* an over-long note is dropped whole, never cut mid-line */
        if (len + (size_t)n > max_bytes)
            break;
        memcpy(buf + len, line, (size_t)n);
        len += (size_t)n;
    }
    sqlite3_finalize(st);
    buf[len] = '\0';
    if (len == 0) {
        alloc->free(alloc->ctx, buf, max_bytes + 1);
        return HU_OK;
    }
    /* Shrink to fit the documented (*out_len + 1) free contract. */
    char *fit = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!fit) {
        alloc->free(alloc->ctx, buf, max_bytes + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(fit, buf, len + 1);
    alloc->free(alloc->ctx, buf, max_bytes + 1);
    *out = fit;
    if (out_len)
        *out_len = len;
    return HU_OK;
}

#else /* !HU_ENABLE_SQLITE */

hu_error_t hu_contact_insights_ensure_schema(hu_memory_t *mem) {
    (void)mem;
    return HU_ERR_NOT_SUPPORTED;
}
hu_error_t hu_contact_insights_add(hu_memory_t *mem, const char *contact_id, size_t contact_id_len,
                                   const char *kind, const char *insight, double confidence,
                                   int64_t as_of_ms, const char *source, int64_t *out_id) {
    (void)mem;
    (void)contact_id;
    (void)contact_id_len;
    (void)kind;
    (void)insight;
    (void)confidence;
    (void)as_of_ms;
    (void)source;
    (void)out_id;
    return HU_ERR_NOT_SUPPORTED;
}
hu_error_t hu_contact_insights_retire(hu_memory_t *mem, int64_t id, int64_t retired_at_ms) {
    (void)mem;
    (void)id;
    (void)retired_at_ms;
    return HU_ERR_NOT_SUPPORTED;
}
hu_error_t hu_contact_insights_render(hu_memory_t *mem, hu_allocator_t *alloc,
                                      const char *contact_id, size_t contact_id_len,
                                      size_t max_items, size_t max_bytes, double min_confidence,
                                      char **out, size_t *out_len) {
    (void)mem;
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
    (void)max_items;
    (void)max_bytes;
    (void)min_confidence;
    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;
    return HU_ERR_NOT_SUPPORTED;
}

#endif /* HU_ENABLE_SQLITE */
