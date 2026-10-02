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
                    strcmp(kind, HU_OUTBOUND_SEND_KIND_REPLY) == 0);
}

hu_error_t hu_outbound_sends_repo_ensure_schema(sqlite3 *db) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    static const char *kSchema = "CREATE TABLE IF NOT EXISTS outbound_sends ("
                                 "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                 "  sent_at_ms INTEGER NOT NULL,"
                                 "  channel TEXT NOT NULL,"
                                 "  contact TEXT NOT NULL,"
                                 "  kind TEXT NOT NULL CHECK (kind IN ('text','media','reply')),"
                                 "  text TEXT,"
                                 "  prior_max_rowid INTEGER NOT NULL DEFAULT -1"
                                 ");"
                                 "CREATE INDEX IF NOT EXISTS idx_outbound_sends_contact_ts "
                                 "  ON outbound_sends(contact, sent_at_ms);";
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

hu_error_t hu_outbound_sends_repo_find_delivery(sqlite3 *db, const char *channel,
                                                const char *contact, size_t contact_len,
                                                int64_t target_rowid, int64_t prev_own_rowid,
                                                int64_t target_sent_ms, int64_t slack_ms,
                                                int64_t *out_sent_at_ms) {
    if (out_sent_at_ms)
        *out_sent_at_ms = 0;
    if (!db || !channel || !contact || contact_len == 0 || !out_sent_at_ms || target_rowid <= 0 ||
        target_sent_ms <= 0 || slack_ms < 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (hu_outbound_sends_repo_ensure_schema(db) != HU_OK)
        return HU_ERR_MEMORY_STORE;
    /* The send whose chat.db boundary sits in [prev own message, target):
     * its first is_from_me row after the boundary IS the target. A send whose
     * boundary precedes an earlier message of ours cannot claim this row. */
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT sent_at_ms FROM outbound_sends WHERE channel = ?1 "
                           "AND contact = ?2 AND prior_max_rowid >= ?3 AND prior_max_rowid < ?4 "
                           "AND ABS(sent_at_ms - ?5) <= ?6 "
                           "ORDER BY ABS(sent_at_ms - ?5) LIMIT 1;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_text(stmt, 1, channel, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, contact, (int)contact_len, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, prev_own_rowid > 0 ? prev_own_rowid : 0);
    sqlite3_bind_int64(stmt, 4, target_rowid);
    sqlite3_bind_int64(stmt, 5, target_sent_ms);
    sqlite3_bind_int64(stmt, 6, slack_ms);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
        *out_sent_at_ms = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_ROW)
        return HU_OK;
    return rc == SQLITE_DONE ? HU_ERR_NOT_FOUND : HU_ERR_MEMORY_STORE;
}

#endif /* HU_ENABLE_SQLITE */
