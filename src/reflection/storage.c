/* src/reflection/storage.c — Reflection SQLite storage layer (T2).
 *
 * Spec: docs/plans/2026-05-26-reflection-loop/tasks.md Task 2 +
 *       design.md "Components → src/reflection/storage.c".
 *
 * Two tables (DDL is the canonical schema; future migrations must
 * preserve column order for the SELECT * call sites):
 *   reflection_runs       — one row per reflection invocation
 *   reflection_patterns   — one row per stable-id pattern (UPSERT)
 *
 * Confidence floor (0.5) is applied at the UPSERT boundary: parse
 * keeps low-confidence patterns so the count can be reported, but
 * storage drops them silently. Caller bumps low_confidence_dropped_count
 * via complete_run for telemetry.
 *
 * UPSERT chooses MAX(confidence) on conflict so a stronger
 * re-observation of a pattern doesn't get downgraded by a later run
 * that happened to see weaker evidence. observation_count is the
 * canonical "how many runs noticed this" metric used by the Phase 2
 * quorum predicate.
 *
 * JSON1 dependency: evidence_ids and channels are stored as JSON
 * arrays. The channel-filter query in consumer.c uses json_each() to
 * unpack them on read. */

#include "human/reflection.h"

#include "human/core/log.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Schema DDL ────────────────────────────────────────────────── */

static const char *const k_migrate_sql =
    "CREATE TABLE IF NOT EXISTS reflection_runs ("
    "  run_id TEXT PRIMARY KEY,"
    "  provider TEXT NOT NULL,"
    "  started_at_ms INTEGER NOT NULL,"
    "  completed_at_ms INTEGER,"
    "  input_turns INTEGER NOT NULL,"
    "  input_tokens INTEGER,"
    "  output_tokens INTEGER,"
    "  status TEXT NOT NULL,"
    "  error_message TEXT,"
    "  json_dump_path TEXT,"
    "  prose_summary TEXT,"
    "  low_confidence_dropped_count INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE TABLE IF NOT EXISTS reflection_patterns ("
    "  id TEXT PRIMARY KEY,"
    "  type TEXT NOT NULL,"
    "  subject TEXT NOT NULL,"
    "  observation TEXT NOT NULL,"
    "  confidence REAL NOT NULL,"
    "  evidence_json TEXT NOT NULL,"
    "  channels_json TEXT NOT NULL,"
    "  first_seen_run_id TEXT NOT NULL REFERENCES reflection_runs(run_id),"
    "  last_seen_run_id TEXT NOT NULL REFERENCES reflection_runs(run_id),"
    "  observation_count INTEGER NOT NULL DEFAULT 1,"
    "  created_at_ms INTEGER NOT NULL,"
    "  last_observed_at_ms INTEGER NOT NULL,"
    "  expires_at_ms INTEGER NOT NULL,"
    "  surfaced_to_user INTEGER NOT NULL DEFAULT 0,"
    "  retired INTEGER NOT NULL DEFAULT 0,"
    "  retired_at_ms INTEGER"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_patterns_recent "
    "  ON reflection_patterns(last_observed_at_ms DESC);"
    "CREATE INDEX IF NOT EXISTS idx_patterns_unsurfaced "
    "  ON reflection_patterns(surfaced_to_user, retired, confidence DESC);";

hu_error_t hu_reflection_storage_migrate(struct sqlite3 *db) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    char *err = NULL;
    int rc = sqlite3_exec(db, k_migrate_sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        hu_log_error("reflection", NULL, "storage migrate failed: %s", err ? err : "?");
        sqlite3_free(err);
        return HU_ERR_MEMORY_BACKEND;
    }
    return HU_OK;
}

/* ── insert_run ────────────────────────────────────────────────── */

hu_error_t hu_reflection_storage_insert_run(struct sqlite3 *db, const char *run_id,
                                            const char *provider, uint64_t started_at_ms,
                                            int input_turns) {
    if (!db || !run_id || !provider)
        return HU_ERR_INVALID_ARGUMENT;
    const char *sql = "INSERT INTO reflection_runs"
                      " (run_id, provider, started_at_ms, input_turns, status)"
                      " VALUES (?, ?, ?, ?, 'in_progress')";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        hu_log_error("reflection", NULL, "insert_run prepare: %s", sqlite3_errmsg(db));
        return HU_ERR_MEMORY_BACKEND;
    }
    sqlite3_bind_text(st, 1, run_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, provider, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)started_at_ms);
    sqlite3_bind_int(st, 4, input_turns);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        hu_log_error("reflection", NULL, "insert_run step rc=%d: %s", rc, sqlite3_errmsg(db));
        return HU_ERR_MEMORY_BACKEND;
    }
    return HU_OK;
}

/* ── complete_run ──────────────────────────────────────────────── */

hu_error_t hu_reflection_storage_complete_run(struct sqlite3 *db, const char *run_id,
                                              const char *status, int output_tokens,
                                              const char *prose_summary, const char *json_dump_path,
                                              const char *error_message,
                                              int low_confidence_dropped_count) {
    if (!db || !run_id || !status)
        return HU_ERR_INVALID_ARGUMENT;
    const char *sql = "UPDATE reflection_runs SET"
                      " completed_at_ms = ?,"
                      " status = ?,"
                      " output_tokens = ?,"
                      " prose_summary = ?,"
                      " json_dump_path = ?,"
                      " error_message = ?,"
                      " low_confidence_dropped_count = ?"
                      " WHERE run_id = ?";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        hu_log_error("reflection", NULL, "complete_run prepare: %s", sqlite3_errmsg(db));
        return HU_ERR_MEMORY_BACKEND;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL) * 1000);
    sqlite3_bind_text(st, 2, status, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 3, output_tokens);
    if (prose_summary)
        sqlite3_bind_text(st, 4, prose_summary, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(st, 4);
    if (json_dump_path)
        sqlite3_bind_text(st, 5, json_dump_path, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(st, 5);
    if (error_message)
        sqlite3_bind_text(st, 6, error_message, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(st, 6);
    sqlite3_bind_int(st, 7, low_confidence_dropped_count);
    sqlite3_bind_text(st, 8, run_id, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        hu_log_error("reflection", NULL, "complete_run step rc=%d: %s", rc, sqlite3_errmsg(db));
        return HU_ERR_MEMORY_BACKEND;
    }
    return HU_OK;
}

/* ── upsert (the load-bearing piece) ───────────────────────────── */

/* Serialize a fixed-size array of bounded strings as a JSON array.
 * Output is malloc'd; caller frees. */
static char *json_array_from_evidence(const char arr[][64], int count) {
    size_t cap = (size_t)count * (64 + 4) + 4;
    char *buf = (char *)malloc(cap);
    if (!buf)
        return NULL;
    size_t pos = 0;
    buf[pos++] = '[';
    for (int i = 0; i < count; i++) {
        if (i > 0)
            buf[pos++] = ',';
        buf[pos++] = '"';
        const char *s = arr[i];
        while (*s && pos < cap - 3) {
            char c = *s++;
            if (c == '"' || c == '\\')
                buf[pos++] = '\\';
            if (c < 0x20)
                continue;
            buf[pos++] = c;
        }
        buf[pos++] = '"';
    }
    buf[pos++] = ']';
    buf[pos] = '\0';
    return buf;
}

static char *json_array_from_channels(const char arr[][32], int count) {
    size_t cap = (size_t)count * (32 + 4) + 4;
    char *buf = (char *)malloc(cap);
    if (!buf)
        return NULL;
    size_t pos = 0;
    buf[pos++] = '[';
    for (int i = 0; i < count; i++) {
        if (i > 0)
            buf[pos++] = ',';
        buf[pos++] = '"';
        const char *s = arr[i];
        while (*s && pos < cap - 3) {
            char c = *s++;
            if (c == '"' || c == '\\')
                buf[pos++] = '\\';
            if (c < 0x20)
                continue;
            buf[pos++] = c;
        }
        buf[pos++] = '"';
    }
    buf[pos++] = ']';
    buf[pos] = '\0';
    return buf;
}

hu_error_t hu_reflection_storage_upsert(struct sqlite3 *db, const char *run_id,
                                        const hu_reflection_pattern_t *pattern) {
    if (!db || !run_id || !pattern)
        return HU_ERR_INVALID_ARGUMENT;
    if (pattern->confidence < 0.5) {
        return HU_OK;
    }

    char *evidence_json = json_array_from_evidence(pattern->evidence_ids, pattern->evidence_count);
    char *channels_json = json_array_from_channels(pattern->channels, pattern->channel_count);
    if (!evidence_json || !channels_json) {
        free(evidence_json);
        free(channels_json);
        return HU_ERR_OUT_OF_MEMORY;
    }

    const char *sql = "INSERT INTO reflection_patterns"
                      " (id, type, subject, observation, confidence,"
                      "  evidence_json, channels_json,"
                      "  first_seen_run_id, last_seen_run_id,"
                      "  observation_count, created_at_ms, last_observed_at_ms,"
                      "  expires_at_ms, surfaced_to_user, retired, retired_at_ms)"
                      " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 1, ?, ?, ?, 0, 0, NULL)"
                      " ON CONFLICT(id) DO UPDATE SET"
                      "   observation_count = observation_count + 1,"
                      "   last_observed_at_ms = excluded.last_observed_at_ms,"
                      "   confidence = MAX(confidence, excluded.confidence),"
                      "   last_seen_run_id = excluded.last_seen_run_id,"
                      "   expires_at_ms = excluded.expires_at_ms,"
                      "   evidence_json = excluded.evidence_json,"
                      "   channels_json = excluded.channels_json";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        hu_log_error("reflection", NULL, "upsert prepare: %s", sqlite3_errmsg(db));
        free(evidence_json);
        free(channels_json);
        return HU_ERR_MEMORY_BACKEND;
    }
    const char *type_str = hu_reflection_pattern_type_str(pattern->type);
    sqlite3_bind_text(st, 1, pattern->id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, type_str, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, pattern->subject, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, pattern->observation, -1, SQLITE_STATIC);
    sqlite3_bind_double(st, 5, pattern->confidence);
    sqlite3_bind_text(st, 6, evidence_json, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 7, channels_json, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 8, run_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 9, run_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 10, (sqlite3_int64)pattern->created_at_ms);
    sqlite3_bind_int64(st, 11, (sqlite3_int64)pattern->last_observed_at_ms);
    sqlite3_bind_int64(st, 12, (sqlite3_int64)pattern->expires_at_ms);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    free(evidence_json);
    free(channels_json);
    if (rc != SQLITE_DONE) {
        hu_log_error("reflection", NULL, "upsert step rc=%d: %s", rc, sqlite3_errmsg(db));
        return HU_ERR_MEMORY_BACKEND;
    }
    return HU_OK;
}

uint64_t hu_reflection_storage_last_completed_ms(struct sqlite3 *db) {
    if (!db)
        return 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT MAX(completed_at_ms) FROM reflection_runs WHERE status='ok'",
                           -1, &st, NULL) != SQLITE_OK)
        return 0;
    uint64_t out = 0;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_INTEGER)
        out = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return out;
}

/* ── Phase 2 quorum predicate (T11) ────────────────────────────── */

/* Returns true iff `pattern_id` has been observed in ≥ 3 distinct
 * runs with confidence > 0.7. Phase 1 callers MUST treat this as
 * TELEMETRY ONLY — mutation against hu_personal_model_t on the
 * basis of quorum is Phase 2 work, gated by
 * scripts/check-reflection-quorum-not-wired.sh.
 *
 * Phase 1 approximation: storage keeps only MAX(confidence) per
 * pattern, not per-run confidence. For the strict "≥3 distinct runs
 * each > 0.7" check Phase 2 will want a separate observations
 * (pattern_id, run_id, confidence) table. For now we use the looser
 * "observation_count >= 3 AND MAX confidence > 0.7" — documented in
 * design.md "Open questions" and revisited before Phase 2 wires
 * belief updates. */
bool hu_reflection_pattern_has_quorum(struct sqlite3 *db, const char *pattern_id) {
    if (!db || !pattern_id)
        return false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT observation_count, confidence FROM reflection_patterns "
                           "WHERE id = ? AND retired = 0",
                           -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, pattern_id, -1, SQLITE_STATIC);
    bool has = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        int obs_count = sqlite3_column_int(st, 0);
        double conf = sqlite3_column_double(st, 1);
        has = (obs_count >= 3 && conf > 0.7);
    }
    sqlite3_finalize(st);
    return has;
}
