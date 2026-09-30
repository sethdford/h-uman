#ifndef HU_MEMORY_PROSPECTIVE_REPO_H
#define HU_MEMORY_PROSPECTIVE_REPO_H
/*
 * Prospective memory v2 — the typed intention store (spec
 * docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.1).
 *
 * prospective_memories gains typed columns, additively: cue_kind, due_at,
 * status, surfaced_at, attempts, outcome, source. The legacy `fired` column
 * stays authoritative for every pre-v2 reader (the curator, the nightly eval,
 * the HU_PROSPECTIVE=off path); `status` is derived from it — 0 pending,
 * 1 done, 2 canceled, 3 expired — by a trigger whenever a legacy writer
 * changes `fired`, and v2 writers set both columns in one UPDATE.
 * `surfaced` writes fired=0. Every time value is unix SECONDS, like every
 * other column of these tables.
 *
 * Free functions over a borrowed `sqlite3 *db` from hu_sqlite_memory_get_db();
 * domain callers never include sqlite3.h (sqlite-includer ratchet).
 */
#include "human/core/error.h"

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

/* Idempotent migration of an existing prospective_memories table: adds the
 * missing v2 columns, maps fired -> status on rows still 'pending', creates
 * idx_prospective_status and the fired -> status trigger. HU_ERR_NOT_FOUND
 * when the table does not exist (the sqlite engine creates it first). */
hu_error_t hu_prospective_repo_ensure_schema(sqlite3 *db);

#endif /* HU_ENABLE_SQLITE */
#endif /* HU_MEMORY_PROSPECTIVE_REPO_H */
