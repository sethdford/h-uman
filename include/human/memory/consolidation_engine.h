#ifndef HU_MEMORY_CONSOLIDATION_ENGINE_H
#define HU_MEMORY_CONSOLIDATION_ENGINE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdint.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

/* Phase 7 F72 — Memory Consolidation Engine.
 * Nightly/weekly/monthly offline processing. Extends memory with tiered consolidation.
 */

typedef struct hu_consolidation_engine {
    hu_allocator_t *alloc;
    sqlite3 *db;
} hu_consolidation_engine_t;

/* Nightly: apply forgetting curve decay; deduplicate episodes (same contact, similar
 * summaries); merge by keeping higher impact, delete duplicates. */
hu_error_t hu_consolidation_engine_nightly(hu_consolidation_engine_t *engine, int64_t now_ts);

/* Weekly: for contacts with > 5 episodes in last 7 days, create a summary episode
 * "This week with [contact]: ..." aggregating key_moments. */
hu_error_t hu_consolidation_engine_weekly(hu_consolidation_engine_t *engine, int64_t now_ts);

/* Monthly: delete episodes with salience_score < 0.05 and older than 90 days. */
hu_error_t hu_consolidation_engine_monthly(hu_consolidation_engine_t *engine, int64_t now_ts);

/* Run scheduled tasks if enough time has passed: nightly 20+ hours, weekly 6+ days,
 * monthly 25+ days. last_* are 0 if never run. */
hu_error_t hu_consolidation_engine_run_scheduled(hu_consolidation_engine_t *engine,
                                                 int64_t now_ts, int64_t last_nightly,
                                                 int64_t last_weekly, int64_t last_monthly);

#endif /* HU_ENABLE_SQLITE */

#endif /* HU_MEMORY_CONSOLIDATION_ENGINE_H */
