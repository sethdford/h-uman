#ifndef HU_MEMORY_CONTACT_STAGE_REPO_H
#define HU_MEMORY_CONTACT_STAGE_REPO_H
/*
 * Contact-stage repository (DEF-16): per-contact interaction counts read from
 * the session store's `messages` table (one row per message; session_id is
 * the contact), and the persisted per-contact stage in `frontier_state`.
 * Counts only: no message text leaves this file.
 *
 * Free functions over a borrowed `sqlite3 *db` from hu_sqlite_memory_get_db();
 * domain callers never include sqlite3.h (sqlite-includer ratchet).
 */
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*hu_contact_stage_repo_row_fn)(void *ctx, const char *contact, size_t contact_len,
                                             uint32_t inbound, uint32_t outbound,
                                             uint32_t active_days);

/* One callback per contact with any message: inbound (role 'user'), outbound
 * (role 'assistant') and distinct calendar days. Missing table -> no rows,
 * HU_OK. Order: session_id ascending. */
hu_error_t hu_contact_stage_repo_each_contact(sqlite3 *db, hu_contact_stage_repo_row_fn fn,
                                              void *ctx);

/* Overwrite the persisted stage of an EXISTING frontier_state row (no
 * insert: a new row would make the frontier loader treat the contact as
 * restored, with column defaults; the end-of-turn frontier save creates it).
 * Missing table or row -> HU_OK, nothing written. */
hu_error_t hu_contact_stage_repo_update_persisted(sqlite3 *db, const char *contact,
                                                  size_t contact_len, int stage, int sessions,
                                                  int turns);

/* counts[stage] = number of frontier_state rows persisted at that stage
 * (stages 0..3; out-of-range values are ignored). Missing table -> zeros. */
hu_error_t hu_contact_stage_repo_persisted_counts(sqlite3 *db, uint32_t counts[4]);

#ifdef __cplusplus
}
#endif

#endif /* HU_ENABLE_SQLITE */
#endif /* HU_MEMORY_CONTACT_STAGE_REPO_H */
