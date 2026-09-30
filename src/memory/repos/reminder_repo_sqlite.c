/*
 * src/memory/repos/reminder_repo_sqlite.c
 *
 * SQLite-backed owner reminders. Contract: include/human/memory/reminder_repo.h.
 */
#include "human/memory/reminder_repo.h"

#ifdef HU_ENABLE_SQLITE

#include "human/memory/repo_util.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* A row left in `sending` longer than this was orphaned by a crash mid-send. */
#define REMINDER_SENDING_STALE_S 600

hu_error_t hu_reminder_repo_ensure_schema(sqlite3 *db) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    static const char *schema_sql =
        "CREATE TABLE IF NOT EXISTS reminders ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  owner TEXT NOT NULL,"
        "  channel TEXT NOT NULL,"
        "  what TEXT NOT NULL,"
        "  due_at INTEGER NOT NULL,"
        "  status TEXT NOT NULL DEFAULT 'pending'"
        "    CHECK (status IN ('pending','sending','sent','missed','missed_told','done')),"
        "  created_at INTEGER NOT NULL,"
        "  updated_at INTEGER NOT NULL,"
        "  source TEXT"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_reminders_status_due ON reminders(status, due_at);"
        "CREATE INDEX IF NOT EXISTS idx_reminders_owner ON reminders(owner, status);";
    return hu_repo_exec_ddl(db, schema_sql);
}

static void copy_text(char *dst, size_t cap, const unsigned char *src) {
    snprintf(dst, cap, "%s", src ? (const char *)src : "");
}

/* Columns: id, owner, channel, what, due_at, updated_at. */
static void read_row(sqlite3_stmt *stmt, hu_reminder_t *r) {
    memset(r, 0, sizeof(*r));
    r->id = sqlite3_column_int64(stmt, 0);
    copy_text(r->owner, sizeof(r->owner), sqlite3_column_text(stmt, 1));
    copy_text(r->channel, sizeof(r->channel), sqlite3_column_text(stmt, 2));
    copy_text(r->what, sizeof(r->what), sqlite3_column_text(stmt, 3));
    r->due_at = sqlite3_column_int64(stmt, 4);
    r->updated_at = sqlite3_column_int64(stmt, 5);
}

hu_error_t hu_reminder_repo_add(sqlite3 *db, const char *owner, const char *channel,
                                const char *what, int64_t due_at, int64_t now, const char *source,
                                int64_t *out_id) {
    if (!db || !owner || !owner[0] || !channel || !channel[0] || !what || !what[0] || due_at <= 0)
        return HU_ERR_INVALID_ARGUMENT;
    hu_error_t err = hu_reminder_repo_ensure_schema(db);
    if (err != HU_OK)
        return err;
    static const char *insert_sql =
        "INSERT INTO reminders (owner, channel, what, due_at, status, created_at, updated_at, "
        "source) VALUES (?1, ?2, ?3, ?4, 'pending', ?5, ?5, ?6);";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, insert_sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_text(stmt, 1, owner, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, channel, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, what, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4, due_at);
    sqlite3_bind_int64(stmt, 5, now);
    if (source)
        sqlite3_bind_text(stmt, 6, source, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 6);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_STORE;
    if (out_id)
        *out_id = sqlite3_last_insert_rowid(db);
    return HU_OK;
}

static hu_error_t exec_update(sqlite3 *db, const char *sql, int64_t a, int64_t b,
                              int *out_changes) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_int64(stmt, 1, a);
    sqlite3_bind_int64(stmt, 2, b);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_STORE;
    if (out_changes)
        *out_changes = sqlite3_changes(db);
    return HU_OK;
}

hu_error_t hu_reminder_repo_claim_due(sqlite3 *db, int64_t now, int64_t late_after_s,
                                      hu_reminder_t *out, size_t cap, size_t *out_n,
                                      size_t *out_missed) {
    if (!db || !out || cap == 0 || !out_n || late_after_s <= 0)
        return HU_ERR_INVALID_ARGUMENT;
    *out_n = 0;
    if (out_missed)
        *out_missed = 0;
    hu_error_t err = hu_reminder_repo_ensure_schema(db);
    if (err != HU_OK)
        return err;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;

    int changes = 0;
    /* Orphaned by a crash mid-send: retry rather than lose it. */
    err = exec_update(db,
                      "UPDATE reminders SET status='pending', updated_at=?1 "
                      "WHERE status='sending' AND updated_at < ?2;",
                      now, now - REMINDER_SENDING_STALE_S, NULL);
    /* Too late to be useful as a nudge: record it, do not send it. */
    if (err == HU_OK)
        err = exec_update(db,
                          "UPDATE reminders SET status='missed', updated_at=?1 "
                          "WHERE status='pending' AND due_at < ?2;",
                          now, now - late_after_s, &changes);
    if (err == HU_OK && out_missed)
        *out_missed = (size_t)changes;

    sqlite3_stmt *sel = NULL;
    if (err == HU_OK &&
        sqlite3_prepare_v2(db,
                           "SELECT id, owner, channel, what, due_at, updated_at FROM reminders "
                           "WHERE status='pending' AND due_at <= ?1 ORDER BY due_at, id LIMIT ?2;",
                           -1, &sel, NULL) != SQLITE_OK)
        err = HU_ERR_MEMORY_STORE;
    if (err == HU_OK) {
        sqlite3_bind_int64(sel, 1, now);
        sqlite3_bind_int64(sel, 2, (int64_t)cap);
        int rc;
        while ((rc = sqlite3_step(sel)) == SQLITE_ROW && *out_n < cap)
            read_row(sel, &out[(*out_n)++]);
        if (rc != SQLITE_ROW && rc != SQLITE_DONE)
            err = HU_ERR_MEMORY_STORE;
        sqlite3_finalize(sel);
    }
    for (size_t i = 0; err == HU_OK && i < *out_n; i++) {
        err = exec_update(db, "UPDATE reminders SET status='sending', updated_at=?1 WHERE id=?2;",
                          now, out[i].id, NULL);
        out[i].updated_at = now;
    }

    if (err == HU_OK && sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) == SQLITE_OK)
        return HU_OK;
    (void)sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    *out_n = 0;
    if (out_missed)
        *out_missed = 0;
    return err == HU_OK ? HU_ERR_MEMORY_STORE : err;
}

/* One-row update: ?1 is `text1` when non-NULL, else `int1`; ?2 is now; ?3 id.
 * HU_ERR_NOT_FOUND when no row has that id. */
static hu_error_t update_one(sqlite3 *db, const char *sql, const char *text1, int64_t int1,
                             int64_t now, int64_t id) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    if (text1)
        sqlite3_bind_text(stmt, 1, text1, -1, SQLITE_STATIC);
    else
        sqlite3_bind_int64(stmt, 1, int1);
    sqlite3_bind_int64(stmt, 2, now);
    sqlite3_bind_int64(stmt, 3, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_STORE;
    return sqlite3_changes(db) == 1 ? HU_OK : HU_ERR_NOT_FOUND;
}

hu_error_t hu_reminder_repo_mark(sqlite3 *db, int64_t id, const char *status, int64_t now) {
    if (!db || id <= 0 || !status)
        return HU_ERR_INVALID_ARGUMENT;
    if (strcmp(status, "sent") != 0 && strcmp(status, "pending") != 0 &&
        strcmp(status, "done") != 0 && strcmp(status, "missed_told") != 0)
        return HU_ERR_INVALID_ARGUMENT;
    return update_one(db, "UPDATE reminders SET status=?1, updated_at=?2 WHERE id=?3;", status, 0,
                      now, id);
}

hu_error_t hu_reminder_repo_snooze(sqlite3 *db, int64_t id, int64_t new_due, int64_t now) {
    if (!db || id <= 0 || new_due <= 0)
        return HU_ERR_INVALID_ARGUMENT;
    return update_one(
        db, "UPDATE reminders SET status='pending', due_at=?1, updated_at=?2 WHERE id=?3;", NULL,
        new_due, now, id);
}

hu_error_t hu_reminder_repo_last_sent(sqlite3 *db, const char *owner, int64_t since,
                                      hu_reminder_t *out) {
    if (!db || !owner || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_error_t err = hu_reminder_repo_ensure_schema(db);
    if (err != HU_OK)
        return err;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT id, owner, channel, what, due_at, updated_at FROM reminders "
                           "WHERE owner=?1 AND status='sent' AND updated_at >= ?2 "
                           "ORDER BY updated_at DESC, id DESC LIMIT 1;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_text(stmt, 1, owner, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, since);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
        read_row(stmt, out);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_ROW)
        return HU_OK;
    return rc == SQLITE_DONE ? HU_ERR_NOT_FOUND : HU_ERR_MEMORY_STORE;
}

/* Rows matching `where` (which binds ?1 = owner when owner is non-NULL),
 * oldest due first, at most `cap`. */
static hu_error_t list_rows(sqlite3 *db, const char *where, const char *owner, hu_reminder_t *out,
                            size_t cap, size_t *out_n) {
    if (!db || !out || cap == 0 || !out_n)
        return HU_ERR_INVALID_ARGUMENT;
    *out_n = 0;
    hu_error_t err = hu_reminder_repo_ensure_schema(db);
    if (err != HU_OK)
        return err;
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT id, owner, channel, what, due_at, updated_at FROM reminders WHERE %s "
             "ORDER BY due_at, id LIMIT %zu;",
             where, cap);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    if (owner)
        sqlite3_bind_text(stmt, 1, owner, -1, SQLITE_STATIC);
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && *out_n < cap)
        read_row(stmt, &out[(*out_n)++]);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_ROW || rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_STORE;
}

hu_error_t hu_reminder_repo_upcoming(sqlite3 *db, const char *owner, hu_reminder_t *out, size_t cap,
                                     size_t *out_n) {
    if (!owner)
        return HU_ERR_INVALID_ARGUMENT;
    return list_rows(db, "owner=?1 AND status='pending'", owner, out, cap, out_n);
}

hu_error_t hu_reminder_repo_missed(sqlite3 *db, hu_reminder_t *out, size_t cap, size_t *out_n) {
    return list_rows(db, "status='missed'", NULL, out, cap, out_n);
}

#endif /* HU_ENABLE_SQLITE */
