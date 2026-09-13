#ifndef HU_MEMORY_FORGETTING_REPO_H
#define HU_MEMORY_FORGETTING_REPO_H

#include "human/core/error.h"
#include <stdint.h>

/* Repository over the `episodes` table for salience decay.
 *
 * This exists so src/memory/forgetting_curve.c — domain code implementing the
 * Ebbinghaus curve — holds no SQL and no <sqlite3.h>. The curve math is a pure
 * function of (salience, rate, age); persisting the result is a storage
 * concern, and the two were previously interleaved in one file behind an
 * #ifdef HU_ENABLE_SQLITE. Only the storage half lives here, where the sqlite
 * include is legal (.claude/rules/sqlite-includer-ratchet.md exempts
 * src/memory/repos/).
 *
 * A direct function rather than the vtable+factory shape used by
 * memories_repo/boundary_repo: all three existing callers already hold a raw
 * backend handle rather than an hu_memory_t, so a factory would force three
 * unrelated call sites to change without decoupling anything further. The
 * table itself is owned by the sqlite engine; this repo issues no DDL.
 *
 * `db` is the backend handle (a sqlite3* for the sqlite engine), passed opaque
 * so callers need no sqlite type. Returns HU_ERR_INVALID_ARGUMENT on a NULL
 * handle and HU_ERR_MEMORY_BACKEND on any backend failure — the exact contract
 * the inline implementation had, so callers and tests are unaffected. */
hu_error_t hu_forgetting_repo_apply_batch_decay(void *db, int64_t now_ts, double rate,
                                                double min_salience);

#endif /* HU_MEMORY_FORGETTING_REPO_H */
