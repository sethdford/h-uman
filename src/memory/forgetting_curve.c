#include "human/memory/forgetting_curve.h"
#include <math.h>
#include <stddef.h>

#define SECONDS_PER_DAY 86400.0
#define EMOTIONAL_ANCHOR_DECAY_FACTOR 0.3
#define BATCH_DECAY_THRESHOLD 0.05

double hu_forgetting_decayed_salience(double initial_salience, double decay_rate,
                                      int64_t created_at, int64_t now_ts,
                                      bool is_emotional_anchor) {
    if (initial_salience <= 0.0)
        return 0.0;
    if (now_ts <= created_at)
        return initial_salience;

    double days = (double)(now_ts - created_at) / SECONDS_PER_DAY;
    if (days <= 0.0)
        return initial_salience;

    double effective_decay = is_emotional_anchor ? decay_rate * EMOTIONAL_ANCHOR_DECAY_FACTOR
                                               : decay_rate;
    return initial_salience * exp(-effective_decay * days);
}

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#include <stddef.h>

hu_error_t hu_forgetting_apply_batch_decay(void *db, int64_t now_ts, double rate) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3 *sqlite_db = (sqlite3 *)db;

    /* Episodes table must have: salience_score, impact_score, created_at.
     * Use created_at for days. Emotional anchors (impact_score > 0.8) use 0.3x decay. */
    const char *sql =
        "UPDATE episodes SET salience_score = salience_score * exp(-"
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
    sqlite3_bind_double(stmt, 4, BATCH_DECAY_THRESHOLD);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_BACKEND;
    return HU_OK;
}
#endif
