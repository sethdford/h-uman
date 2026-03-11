#ifndef HU_MEMORY_EMOTIONAL_RESIDUE_H
#define HU_MEMORY_EMOTIONAL_RESIDUE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * F76 Emotional Residue — valence/intensity with decay
 * Table: emotional_residue (created by sqlite engine)
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_emotional_residue {
    int64_t id;
    int64_t episode_id;
    char contact_id[128];
    double valence;   /* -1.0 to 1.0 */
    double intensity; /* 0.0 to 1.0 */
    double decay_rate;
    int64_t created_at;
} hu_emotional_residue_t;

#ifdef HU_ENABLE_SQLITE

#include <sqlite3.h>

hu_error_t hu_emotional_residue_add(sqlite3 *db, int64_t episode_id, const char *contact_id,
                                    size_t cid_len, double valence, double intensity,
                                    double decay_rate, int64_t *out_id);

hu_error_t hu_emotional_residue_get_active(hu_allocator_t *alloc, sqlite3 *db,
                                           const char *contact_id, size_t cid_len, int64_t now_ts,
                                           hu_emotional_residue_t **out, size_t *out_count);

char *hu_emotional_residue_build_directive(hu_allocator_t *alloc,
                                           const hu_emotional_residue_t *residues, size_t count,
                                           size_t *out_len);

#endif /* HU_ENABLE_SQLITE */

#endif /* HU_MEMORY_EMOTIONAL_RESIDUE_H */
