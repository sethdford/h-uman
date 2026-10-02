#ifndef HU_MEMORY_CONFIDENCE_REPO_H
#define HU_MEMORY_CONFIDENCE_REPO_H
/*
 * Provenance columns on `memories` for the confidence boundary
 * (include/human/memory/confidence_boundary.h):
 *   source_contact TEXT    — the contact a private row came from ("" unknown)
 *   share_level    INTEGER — hu_share_level_t (NULL until stamped)
 *
 * The columns are metadata only: nothing reads them unless
 * HU_CONFIDENCE_BOUNDARY is shadow or live, so they never change a prompt
 * with the gate off. Free functions over the engine's sqlite3 handle; domain
 * callers go through the hu_memory_t forms and never include sqlite3.h.
 */
#include "human/memory.h"
#include "human/memory/confidence_boundary.h"
#include <stdbool.h>
#include <stddef.h>

/* Read a stored row's provenance by key. False when the key is not stored,
 * mem is not sqlite, or the build has no SQLite. An unstamped row reads as
 * HU_SHARE_UNSET with contact "". */
bool hu_confidence_repo_lookup(hu_memory_t *mem, const char *key, size_t key_len,
                               hu_share_level_t *level, char *contact, size_t cap);

/* Re-stamp a stored row as written during `contact`'s conversation (owner when
 * empty), unless its session or key already names its source. */
void hu_confidence_repo_stamp_write(hu_memory_t *mem, const char *key, size_t key_len,
                                    const char *contact, size_t contact_len);

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

/* Idempotent: add the two columns, then backfill every row whose share_level
 * is NULL with hu_confidence_derive_row (conservative: unknown -> private).
 * Returns the number of rows backfilled, or -1 on error. */
int hu_confidence_repo_ensure_schema(sqlite3 *db);

/* Write-time stamp for a row the engine just stored (derive from its key,
 * session and source). Silent on error: provenance is additive metadata. */
void hu_confidence_repo_stamp(sqlite3 *db, const char *key, size_t key_len, const char *session,
                              size_t session_len);
#endif

#endif /* HU_MEMORY_CONFIDENCE_REPO_H */
