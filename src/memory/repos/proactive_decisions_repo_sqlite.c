/*
 * src/memory/repos/proactive_decisions_repo_sqlite.c
 *
 * SQLite-backed implementation of the proactive-decisions repository. See
 * include/human/memory/proactive_decisions_repo.h for the contract this
 * file implements. Contract C5, Part A.
 */
#include "human/memory/proactive_decisions_repo.h"
#include <stdlib.h>

#ifdef HU_ENABLE_SQLITE

#include "human/memory/repo_util.h"
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

    return hu_repo_exec_ddl(db, kSchema);
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

/* One-column, one-row int64 query bound to a single TEXT parameter — the shape
 * both circuit reads need. Extracted rather than repeated: the prepare/bind/
 * step/finalize block is exactly the kind of duplication the clone ratchet
 * exists to stop. `have_value` distinguishes "no row / SQL NULL" from a
 * legitimate 0. */
static bool proactive_scalar_i64(sqlite3 *db, const char *sql, const char *param, int64_t *out,
                                 bool *have_value) {
    if (have_value)
        *have_value = false;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, param, -1, SQLITE_TRANSIENT);
    bool ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ok = true;
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            *out = sqlite3_column_int64(stmt, 0);
            if (have_value)
                *have_value = true;
        }
    }
    sqlite3_finalize(stmt);
    return ok;
}

hu_error_t hu_proactive_decisions_repo_consecutive_send_failures(sqlite3 *db, const char *contact,
                                                                 int64_t *out_n) {
    if (!db || !contact || !out_n)
        return HU_ERR_INVALID_ARGUMENT;
    *out_n = 0;

    hu_error_t schema_err = hu_proactive_decisions_repo_ensure_schema(db);
    if (schema_err != HU_OK)
        return schema_err;

    /* Failures strictly after this contact's most recent confirmed delivery.
     * COALESCE(...,0) makes "never delivered" count every failure, which is
     * exactly the case the breaker exists for. */
    static const char *SQL =
        "SELECT COUNT(*) FROM proactive_decisions "
        "WHERE contact = ?1 AND trigger = 'proactive_send' AND reason = 'send_failed' "
        "  AND ts > COALESCE((SELECT MAX(ts) FROM proactive_decisions "
        "                     WHERE contact = ?1 AND trigger = 'proactive_send' AND sent = 1), 0);";

    if (!proactive_scalar_i64(db, SQL, contact, out_n, NULL))
        return HU_ERR_MEMORY_STORE;
    return HU_OK;
}

/* Tunable via env so an operator can widen/narrow without a rebuild; the
 * defaults are set from the 2026-09-22 measurement (one contact, 124 attempts,
 * 0 deliveries, ~10/day). Threshold 5 tolerates a transient channel blip;
 * a 24h cooldown means a wedged contact costs 1 wasted send/day, not ~10. */
#define HU_PROACTIVE_SEND_CIRCUIT_THRESHOLD  5
#define HU_PROACTIVE_SEND_CIRCUIT_COOLDOWN_S 86400

static int64_t proactive_circuit_env_i64(const char *name, int64_t dflt) {
    const char *v = getenv(name);
    if (!v || !*v)
        return dflt;
    char *end = NULL;
    long long parsed = strtoll(v, &end, 10);
    if (end == v || *end != '\0' || parsed < 0)
        return dflt;
    return (int64_t)parsed;
}

bool hu_proactive_send_circuit_is_open(sqlite3 *db, const char *contact, int64_t now) {
    if (!db || !contact)
        return false;

    const int64_t threshold = proactive_circuit_env_i64("HU_PROACTIVE_SEND_CIRCUIT_THRESHOLD",
                                                        HU_PROACTIVE_SEND_CIRCUIT_THRESHOLD);
    if (threshold <= 0)
        return false; /* 0 disables the breaker entirely. */

    int64_t failures = 0;
    if (hu_proactive_decisions_repo_consecutive_send_failures(db, contact, &failures) != HU_OK)
        return false; /* Fail CLOSED — never silence a contact on a read error. */
    if (failures < threshold)
        return false;

    /* Open — unless the cooldown has elapsed, which lets one probe through. */
    const int64_t cooldown = proactive_circuit_env_i64("HU_PROACTIVE_SEND_CIRCUIT_COOLDOWN_S",
                                                       HU_PROACTIVE_SEND_CIRCUIT_COOLDOWN_S);
    int64_t last_failure = 0;
    bool have = false;
    if (!proactive_scalar_i64(db,
                              "SELECT MAX(ts) FROM proactive_decisions "
                              "WHERE contact = ?1 AND trigger = 'proactive_send' "
                              "  AND reason = 'send_failed';",
                              contact, &last_failure, &have) ||
        !have)
        return false;

    return (now - last_failure) < cooldown;
}

#endif /* HU_ENABLE_SQLITE */
