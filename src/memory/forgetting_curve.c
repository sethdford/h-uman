#include "human/memory/forgetting_curve.h"
#include <math.h>
#include <stddef.h>

#define SECONDS_PER_DAY               86400.0
#define EMOTIONAL_ANCHOR_DECAY_FACTOR 0.3
#define BATCH_DECAY_THRESHOLD         0.05

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

    double effective_decay =
        is_emotional_anchor ? decay_rate * EMOTIONAL_ANCHOR_DECAY_FACTOR : decay_rate;
    return initial_salience * exp(-effective_decay * days);
}

#ifdef HU_ENABLE_SQLITE
#include "human/memory/forgetting_repo.h"

/* The curve math above is a pure function of (salience, rate, age); applying it
 * across stored episodes is a storage concern. The SQL and the sqlite3 handle
 * therefore live in src/memory/repos/forgetting_repo_sqlite.c, where the
 * include is legal — this file keeps only the policy constant it owns
 * (BATCH_DECAY_THRESHOLD: rows already below it are not worth rewriting).
 * Signature and error contract are unchanged for all three callers. */
hu_error_t hu_forgetting_apply_batch_decay(void *db, int64_t now_ts, double rate) {
    return hu_forgetting_repo_apply_batch_decay(db, now_ts, rate, BATCH_DECAY_THRESHOLD);
}
#endif
