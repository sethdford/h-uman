#ifndef HU_AGENT_OUTBOUND_CROSSTALK_SQLITE_H
#define HU_AGENT_OUTBOUND_CROSSTALK_SQLITE_H

/* Sprint 60 follow-up to Sprint 59 — SQLite-backed lookup for the
 * outbound crosstalk stage's cross-contact bleed check. Closes the
 * "degraded mode" gap noted at
 * docs/plans/2026-05-26-sprint-59-outbound-safety/STATUS.md item #3.
 *
 * The crosstalk stage (src/agent/outbound/crosstalk.c) queries
 * recently-sent content from contacts OTHER than the recipient and
 * Jaccard-compares the outbound payload against each. The stage
 * itself is corpus-agnostic — it talks to a pluggable lookup via
 * hu_outbound_crosstalk_set_lookup. This file is the production
 * SQLite-backed implementation; tests inject fakes.
 */

#include "human/core/allocator.h"
#include <stddef.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lookup function matching hu_outbound_crosstalk_lookup_fn_t.
 *
 * userdata MUST be a non-null sqlite3 *. Reads from the `messages`
 * table — `session_id` is the contact identifier per the h-uman
 * convention (see src/memory/engines/sqlite.c:59 schema).
 *
 * Filter: session_id != exclude_id AND created_at > now - 7 days.
 * Result is capped at HU_OUTBOUND_CROSSTALK_SQLITE_LIMIT rows for
 * bounded work per outbound call (~10ms typical).
 *
 * Returns:
 *   0  → success; out_texts / out_count populated. Caller frees both
 *        the strings and the array via the same allocator, per the
 *        contract in include/human/agent/outbound_pipeline.h.
 *   -1 → error (bad args, SQL failure, allocation failure). out_texts
 *        and out_count are cleared. Stage falls to graceful SEND.
 *
 * The empty-result case returns 0 with *out_texts = NULL and
 * *out_count = 0 (no allocation). */
int hu_outbound_crosstalk_sqlite_lookup(void *userdata, hu_allocator_t *alloc,
                                        const char *exclude_id, size_t exclude_id_len,
                                        char ***out_texts, size_t *out_count);

/* Convenience: install hu_outbound_crosstalk_sqlite_lookup with `db`
 * as userdata via hu_outbound_crosstalk_set_lookup. The daemon calls
 * this once after its SQLite handle is open and corresponding
 * _unregister_sqlite before closing the handle, so the static
 * callback never sees a freed sqlite3 *.
 *
 * Passing db == NULL is equivalent to _unregister_sqlite (clears the
 * lookup; the stage falls to graceful degraded SEND). */
void hu_outbound_crosstalk_register_sqlite(sqlite3 *db);

/* Unregister the SQLite lookup. Idempotent. The stage's cross-contact
 * check then runs in degraded mode (metadata-pattern check still
 * fires; the cross-contact Jaccard check is skipped). */
void hu_outbound_crosstalk_unregister_sqlite(void);

#ifdef __cplusplus
}
#endif

#endif /* HU_ENABLE_SQLITE */
#endif
