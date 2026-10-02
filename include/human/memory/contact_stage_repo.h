#ifndef HU_MEMORY_CONTACT_STAGE_REPO_H
#define HU_MEMORY_CONTACT_STAGE_REPO_H
/*
 * Contact-stage repository (DEF-16).
 *
 * Reads per-contact counts of the contact's OWN messages from the session
 * store's `messages` table (role 'user'; session_id is the contact). The
 * 'assistant' rows are the twin's replies and are never counted. Writes the
 * derived stage to its own table, `contact_rel_stage`, never to
 * frontier_state, so turning HU_REL_STAGE_DERIVED off restores the old
 * agent-wide behaviour with no data cleanup. Counts only: no text.
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
                                             uint32_t inbound, uint32_t active_days);

/* One callback per contact that wrote at least once: their message count and
 * distinct calendar days. Missing table -> no rows, HU_OK. */
hu_error_t hu_contact_stage_repo_each_contact(sqlite3 *db, hu_contact_stage_repo_row_fn fn,
                                              void *ctx);

/* The same counts for one contact (indexed lookup). Unknown contact -> 0/0. */
hu_error_t hu_contact_stage_repo_contact_counts(sqlite3 *db, const char *contact,
                                                size_t contact_len, uint32_t *inbound,
                                                uint32_t *active_days);

/* counts[stage] = number of frontier_state rows persisted at that stage
 * (stages 0..3). Missing table -> zeros. */
hu_error_t hu_contact_stage_repo_persisted_counts(sqlite3 *db, uint32_t counts[4]);

/* Upsert the derived stage of one contact into contact_rel_stage. */
hu_error_t hu_contact_stage_repo_save_derived(sqlite3 *db, const char *contact, size_t contact_len,
                                              int stage, double quality, uint32_t inbound,
                                              uint32_t active_days, int64_t seth_replies,
                                              int64_t now_unix);

/* Read one derived row. HU_ERR_NOT_FOUND when absent (or no table). */
hu_error_t hu_contact_stage_repo_get_derived(sqlite3 *db, const char *contact, size_t contact_len,
                                             int *stage, double *quality, int64_t *seth_replies);

/* counts[stage] over contact_rel_stage rows. Missing table -> zeros. */
hu_error_t hu_contact_stage_repo_derived_counts(sqlite3 *db, uint32_t counts[4]);

#ifdef __cplusplus
}
#endif

#endif /* HU_ENABLE_SQLITE */
#endif /* HU_MEMORY_CONTACT_STAGE_REPO_H */
