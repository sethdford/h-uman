/*
 * src/memory/repos/proactive_decisions_repo_sqlite.c
 *
 * SQLite-backed implementation of the proactive-decisions repository. See
 * include/human/memory/proactive_decisions_repo.h for the contract this
 * file implements. Contract C5, Part A.
 */
#include "human/memory/proactive_decisions_repo.h"

#ifdef HU_ENABLE_SQLITE

#include <stdbool.h>
#include <string.h>

static bool proactive_decision_is_valid(const char *decision) {
    return decision && (strcmp(decision, HU_PROACTIVE_DECISION_SEND) == 0 ||
                        strcmp(decision, HU_PROACTIVE_DECISION_DECLINE) == 0 ||
                        strcmp(decision, HU_PROACTIVE_DECISION_DEFER) == 0);
}

hu_error_t hu_proactive_decisions_repo_ensure_schema(sqlite3 *db) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;

    static const char *kSchema =
        "CREATE TABLE IF NOT EXISTS proactive_decisions ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ts INTEGER NOT NULL,"
        "  contact TEXT,"
        "  trigger TEXT NOT NULL,"
        "  decision TEXT NOT NULL CHECK (decision IN ('send','decline','defer')),"
        "  reason TEXT,"
        "  sent INTEGER NOT NULL DEFAULT 0,"
        "  message_ref TEXT"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_proactive_decisions_ts ON proactive_decisions(ts);"
        "CREATE INDEX IF NOT EXISTS idx_proactive_decisions_contact "
        "  ON proactive_decisions(contact);";

    char *errmsg = NULL;
    int rc = sqlite3_exec(db, kSchema, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        if (errmsg)
            sqlite3_free(errmsg);
        return HU_ERR_MEMORY_STORE;
    }
    return HU_OK;
}

hu_error_t hu_proactive_decisions_repo_record(sqlite3 *db, int64_t ts, const char *contact,
                                              const char *trigger, const char *decision,
                                              const char *reason, int sent,
                                              const char *message_ref) {
    if (!db || !trigger || !trigger[0])
        return HU_ERR_INVALID_ARGUMENT;
    /* Per ~/.claude/rules/reports-success-does-nothing.md: a decision value
     * outside the known set must not silently land in the table as an
     * uncategorized row — that's exactly the kind of measurement corruption
     * this table exists to avoid. Reject loudly instead. */
    if (!proactive_decision_is_valid(decision))
        return HU_ERR_INVALID_ARGUMENT;

    hu_error_t schema_err = hu_proactive_decisions_repo_ensure_schema(db);
    if (schema_err != HU_OK)
        return schema_err;

    static const char *kInsert =
        "INSERT INTO proactive_decisions (ts, contact, trigger, decision, reason, sent, "
        "message_ref) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, kInsert, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;

    sqlite3_bind_int64(stmt, 1, ts);
    if (contact)
        sqlite3_bind_text(stmt, 2, contact, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 2);
    sqlite3_bind_text(stmt, 3, trigger, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, decision, -1, SQLITE_STATIC);
    if (reason)
        sqlite3_bind_text(stmt, 5, reason, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 5);
    sqlite3_bind_int(stmt, 6, sent ? 1 : 0);
    if (message_ref)
        sqlite3_bind_text(stmt, 7, message_ref, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 7);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_STORE;
    return HU_OK;
}

hu_error_t hu_proactive_decisions_repo_count(sqlite3 *db, int64_t *out_count) {
    if (!db || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;

    hu_error_t schema_err = hu_proactive_decisions_repo_ensure_schema(db);
    if (schema_err != HU_OK)
        return schema_err;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM proactive_decisions;", -1, &stmt, NULL) !=
        SQLITE_OK)
        return HU_ERR_MEMORY_STORE;

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
        *out_count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_ROW)
        return HU_ERR_MEMORY_STORE;
    return HU_OK;
}

#endif /* HU_ENABLE_SQLITE */
