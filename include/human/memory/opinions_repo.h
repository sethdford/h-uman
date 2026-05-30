#ifndef HU_MEMORY_OPINIONS_REPO_H
#define HU_MEMORY_OPINIONS_REPO_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"          /* hu_memory_t */
#include "human/memory/opinions.h" /* hu_opinion_t (domain value object) */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* DDD Phase 3: the opinions aggregate's repository interface. SQL + the raw
 * sqlite3 handle live ONLY in src/memory/repos/opinions_repo_sqlite.c; domain
 * code (src/memory/opinions.c) depends on this vtable, never on <sqlite3.h>.
 * The opinions subsystem is itself HU_ENABLE_SQLITE-gated (see opinions.h), so
 * this interface — which reuses the gated hu_opinion_t — is gated to match. */
#ifdef HU_ENABLE_SQLITE

struct hu_opinions_repo_vtable;
typedef struct hu_opinions_repo {
    void *ctx;
    const struct hu_opinions_repo_vtable *vtable;
} hu_opinions_repo_t;

typedef struct hu_opinions_repo_vtable {
    /* Find the active (non-superseded) opinion on `topic`. On found=true,
     * *out_id is its id and *out_position is a newly-allocated copy of its
     * position text (caller frees via `alloc`/hu_str_free); on found=false,
     * *out_position is NULL. */
    hu_error_t (*find_active)(void *ctx, hu_allocator_t *alloc, const char *topic, size_t topic_len,
                              bool *found, int64_t *out_id, char **out_position,
                              size_t *out_position_len);
    /* Update an existing opinion's last_expressed timestamp + confidence. */
    hu_error_t (*update_last_expressed)(void *ctx, int64_t id, int64_t last_expressed,
                                        float confidence);
    /* Insert a brand-new opinion (superseded_by NULL). *out_id gets its rowid. */
    hu_error_t (*insert)(void *ctx, const char *topic, size_t topic_len, const char *position,
                         size_t position_len, float confidence, int64_t first_expressed,
                         int64_t last_expressed, int64_t *out_id);
    /* Atomically (one transaction) insert a new opinion AND mark old_id as
     * superseded by it. *out_new_id gets the new rowid. */
    hu_error_t (*insert_superseding)(void *ctx, const char *topic, size_t topic_len,
                                     const char *position, size_t position_len, float confidence,
                                     int64_t now_ts, int64_t old_id, int64_t *out_new_id);
    /* Query opinions on `topic`. superseded_only selects superseded vs active.
     * Allocates *out (array of hu_opinion_t) via `alloc`; caller frees with
     * hu_opinions_free. *out_count is the row count (0 → *out NULL). */
    hu_error_t (*query)(void *ctx, hu_allocator_t *alloc, const char *topic, size_t topic_len,
                        bool superseded_only, hu_opinion_t **out, size_t *out_count);
    void (*deinit)(void *ctx);
} hu_opinions_repo_vtable_t;

/* Factory: build a repo backed by `mem`. sqlite-backed when `mem` is sqlite;
 * HU_ERR_NOT_SUPPORTED for non-SQL backends. The ONLY entry point domain code
 * uses — it never sees sqlite3. Caller owns *out, must call out->vtable->deinit. */
hu_error_t hu_opinions_repo_create(hu_memory_t *mem, hu_allocator_t *alloc,
                                   hu_opinions_repo_t *out);

#endif /* HU_ENABLE_SQLITE */
#endif /* HU_MEMORY_OPINIONS_REPO_H */
