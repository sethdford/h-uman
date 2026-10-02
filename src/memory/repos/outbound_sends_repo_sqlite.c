/*
 * src/memory/repos/outbound_sends_repo_sqlite.c
 *
 * SQLite-backed outbound-sends repository. Contract:
 * include/human/memory/outbound_sends_repo.h.
 */
#include "human/memory/outbound_sends_repo.h"

#ifdef HU_ENABLE_SQLITE

#include "human/memory/repo_util.h"
#include <stdbool.h>
#include <string.h>

static bool outbound_kind_is_valid(const char *kind) {
    return kind && (strcmp(kind, HU_OUTBOUND_SEND_KIND_TEXT) == 0 ||
                    strcmp(kind, HU_OUTBOUND_SEND_KIND_MEDIA) == 0 ||
                    strcmp(kind, HU_OUTBOUND_SEND_KIND_REPLY) == 0 ||
                    strcmp(kind, HU_OUTBOUND_SEND_KIND_TAPBACK) == 0);
}

#define OUTBOUND_SENDS_COLUMNS                                                 \
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"                                  \
    "  sent_at_ms INTEGER NOT NULL,"                                           \
    "  channel TEXT NOT NULL,"                                                 \
    "  contact TEXT NOT NULL,"                                                 \
    "  kind TEXT NOT NULL CHECK (kind IN ('text','media','reply','tapback'))," \
    "  text TEXT,"                                                             \
    "  prior_max_rowid INTEGER NOT NULL DEFAULT -1"

#define OUTBOUND_SENDS_INDEX                                    \
    "CREATE INDEX IF NOT EXISTS idx_outbound_sends_contact_ts " \
    "  ON outbound_sends(contact, sent_at_ms);"

/* True when an outbound_sends table exists whose CHECK predates 'tapback'.
 * SQLite cannot ALTER a CHECK constraint, and CREATE TABLE IF NOT EXISTS
 * leaves the old table as it is, so memory.db files created before tapbacks
 * were recorded need the table rebuilt. */
static bool outbound_sends_needs_rebuild(sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT sql FROM sqlite_master WHERE type='table' AND "
                           "name='outbound_sends';",
                           -1, &st, NULL) != SQLITE_OK)
        return false;
    bool stale = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *sql = (const char *)sqlite3_column_text(st, 0);
        stale = sql && !strstr(sql, "'tapback'");
    }
    sqlite3_finalize(st);
    return stale;
}

/* Rebuild under the new CHECK, keeping every row and its id (SQLite's
 * documented copy/drop/rename procedure). A SAVEPOINT rather than BEGIN, so
 * it nests inside a caller's transaction; on any failure the old table is
 * restored untouched. */
static hu_error_t outbound_sends_rebuild(sqlite3 *db) {
    static const char *kRebuild =
        "SAVEPOINT outbound_sends_tapback;"
        "CREATE TABLE outbound_sends_rebuild (" OUTBOUND_SENDS_COLUMNS ");"
        "INSERT INTO outbound_sends_rebuild (id, sent_at_ms, channel, contact, kind, text, "
        "  prior_max_rowid) "
        "  SELECT id, sent_at_ms, channel, contact, kind, text, prior_max_rowid "
        "  FROM outbound_sends;"
        "DROP TABLE outbound_sends;"
        "ALTER TABLE outbound_sends_rebuild RENAME TO outbound_sends;" OUTBOUND_SENDS_INDEX
        "RELEASE outbound_sends_tapback;";
    if (hu_repo_exec_ddl(db, kRebuild) == HU_OK)
        return HU_OK;
    (void)sqlite3_exec(db, "ROLLBACK TO outbound_sends_tapback; RELEASE outbound_sends_tapback;",
                       NULL, NULL, NULL);
    return HU_ERR_MEMORY_STORE;
}

hu_error_t hu_outbound_sends_repo_ensure_schema(sqlite3 *db) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    if (outbound_sends_needs_rebuild(db)) {
        hu_error_t err = outbound_sends_rebuild(db);
        if (err != HU_OK)
            return err;
    }
    static const char *kSchema =
        "CREATE TABLE IF NOT EXISTS outbound_sends (" OUTBOUND_SENDS_COLUMNS
        ");" OUTBOUND_SENDS_INDEX;
    return hu_repo_exec_ddl(db, kSchema);
}

hu_error_t hu_outbound_sends_repo_record(sqlite3 *db, int64_t sent_at_ms, const char *channel,
                                         const char *contact, size_t contact_len, const char *kind,
                                         const char *text, size_t text_len,
                                         int64_t prior_max_rowid) {
    if (!db || !channel || !channel[0] || !contact || contact_len == 0 ||
        !outbound_kind_is_valid(kind))
        return HU_ERR_INVALID_ARGUMENT;
    if (text_len > 0 && !text)
        return HU_ERR_INVALID_ARGUMENT;

    hu_error_t schema_err = hu_outbound_sends_repo_ensure_schema(db);
    if (schema_err != HU_OK)
        return schema_err;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO outbound_sends (sent_at_ms, channel, contact, kind, text, "
                           "prior_max_rowid) VALUES (?1, ?2, ?3, ?4, ?5, ?6);",
                           -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_int64(stmt, 1, sent_at_ms);
    sqlite3_bind_text(stmt, 2, channel, -1, SQLITE_STATIC);
    /* (ptr, len): the channel's handle is not guaranteed NUL-terminated. */
    sqlite3_bind_text(stmt, 3, contact, (int)contact_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, kind, -1, SQLITE_STATIC);
    if (text && text_len > 0)
        sqlite3_bind_text(stmt, 5, text, (int)text_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 5);
    sqlite3_bind_int64(stmt, 6, prior_max_rowid);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_MEMORY_STORE;
}

hu_error_t hu_outbound_sends_repo_count(sqlite3 *db, int64_t *out_count) {
    if (!db || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    hu_error_t schema_err = hu_outbound_sends_repo_ensure_schema(db);
    if (schema_err != HU_OK)
        return schema_err;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM outbound_sends;", -1, &stmt, NULL) !=
        SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    bool ok = sqlite3_step(stmt) == SQLITE_ROW;
    if (ok)
        *out_count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return ok ? HU_OK : HU_ERR_MEMORY_STORE;
}

#endif /* HU_ENABLE_SQLITE */
