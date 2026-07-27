/* src/memory/repos/forgetting_repo_sqlite.c
 * The forgetting repository: where the `episodes` salience-decay SQL and the
 * raw sqlite3 handle live. Domain code (src/memory/forgetting_curve.c) keeps
 * the Ebbinghaus math and depends on hu_forgetting_repo.h, never on sqlite3.
 * The `episodes` table is owned/created by the sqlite engine, so this repo
 * issues no DDL. */
#ifdef HU_ENABLE_SQLITE
#include "human/memory/forgetting_repo.h"
#include <sqlite3.h>
#include <stddef.h> /* NULL */

hu_error_t hu_forgetting_repo_apply_batch_decay(void *db, int64_t now_ts, double rate,
                                                double min_salience) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3 *sqlite_db = (sqlite3 *)db;

    /* Verbatim SQL from the prior forgetting_curve.c inline implementation.
     * Episodes table must have: salience_score, impact_score, created_at.
     * Emotional anchors (impact_score > 0.8) decay at 0.3x — the same factor
     * hu_forgetting_decayed_salience applies in the pure path. */
    const char *sql = "UPDATE episodes SET salience_score = salience_score * exp(-"
                      "CASE WHEN impact_score > 0.8 THEN ? * 0.3 ELSE ? END "
                      "* ((? - COALESCE(created_at, 0)) / 86400.0)) "
                      "WHERE salience_score > ?";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(sqlite_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_double(stmt, 1, rate);
    sqlite3_bind_double(stmt, 2, rate);
    sqlite3_bind_int64(stmt, 3, now_ts);
    sqlite3_bind_double(stmt, 4, min_salience);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_BACKEND;
    return HU_OK;
}
#endif /* HU_ENABLE_SQLITE */
