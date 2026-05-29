#ifndef HU_MEMORY_MEMORIES_REPO_H
#define HU_MEMORY_MEMORIES_REPO_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h" /* hu_memory_t */
#include <stddef.h>

/* Repository over the core `memories` table for tier/category operations.
 * The table itself is owned by the sqlite engine (always present for a sqlite
 * backend); this repo only issues queries against it. Domain code depends on
 * hu_memories_repo_t, never on sqlite3. */

struct hu_memories_repo_vtable;
typedef struct hu_memories_repo {
    void *ctx;
    const struct hu_memories_repo_vtable *vtable;
} hu_memories_repo_t;

typedef struct hu_memories_repo_vtable {
    /* Promote up to `max_count` most-recently-updated rows from `from_category`
     * to `to_category` (max_count == 0 means "all"). Returns HU_OK on success,
     * HU_ERR_MEMORY_BACKEND on a backend failure (matches prior behavior). */
    hu_error_t (*promote_tier)(void *ctx, const char *from_category, size_t from_len,
                               const char *to_category, size_t to_len, size_t max_count);
    void (*deinit)(void *ctx);
} hu_memories_repo_vtable_t;

/* Factory: build a repo backed by `mem`. Returns a sqlite-backed repo when
 * `mem` is sqlite; HU_ERR_NOT_SUPPORTED for non-SQL backends. Caller owns *out
 * and must call out->vtable->deinit. */
hu_error_t hu_memories_repo_create(hu_memory_t *mem, hu_allocator_t *alloc,
                                   hu_memories_repo_t *out);

#endif /* HU_MEMORY_MEMORIES_REPO_H */
