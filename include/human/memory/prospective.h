#ifndef HU_MEMORY_PROSPECTIVE_H
#define HU_MEMORY_PROSPECTIVE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

#ifdef HU_ENABLE_SQLITE

#include <sqlite3.h>

/* ──────────────────────────────────────────────────────────────────────────
 * F75 Prospective Memory — store and trigger future actions
 * Table: prospective_memories (created by sqlite engine)
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_prospective_entry {
    int64_t id;
    char trigger_type[32];
    char trigger_value[256];
    char action[512];
    char contact_id[128];
    int64_t expires_at;
    int64_t created_at;
} hu_prospective_entry_t;

hu_error_t hu_prospective_store(sqlite3 *db, const char *trigger_type, size_t tt_len,
                                const char *trigger_value, size_t tv_len,
                                const char *action, size_t action_len,
                                const char *contact_id, size_t cid_len, int64_t expires_at,
                                int64_t *out_id);

hu_error_t hu_prospective_check_triggers(hu_allocator_t *alloc, sqlite3 *db,
                                        const char *trigger_type, const char *trigger_value,
                                        size_t tv_len, const char *contact_id, size_t cid_len,
                                        int64_t now_ts, hu_prospective_entry_t **out,
                                        size_t *out_count);

hu_error_t hu_prospective_mark_fired(sqlite3 *db, int64_t id);

#endif /* HU_ENABLE_SQLITE */

#endif /* HU_MEMORY_PROSPECTIVE_H */
