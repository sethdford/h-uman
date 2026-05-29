/* src/memory/repos/theory_of_mind_repo_sqlite.c
 * Theory-of-Mind persistence: the place the tom_user_expectations /
 * tom_user_beliefs / tom_self_change SQL + GC + daemon-tick wrappers + the raw
 * sqlite3 handle live, per the memory repository pattern (see
 * boundary_repo_sqlite.c). The domain module src/agent/theory_of_mind.c keeps
 * the pure in-memory belief-state logic (record/build_context/detect_gaps/
 * detect_user_expectation/...) and no longer includes <sqlite3.h>. Callers
 * pass an injected sqlite3* and are unchanged; signatures preserved.
 * Registered unconditionally with an empty-TU guard for the !SQLITE variant. */
#include "human/agent/theory_of_mind.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef HU_ENABLE_SQLITE

#include <sqlite3.h>

hu_error_t hu_tom_user_expectations_init_table(sqlite3 *db) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    const char *sql_table = "CREATE TABLE IF NOT EXISTS tom_user_expectations("
                            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                            "contact_id TEXT NOT NULL, "
                            "topic TEXT NOT NULL, "
                            "expected_knowledge_type INTEGER NOT NULL, "
                            "session_key TEXT, "
                            "turn_number INTEGER, "
                            "created_ts_ms INTEGER NOT NULL, "
                            "resolved_ts_ms INTEGER, "
                            "UNIQUE(contact_id, topic, session_key) ON CONFLICT IGNORE);";
    char *err_msg = NULL;
    int rc = sqlite3_exec(db, sql_table, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg)
            sqlite3_free(err_msg);
        return HU_ERR_IO;
    }
    const char *sql_idx = "CREATE INDEX IF NOT EXISTS idx_tom_user_expectations_contact "
                          "ON tom_user_expectations(contact_id, resolved_ts_ms);";
    rc = sqlite3_exec(db, sql_idx, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg)
            sqlite3_free(err_msg);
        return HU_ERR_IO;
    }
    return HU_OK;
}

hu_error_t hu_tom_persist_user_expectation(sqlite3 *db, const char *contact_id, const char *topic,
                                           size_t topic_len,
                                           hu_tom_expected_knowledge_t knowledge_type,
                                           const char *session_key, size_t session_key_len,
                                           int64_t turn_number, int64_t now_ts_ms) {
    if (!db || !contact_id || !topic)
        return HU_ERR_INVALID_ARGUMENT;
    size_t tlen = topic_len ? topic_len : strlen(topic);
    if (tlen == 0)
        return HU_ERR_INVALID_ARGUMENT;
    size_t slen = session_key_len;
    if (session_key && slen == 0)
        slen = strlen(session_key);

    sqlite3_stmt *stmt = NULL;
    int rc =
        sqlite3_prepare_v2(db,
                           "INSERT OR IGNORE INTO tom_user_expectations("
                           "contact_id, topic, expected_knowledge_type, session_key, turn_number, "
                           "created_ts_ms, resolved_ts_ms) "
                           "VALUES(?, ?, ?, ?, ?, ?, NULL)",
                           -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(stmt, 1, contact_id, (int)strlen(contact_id), SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, topic, (int)tlen, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, (int)knowledge_type);
    if (session_key && slen > 0)
        sqlite3_bind_text(stmt, 4, session_key, (int)slen, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 4);
    sqlite3_bind_int64(stmt, 5, turn_number);
    sqlite3_bind_int64(stmt, 6, now_ts_ms);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_IO;
}

hu_error_t hu_tom_user_expectations_count_for_contact(sqlite3 *db, const char *contact_id,
                                                      int64_t *out_count) {
    if (!db || !contact_id || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db, "SELECT COUNT(*) FROM tom_user_expectations WHERE contact_id = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(stmt, 1, contact_id, (int)strlen(contact_id), SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW)
        *out_count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return HU_OK;
}

hu_error_t hu_tom_user_expectations_load_unresolved(sqlite3 *db, hu_allocator_t *alloc,
                                                    const char *contact_id, size_t max_rows,
                                                    hu_tom_persisted_expectation_t **out,
                                                    size_t *out_count) {
    if (!db || !alloc || !contact_id || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    if (max_rows == 0)
        max_rows = 32;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT topic, expected_knowledge_type FROM tom_user_expectations "
                                "WHERE contact_id = ? AND resolved_ts_ms IS NULL "
                                "ORDER BY created_ts_ms DESC LIMIT ?",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(stmt, 1, contact_id, (int)strlen(contact_id), SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (int64_t)max_rows);

    hu_tom_persisted_expectation_t *rows = (hu_tom_persisted_expectation_t *)alloc->alloc(
        alloc->ctx, max_rows * sizeof(hu_tom_persisted_expectation_t));
    if (!rows) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(rows, 0, max_rows * sizeof(hu_tom_persisted_expectation_t));

    size_t count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_rows) {
        const unsigned char *topic = sqlite3_column_text(stmt, 0);
        int topic_bytes = sqlite3_column_bytes(stmt, 0);
        int ktype = sqlite3_column_int(stmt, 1);
        if (!topic || topic_bytes <= 0)
            continue;
        char *dup = hu_strndup(alloc, (const char *)topic, (size_t)topic_bytes);
        if (!dup) {
            hu_tom_persisted_expectations_free(alloc, rows, count);
            sqlite3_finalize(stmt);
            return HU_ERR_OUT_OF_MEMORY;
        }
        rows[count].topic = dup;
        rows[count].topic_len = (size_t)topic_bytes;
        rows[count].knowledge_type = (hu_tom_expected_knowledge_t)ktype;
        count++;
    }
    sqlite3_finalize(stmt);

    if (count == 0) {
        alloc->free(alloc->ctx, rows, max_rows * sizeof(hu_tom_persisted_expectation_t));
        return HU_OK;
    }
    *out = rows;
    *out_count = count;
    return HU_OK;
}

void hu_tom_persisted_expectations_free(hu_allocator_t *alloc, hu_tom_persisted_expectation_t *rows,
                                        size_t count) {
    if (!alloc || !rows)
        return;
    for (size_t i = 0; i < count; i++) {
        if (rows[i].topic)
            hu_str_free(alloc, rows[i].topic);
        rows[i].topic = NULL;
        rows[i].topic_len = 0;
    }
    alloc->free(alloc->ctx, rows, count * sizeof(hu_tom_persisted_expectation_t));
}

static const char *knowledge_label(hu_tom_expected_knowledge_t k) {
    switch (k) {
    case HU_TOM_EXPECT_REMEMBERS:
        return "remembers";
    case HU_TOM_EXPECT_UNDERSTANDS:
        return "understands";
    case HU_TOM_EXPECT_TRACKS:
        return "tracks";
    default:
        return "knows";
    }
}

hu_error_t hu_tom_build_context_with_expectations(const hu_tom_belief_state_t *state,
                                                  const hu_tom_persisted_expectation_t *exps,
                                                  size_t exp_count, hu_allocator_t *alloc,
                                                  char **out, size_t *out_len) {
    if (!state || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    char *base = NULL;
    size_t base_len = 0;
    hu_error_t err = hu_tom_build_context(state, alloc, &base, &base_len);
    if (err != HU_OK)
        return err;

    /* D-TOM-3: when exp_count == 0 the new section is OMITTED entirely. */
    if (!exps || exp_count == 0) {
        *out = base;
        *out_len = base_len;
        return HU_OK;
    }

    char buf[4096];
    size_t pos = 0;
    pos = hu_buf_appendf(buf, sizeof(buf), pos, "### Unmet User Expectations\n");
    pos = hu_buf_appendf(buf, sizeof(buf), pos,
                         "The user has signalled (via phrases like \"as you know\", \"remember "
                         "when\", \"we discussed\") that you should already know these topics, "
                         "but you have no recorded belief about them:\n");
    for (size_t i = 0; i < exp_count && pos < sizeof(buf) - 64; i++) {
        if (!exps[i].topic || exps[i].topic_len == 0)
            continue;
        pos = hu_buf_appendf(buf, sizeof(buf), pos, "- \"%.*s\" (expects AI %s)\n",
                             (int)exps[i].topic_len, exps[i].topic,
                             knowledge_label(exps[i].knowledge_type));
    }
    pos = hu_buf_appendf(buf, sizeof(buf), pos,
                         "Acknowledge the gap honestly; do not confabulate prior knowledge.\n");
    if (pos >= sizeof(buf))
        pos = sizeof(buf) - 1;

    size_t need_sep = (base_len > 0 && base[base_len - 1] != '\n') ? 1 : 0;
    size_t total = base_len + need_sep + pos;
    char *merged = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!merged) {
        hu_str_free(alloc, base);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(merged, base, base_len);
    if (need_sep)
        merged[base_len] = '\n';
    memcpy(merged + base_len + need_sep, buf, pos);
    merged[total] = '\0';
    hu_str_free(alloc, base);
    *out = merged;
    *out_len = total;
    return HU_OK;
}

hu_error_t hu_tom_user_expectations_gc(sqlite3 *db, int64_t now_ts_ms, int64_t ttl_ms,
                                       int64_t *out_deleted) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    if (out_deleted)
        *out_deleted = 0;
    if (ttl_ms <= 0)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "DELETE FROM tom_user_expectations "
                                "WHERE resolved_ts_ms IS NOT NULL AND resolved_ts_ms < ?",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_int64(stmt, 1, now_ts_ms - ttl_ms);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_IO;
    if (out_deleted)
        *out_deleted = sqlite3_changes(db);
    return HU_OK;
}

static atomic_bool g_warned_tom_gc_enabled = false;

#if HU_IS_TEST
void hu_daemon_tick_tom_expectation_gc_reset_warn_guards_for_test(void) {
    atomic_store(&g_warned_tom_gc_enabled, false);
}
#endif

hu_error_t hu_daemon_tick_tom_expectation_gc(sqlite3 *db, int64_t now_ts_ms,
                                             int64_t *last_run_ts_ms_inout,
                                             int64_t interval_seconds, int64_t ttl_ms) {
    if (!db || !last_run_ts_ms_inout)
        return HU_ERR_INVALID_ARGUMENT;
    if (interval_seconds <= 0)
        interval_seconds = 86400;
    if (ttl_ms <= 0)
        ttl_ms = (int64_t)30 * 24 * 3600 * 1000;

    int64_t interval_ms = interval_seconds * 1000;
    if (*last_run_ts_ms_inout > 0 && now_ts_ms - *last_run_ts_ms_inout < interval_ms)
        return HU_OK;

    hu_log_info_once(&g_warned_tom_gc_enabled, "daemon", NULL,
                     "tom_user_expectations GC tick enabled — sweeping resolved rows older than "
                     "ttl_ms=%lld every %lld seconds",
                     (long long)ttl_ms, (long long)interval_seconds);

    int64_t deleted = 0;
    hu_error_t err = hu_tom_user_expectations_gc(db, now_ts_ms, ttl_ms, &deleted);
    *last_run_ts_ms_inout = now_ts_ms;
    (void)deleted;
    return err;
}

/* =====================================================================
 * Phase B of Spec 4: SQLite-backed belief temporality.
 *
 * Spec: specs/2026-05-19-tom-activation/{requirements,design,tasks}.md
 *  - Task 4: tom_user_beliefs table with nullable session_key +
 *    turn_number columns.
 *
 * Design: D-TOM-4 — beliefs gain optional conversation-local temporality
 * via session_key + turn_number. Existing in-memory belief recording
 * (hu_tom_record_belief above) is unchanged; this is the persisted
 * layer the daemon writes through on every post-turn capture.
 * ===================================================================== */

hu_error_t hu_tom_user_beliefs_init_table(sqlite3 *db) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    /* Single CREATE statement includes all temporal columns up-front.
     * D-TOM-4 framed Task 4 as an ALTER TABLE migration on an existing
     * table; in practice no prior tom_user_beliefs table existed in
     * the codebase, so the "migration" collapses to first-time table
     * creation with the columns already present. Rows with NULL
     * session_key / turn_number represent the AC-TOM-4 backward-compat
     * "global / pre-temporality" semantic. */
    const char *sql_table = "CREATE TABLE IF NOT EXISTS tom_user_beliefs("
                            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                            "contact_id TEXT NOT NULL, "
                            "topic TEXT NOT NULL, "
                            "belief_type INTEGER NOT NULL, "
                            "confidence REAL NOT NULL, "
                            "session_key TEXT, "
                            "turn_number INTEGER, "
                            "last_updated_ts_ms INTEGER NOT NULL);";
    char *err_msg = NULL;
    int rc = sqlite3_exec(db, sql_table, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg)
            sqlite3_free(err_msg);
        return HU_ERR_IO;
    }
    /* Idempotency on (contact_id, topic, session_key). SQLite treats
     * NULL values in UNIQUE indexes as distinct (so multiple NULL
     * session_key rows for the same topic are allowed — matching the
     * "global belief" backward-compat semantic). The same-session
     * collision path falls through to an UPDATE in hu_tom_persist_belief. */
    const char *sql_unique = "CREATE UNIQUE INDEX IF NOT EXISTS "
                             "idx_tom_user_beliefs_contact_topic_session "
                             "ON tom_user_beliefs(contact_id, topic, session_key);";
    rc = sqlite3_exec(db, sql_unique, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg)
            sqlite3_free(err_msg);
        return HU_ERR_IO;
    }
    /* Range-scan index for staleness gap detection (Phase D). */
    const char *sql_ts = "CREATE INDEX IF NOT EXISTS idx_tom_user_beliefs_contact_ts "
                         "ON tom_user_beliefs(contact_id, last_updated_ts_ms);";
    rc = sqlite3_exec(db, sql_ts, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg)
            sqlite3_free(err_msg);
        return HU_ERR_IO;
    }
    return HU_OK;
}

/* Spec 4 Q-TOM-B / Phase B follow-up: when a belief is recorded, mark
 * any matching unresolved expectations on the same (contact_id, topic) as
 * resolved. Closes the expectation→belief→resolution loop so the GC tick
 * has rows to delete and the clarify directive stops firing on topics
 * the agent now actually knows about.
 *
 * Best-effort: failures don't fail the caller's belief write. Logged
 * once per process on the first invocation to confirm the loop is alive. */
static void resolve_expectations_for_belief(sqlite3 *db, const char *contact_id, const char *topic,
                                            size_t topic_len, int64_t now_ts_ms) {
    if (!db || !contact_id || !topic || topic_len == 0)
        return;
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "UPDATE tom_user_expectations "
                                "SET resolved_ts_ms = ? "
                                "WHERE contact_id = ? AND topic = ? "
                                "  AND resolved_ts_ms IS NULL",
                                -1, &st, NULL);
    if (rc != SQLITE_OK) {
        if (st)
            sqlite3_finalize(st);
        return;
    }
    sqlite3_bind_int64(st, 1, now_ts_ms);
    sqlite3_bind_text(st, 2, contact_id, (int)strlen(contact_id), SQLITE_STATIC);
    sqlite3_bind_text(st, 3, topic, (int)topic_len, SQLITE_STATIC);
    (void)sqlite3_step(st);
    sqlite3_finalize(st);
}

hu_error_t hu_tom_persist_belief(sqlite3 *db, const char *contact_id, const char *topic,
                                 size_t topic_len, hu_belief_type_t belief_type, float confidence,
                                 const char *session_key, size_t session_key_len,
                                 int64_t turn_number, int64_t now_ts_ms) {
    if (!db || !contact_id || !topic)
        return HU_ERR_INVALID_ARGUMENT;
    size_t tlen = topic_len ? topic_len : strlen(topic);
    if (tlen == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (belief_type > HU_BELIEF_MISTAKEN)
        return HU_ERR_INVALID_ARGUMENT;
    float clamped = confidence;
    if (clamped < 0.0f)
        clamped = 0.0f;
    if (clamped > 1.0f)
        clamped = 1.0f;
    size_t slen = session_key_len;
    if (session_key && slen == 0)
        slen = strlen(session_key);

    /* Two-step upsert: SQLite's UNIQUE INDEX on (contact_id, topic,
     * session_key) treats NULL session_keys as distinct, so naive
     * INSERT OR REPLACE would clobber a different existing row. We
     * explicitly UPDATE first; if zero rows changed, INSERT. */
    sqlite3_stmt *upd = NULL;
    const char *sql_upd_with_session =
        "UPDATE tom_user_beliefs "
        "SET belief_type = ?, confidence = ?, turn_number = ?, last_updated_ts_ms = ? "
        "WHERE contact_id = ? AND topic = ? AND session_key = ?";
    const char *sql_upd_null_session =
        "UPDATE tom_user_beliefs "
        "SET belief_type = ?, confidence = ?, turn_number = ?, last_updated_ts_ms = ? "
        "WHERE contact_id = ? AND topic = ? AND session_key IS NULL";
    int rc = sqlite3_prepare_v2(
        db, (session_key && slen > 0) ? sql_upd_with_session : sql_upd_null_session, -1, &upd,
        NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_int(upd, 1, (int)belief_type);
    sqlite3_bind_double(upd, 2, (double)clamped);
    sqlite3_bind_int64(upd, 3, turn_number);
    sqlite3_bind_int64(upd, 4, now_ts_ms);
    sqlite3_bind_text(upd, 5, contact_id, (int)strlen(contact_id), SQLITE_STATIC);
    sqlite3_bind_text(upd, 6, topic, (int)tlen, SQLITE_STATIC);
    if (session_key && slen > 0)
        sqlite3_bind_text(upd, 7, session_key, (int)slen, SQLITE_STATIC);
    rc = sqlite3_step(upd);
    sqlite3_finalize(upd);
    if (rc != SQLITE_DONE)
        return HU_ERR_IO;
    if (sqlite3_changes(db) > 0) {
        resolve_expectations_for_belief(db, contact_id, topic, tlen, now_ts_ms);
        return HU_OK;
    }

    /* No row matched — INSERT. */
    sqlite3_stmt *ins = NULL;
    rc = sqlite3_prepare_v2(db,
                            "INSERT INTO tom_user_beliefs("
                            "contact_id, topic, belief_type, confidence, "
                            "session_key, turn_number, last_updated_ts_ms) "
                            "VALUES(?, ?, ?, ?, ?, ?, ?)",
                            -1, &ins, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(ins, 1, contact_id, (int)strlen(contact_id), SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, topic, (int)tlen, SQLITE_STATIC);
    sqlite3_bind_int(ins, 3, (int)belief_type);
    sqlite3_bind_double(ins, 4, (double)clamped);
    if (session_key && slen > 0)
        sqlite3_bind_text(ins, 5, session_key, (int)slen, SQLITE_STATIC);
    else
        sqlite3_bind_null(ins, 5);
    sqlite3_bind_int64(ins, 6, turn_number);
    sqlite3_bind_int64(ins, 7, now_ts_ms);
    rc = sqlite3_step(ins);
    sqlite3_finalize(ins);
    if (rc == SQLITE_DONE) {
        resolve_expectations_for_belief(db, contact_id, topic, tlen, now_ts_ms);
        return HU_OK;
    }
    return HU_ERR_IO;
}

hu_error_t hu_tom_user_beliefs_count_for_contact_session(sqlite3 *db, const char *contact_id,
                                                         const char *session_key,
                                                         int64_t *out_count) {
    if (!db || !contact_id || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    sqlite3_stmt *stmt = NULL;
    int rc;
    if (session_key && session_key[0]) {
        rc = sqlite3_prepare_v2(db,
                                "SELECT COUNT(*) FROM tom_user_beliefs "
                                "WHERE contact_id = ? AND session_key = ?",
                                -1, &stmt, NULL);
        if (rc != SQLITE_OK)
            return HU_ERR_IO;
        sqlite3_bind_text(stmt, 1, contact_id, (int)strlen(contact_id), SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, session_key, (int)strlen(session_key), SQLITE_STATIC);
    } else {
        rc = sqlite3_prepare_v2(db,
                                "SELECT COUNT(*) FROM tom_user_beliefs "
                                "WHERE contact_id = ? AND session_key IS NULL",
                                -1, &stmt, NULL);
        if (rc != SQLITE_OK)
            return HU_ERR_IO;
        sqlite3_bind_text(stmt, 1, contact_id, (int)strlen(contact_id), SQLITE_STATIC);
    }
    if (sqlite3_step(stmt) == SQLITE_ROW)
        *out_count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return HU_OK;
}

/* =====================================================================
 * Phase C of Spec 4: self-change event recording.
 *
 * Spec: tasks 6, 7, 8, 9. Persona-delta-applied, adapter-swap-success,
 * and emotional-register-shift each emit one row. Stored in a
 * dedicated table because (a) event kinds form a small enum, (b)
 * range-scan-by-timestamp queries are the hot path for the Phase D
 * staleness check.
 * ===================================================================== */

hu_error_t hu_tom_self_change_events_init_table(sqlite3 *db) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    const char *sql_table = "CREATE TABLE IF NOT EXISTS tom_self_change_events("
                            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                            "contact_id TEXT NOT NULL, "
                            "event_kind INTEGER NOT NULL, "
                            "session_key TEXT, "
                            "turn_number INTEGER, "
                            "timestamp_utc_ms INTEGER NOT NULL, "
                            "magnitude REAL);";
    char *err_msg = NULL;
    int rc = sqlite3_exec(db, sql_table, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg)
            sqlite3_free(err_msg);
        return HU_ERR_IO;
    }
    const char *sql_idx = "CREATE INDEX IF NOT EXISTS idx_tom_self_change_contact_ts "
                          "ON tom_self_change_events(contact_id, timestamp_utc_ms);";
    rc = sqlite3_exec(db, sql_idx, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg)
            sqlite3_free(err_msg);
        return HU_ERR_IO;
    }
    return HU_OK;
}

hu_error_t hu_tom_record_self_change_event(sqlite3 *db, const char *contact_id,
                                           hu_tom_self_change_kind_t event_kind,
                                           const char *session_key, size_t session_key_len,
                                           int64_t turn_number, double magnitude,
                                           int64_t now_ts_ms) {
    if (!db || !contact_id)
        return HU_ERR_INVALID_ARGUMENT;
    if (event_kind != HU_TOM_SELF_CHANGE_PERSONA_DELTA &&
        event_kind != HU_TOM_SELF_CHANGE_ADAPTER_SWAP &&
        event_kind != HU_TOM_SELF_CHANGE_REGISTER_SHIFT)
        return HU_ERR_INVALID_ARGUMENT;
    size_t slen = session_key_len;
    if (session_key && slen == 0)
        slen = strlen(session_key);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "INSERT INTO tom_self_change_events("
                                "contact_id, event_kind, session_key, turn_number, "
                                "timestamp_utc_ms, magnitude) "
                                "VALUES(?, ?, ?, ?, ?, ?)",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(stmt, 1, contact_id, (int)strlen(contact_id), SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, (int)event_kind);
    if (session_key && slen > 0)
        sqlite3_bind_text(stmt, 3, session_key, (int)slen, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 3);
    sqlite3_bind_int64(stmt, 4, turn_number);
    sqlite3_bind_int64(stmt, 5, now_ts_ms);
    sqlite3_bind_double(stmt, 6, magnitude);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_IO;
}

hu_error_t hu_tom_self_change_events_count(sqlite3 *db, const char *contact_id,
                                           hu_tom_self_change_kind_t event_kind,
                                           int64_t *out_count) {
    if (!db || !contact_id || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    sqlite3_stmt *stmt = NULL;
    int rc;
    if ((int)event_kind == 0) {
        rc = sqlite3_prepare_v2(db,
                                "SELECT COUNT(*) FROM tom_self_change_events WHERE contact_id = ?",
                                -1, &stmt, NULL);
        if (rc != SQLITE_OK)
            return HU_ERR_IO;
        sqlite3_bind_text(stmt, 1, contact_id, (int)strlen(contact_id), SQLITE_STATIC);
    } else {
        rc = sqlite3_prepare_v2(db,
                                "SELECT COUNT(*) FROM tom_self_change_events "
                                "WHERE contact_id = ? AND event_kind = ?",
                                -1, &stmt, NULL);
        if (rc != SQLITE_OK)
            return HU_ERR_IO;
        sqlite3_bind_text(stmt, 1, contact_id, (int)strlen(contact_id), SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, (int)event_kind);
    }
    if (sqlite3_step(stmt) == SQLITE_ROW)
        *out_count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return HU_OK;
}

/* =====================================================================
 * Phase D of Spec 4: staleness gap detection.
 *
 * Spec: task 10. Extends gap-detection to surface self-change-driven
 * invalidation of prior beliefs. D-TOM-6 maps each self-change event
 * kind to the belief-expectation kind it invalidates.
 * ===================================================================== */

hu_tom_expected_knowledge_t
hu_tom_self_change_invalidates_kind(hu_tom_self_change_kind_t event_kind) {
    switch (event_kind) {
    case HU_TOM_SELF_CHANGE_PERSONA_DELTA:
        return HU_TOM_EXPECT_REMEMBERS;
    case HU_TOM_SELF_CHANGE_ADAPTER_SWAP:
        return HU_TOM_EXPECT_UNDERSTANDS;
    case HU_TOM_SELF_CHANGE_REGISTER_SHIFT:
        return HU_TOM_EXPECT_TRACKS;
    }
    /* Defensive default — out-of-range event_kind should never happen
     * (validated on insert), but pick the most conservative bucket
     * (REMEMBERS) so the gap still surfaces. */
    return HU_TOM_EXPECT_REMEMBERS;
}

/* Map a belief_type to the expected_knowledge_type it most naturally
 * satisfies. KNOWS / ASSUMES → REMEMBERS (factual recall); MISTAKEN
 * also REMEMBERS (the user told us, we just got it wrong); UNAWARE
 * doesn't map to any positive expectation. We over-flag UNDERSTANDS
 * and TRACKS by treating every recorded belief as potentially
 * satisfying any kind, then filter on event_kind via D-TOM-6.
 *
 * The simpler approach used here: every persisted belief is a
 * candidate for staleness against every event_kind; the mapping at
 * gap-time picks the expected_kind the caller surfaces. This matches
 * design D-TOM-6: "PERSONA_DELTA -> invalidates beliefs of REMEMBERS"
 * — the BELIEF itself doesn't carry a knowledge_type; the EVENT picks
 * which kind of gap to surface. */
hu_error_t hu_tom_detect_staleness_gaps(sqlite3 *db, hu_allocator_t *alloc, const char *contact_id,
                                        int64_t now_ts_ms, int64_t staleness_window_sec,
                                        size_t max_gaps, hu_tom_staleness_gap_t **out,
                                        size_t *out_count) {
    if (!db || !alloc || !contact_id || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    if (max_gaps == 0)
        max_gaps = 32;
    if (staleness_window_sec <= 0)
        staleness_window_sec = HU_TOM_DEFAULT_STALENESS_WINDOW_SEC;

    int64_t window_ms = staleness_window_sec * 1000;
    int64_t cutoff_ms = now_ts_ms - window_ms;

    /* For each in-window event, find every belief on this contact whose
     * last_updated_ts_ms < event.timestamp_utc_ms. One row per
     * (belief, event) pair below max_gaps. We order by event.timestamp
     * DESC so the most recent change wins if the cap clips us. */
    sqlite3_stmt *stmt = NULL;
    int rc =
        sqlite3_prepare_v2(db,
                           "SELECT b.topic, b.last_updated_ts_ms, e.event_kind, e.timestamp_utc_ms "
                           "FROM tom_user_beliefs b "
                           "JOIN tom_self_change_events e "
                           "  ON b.contact_id = e.contact_id "
                           "WHERE b.contact_id = ? "
                           "  AND b.last_updated_ts_ms < e.timestamp_utc_ms "
                           "  AND e.timestamp_utc_ms > ? "
                           "ORDER BY e.timestamp_utc_ms DESC, b.last_updated_ts_ms ASC "
                           "LIMIT ?",
                           -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(stmt, 1, contact_id, (int)strlen(contact_id), SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, cutoff_ms);
    sqlite3_bind_int64(stmt, 3, (int64_t)max_gaps);

    hu_tom_staleness_gap_t *gaps = (hu_tom_staleness_gap_t *)alloc->alloc(
        alloc->ctx, max_gaps * sizeof(hu_tom_staleness_gap_t));
    if (!gaps) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(gaps, 0, max_gaps * sizeof(hu_tom_staleness_gap_t));

    size_t count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_gaps) {
        const unsigned char *topic = sqlite3_column_text(stmt, 0);
        int topic_bytes = sqlite3_column_bytes(stmt, 0);
        int64_t belief_ts = sqlite3_column_int64(stmt, 1);
        int event_kind_raw = sqlite3_column_int(stmt, 2);
        int64_t event_ts = sqlite3_column_int64(stmt, 3);
        if (!topic || topic_bytes <= 0)
            continue;
        hu_tom_self_change_kind_t ekind = (hu_tom_self_change_kind_t)event_kind_raw;
        char *dup = hu_strndup(alloc, (const char *)topic, (size_t)topic_bytes);
        if (!dup) {
            hu_tom_staleness_gaps_free(alloc, gaps, count);
            sqlite3_finalize(stmt);
            return HU_ERR_OUT_OF_MEMORY;
        }
        gaps[count].topic = dup;
        gaps[count].topic_len = (size_t)topic_bytes;
        gaps[count].expected_kind = hu_tom_self_change_invalidates_kind(ekind);
        gaps[count].event_kind = ekind;
        gaps[count].belief_ts_ms = belief_ts;
        gaps[count].event_ts_ms = event_ts;
        count++;
    }
    sqlite3_finalize(stmt);

    if (count == 0) {
        alloc->free(alloc->ctx, gaps, max_gaps * sizeof(hu_tom_staleness_gap_t));
        return HU_OK;
    }
    *out = gaps;
    *out_count = count;
    return HU_OK;
}

void hu_tom_staleness_gaps_free(hu_allocator_t *alloc, hu_tom_staleness_gap_t *gaps, size_t count) {
    if (!alloc || !gaps)
        return;
    for (size_t i = 0; i < count; i++) {
        if (gaps[i].topic)
            hu_str_free(alloc, gaps[i].topic);
        gaps[i].topic = NULL;
        gaps[i].topic_len = 0;
    }
    alloc->free(alloc->ctx, gaps, count * sizeof(hu_tom_staleness_gap_t));
}

#else  /* !HU_ENABLE_SQLITE: keep the TU non-empty */
typedef int hu_tom_repo_unused_;
#endif /* HU_ENABLE_SQLITE */
