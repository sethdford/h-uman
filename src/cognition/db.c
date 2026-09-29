#include "human/cognition/db.h"
#include "human/core/paths.h"

#ifdef HU_ENABLE_SQLITE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

hu_error_t hu_cognition_db_open(sqlite3 **out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;

#ifdef HU_IS_TEST
    const char *path = ":memory:";
#else
    static char path_buf[1024];
    hu_paths_state(path_buf, sizeof(path_buf), "cognition.db");
    path_buf[sizeof(path_buf) - 1] = '\0';
    const char *path = path_buf;
#endif

    int rc = sqlite3_open(path, out);
    if (rc != SQLITE_OK) {
        if (*out)
            sqlite3_close(*out);
        *out = NULL;
        return HU_ERR_IO;
    }

    sqlite3_exec(*out, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(*out, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);

    return hu_cognition_db_ensure_schema(*out);
}

hu_error_t hu_cognition_db_ensure_schema(sqlite3 *db) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;

    static const char *ddl[] = {"CREATE TABLE IF NOT EXISTS skill_invocations ("
                                "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                "  skill_name TEXT NOT NULL,"
                                "  contact_id TEXT DEFAULT '',"
                                "  session_id TEXT DEFAULT '',"
                                "  timestamp TEXT NOT NULL DEFAULT (datetime('now')),"
                                "  explicit INTEGER NOT NULL DEFAULT 0,"
                                "  outcome INTEGER DEFAULT 0,"
                                "  turn_count INTEGER DEFAULT 0"
                                ")",

                                "CREATE TABLE IF NOT EXISTS skill_profiles ("
                                "  skill_name TEXT NOT NULL,"
                                "  contact_id TEXT NOT NULL DEFAULT '',"
                                "  total_invocations INTEGER DEFAULT 0,"
                                "  positive_outcomes INTEGER DEFAULT 0,"
                                "  negative_outcomes INTEGER DEFAULT 0,"
                                "  decayed_score REAL DEFAULT 0.5,"
                                "  last_updated TEXT DEFAULT (datetime('now')),"
                                "  PRIMARY KEY (skill_name, contact_id)"
                                ")",

                                "CREATE TABLE IF NOT EXISTS episodic_patterns ("
                                "  id TEXT PRIMARY KEY,"
                                "  problem_type TEXT NOT NULL,"
                                "  approach TEXT NOT NULL,"
                                "  skills_used TEXT DEFAULT '',"
                                "  outcome_quality REAL DEFAULT 0.5,"
                                "  support_count INTEGER DEFAULT 1,"
                                "  insight TEXT DEFAULT '',"
                                "  session_id TEXT DEFAULT '',"
                                "  timestamp TEXT NOT NULL DEFAULT (datetime('now'))"
                                ")",

                                "CREATE TABLE IF NOT EXISTS metacog_history ("
                                "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                "  trace_id TEXT,"
                                "  iteration INTEGER DEFAULT 0,"
                                "  confidence REAL DEFAULT 0.0,"
                                "  coherence REAL DEFAULT 0.0,"
                                "  repetition REAL DEFAULT 0.0,"
                                "  action TEXT DEFAULT 'none',"
                                "  difficulty TEXT DEFAULT 'unknown',"
                                "  stuck_score REAL DEFAULT 0.0,"
                                "  satisfaction_proxy REAL DEFAULT 0.0,"
                                "  trajectory_confidence REAL DEFAULT 0.0,"
                                "  outcome_proxy REAL DEFAULT 0.0,"
                                "  regen_applied INTEGER DEFAULT 0,"
                                "  timestamp TEXT NOT NULL DEFAULT (datetime('now'))"
                                ")",

                                "CREATE INDEX IF NOT EXISTS idx_skill_inv_contact "
                                "ON skill_invocations(contact_id, session_id)",

                                "CREATE INDEX IF NOT EXISTS idx_episodic_type "
                                "ON episodic_patterns(problem_type)",

                                NULL};

    for (size_t i = 0; ddl[i]; i++) {
        char *err_msg = NULL;
        int rc = sqlite3_exec(db, ddl[i], NULL, NULL, &err_msg);
        if (rc != SQLITE_OK) {
            if (err_msg)
                sqlite3_free(err_msg);
            return HU_ERR_IO;
        }
    }

    /* Idempotent migrations for existing metacog_history rows */
    static const char *metacog_alters[] = {
        "ALTER TABLE metacog_history ADD COLUMN difficulty TEXT DEFAULT 'unknown'",
        "ALTER TABLE metacog_history ADD COLUMN stuck_score REAL DEFAULT 0.0",
        "ALTER TABLE metacog_history ADD COLUMN satisfaction_proxy REAL DEFAULT 0.0",
        "ALTER TABLE metacog_history ADD COLUMN trajectory_confidence REAL DEFAULT 0.0",
        "ALTER TABLE metacog_history ADD COLUMN outcome_proxy REAL DEFAULT 0.0",
        "ALTER TABLE metacog_history ADD COLUMN regen_applied INTEGER DEFAULT 0",
        "ALTER TABLE metacog_history ADD COLUMN prompt_tokens INTEGER DEFAULT 0",
        "ALTER TABLE metacog_history ADD COLUMN completion_tokens INTEGER DEFAULT 0",
        "ALTER TABLE metacog_history ADD COLUMN logprob_mean REAL DEFAULT -1.0",
        "ALTER TABLE metacog_history ADD COLUMN risk_score REAL DEFAULT 0.0",
        NULL,
    };
    for (size_t a = 0; metacog_alters[a]; a++)
        (void)sqlite3_exec(db, metacog_alters[a], NULL, NULL, NULL);

    return HU_OK;
}

hu_error_t hu_metacog_history_insert(sqlite3 *db, const char *trace_id, int iteration,
                                     float confidence, float coherence, float repetition,
                                     float stuck_score, float satisfaction_proxy,
                                     float trajectory_confidence, const char *action,
                                     const char *difficulty, int regen_applied,
                                     const hu_metacog_history_extra_t *extra_opt) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;

    static const char sql[] =
        "INSERT INTO metacog_history (trace_id, iteration, confidence, coherence, repetition, "
        "action, difficulty, stuck_score, satisfaction_proxy, trajectory_confidence, "
        "outcome_proxy, regen_applied, prompt_tokens, completion_tokens, logprob_mean, risk_score) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,0.0,?,?,?,?,?)";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_text(st, 1, trace_id ? trace_id : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, iteration);
    sqlite3_bind_double(st, 3, (double)confidence);
    sqlite3_bind_double(st, 4, (double)coherence);
    sqlite3_bind_double(st, 5, (double)repetition);
    sqlite3_bind_text(st, 6, action ? action : "none", -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 7, difficulty ? difficulty : "unknown", -1, SQLITE_STATIC);
    sqlite3_bind_double(st, 8, (double)stuck_score);
    sqlite3_bind_double(st, 9, (double)satisfaction_proxy);
    sqlite3_bind_double(st, 10, (double)trajectory_confidence);
    sqlite3_bind_int(st, 11, regen_applied);
    if (extra_opt) {
        sqlite3_bind_int64(st, 12, (sqlite3_int64)extra_opt->prompt_tokens);
        sqlite3_bind_int64(st, 13, (sqlite3_int64)extra_opt->completion_tokens);
        sqlite3_bind_double(st, 14, (double)extra_opt->logprob_mean);
        sqlite3_bind_double(st, 15, (double)extra_opt->risk_score);
    } else {
        sqlite3_bind_int64(st, 12, 0);
        sqlite3_bind_int64(st, 13, 0);
        sqlite3_bind_double(st, 14, -1.0);
        sqlite3_bind_double(st, 15, 0.0);
    }

    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_IO;
}

hu_error_t hu_metacog_history_update_outcome(sqlite3 *db, const char *trace_id,
                                             float outcome_proxy) {
    if (!db || !trace_id || trace_id[0] == '\0')
        return HU_ERR_INVALID_ARGUMENT;

    static const char sql[] = "UPDATE metacog_history SET outcome_proxy = ? WHERE id = ("
                              "SELECT id FROM metacog_history WHERE trace_id = ? "
                              "ORDER BY id DESC LIMIT 1)";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_double(st, 1, (double)outcome_proxy);
    sqlite3_bind_text(st, 2, trace_id, -1, SQLITE_STATIC);

    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_IO;
}

void hu_cognition_db_close(sqlite3 *db) {
    if (db)
        sqlite3_close(db);
}

#endif /* HU_ENABLE_SQLITE */
