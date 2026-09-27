#ifndef HU_MEMORY_CONTACT_OPTOUT_REPO_H
#define HU_MEMORY_CONTACT_OPTOUT_REPO_H
/*
 * Contact opt-out repository (October roadmap O5, contestability).
 *
 * A system that texts real people as Seth must give the recipient a way to
 * decline proactive contact. Before 2026-09-20 there was none: the
 * `unanswered_count` backoff governor is a one-way damper, not consent. This
 * table records the contacts who asked us to stop; the daemon's proactive
 * candidate loop skips them from the next tick.
 *
 * One row per contact (UNIQUE), upserted so a repeated request refreshes ts.
 * Reactive replies to the contact's OWN messages are unaffected — that reply
 * is the acknowledgement; only initiative is suppressed.
 *
 * Same shape as proactive_decisions_repo.h: free functions over a borrowed
 * `sqlite3 *db` from hu_sqlite_memory_get_db(); domain callers never include
 * sqlite3.h (sqlite-includer ratchet).
 */
#include "human/core/error.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Idempotent CREATE TABLE IF NOT EXISTS contact_suppressions. */
hu_error_t hu_contact_optout_repo_ensure_schema(sqlite3 *db);

/* Upsert one suppression. contact non-empty; reason/excerpt may be NULL.
 * excerpt is the inbound text that triggered it (caller caps its length). */
hu_error_t hu_contact_optout_repo_record(sqlite3 *db, int64_t ts, const char *contact,
                                         const char *reason, const char *excerpt);

/* *out = a row exists for contact. Missing table reads as not suppressed. */
hu_error_t hu_contact_optout_repo_is_suppressed(sqlite3 *db, const char *contact, bool *out);

/* Operator reversal ("you can text me again"): delete the row. OK if absent. */
hu_error_t hu_contact_optout_repo_clear(sqlite3 *db, const char *contact);

/* Number of suppressed contacts — the doctor card number. */
hu_error_t hu_contact_optout_repo_count(sqlite3 *db, int64_t *out);

#ifdef __cplusplus
}
#endif
#endif /* HU_ENABLE_SQLITE */
#endif /* HU_MEMORY_CONTACT_OPTOUT_REPO_H */
