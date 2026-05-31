#ifndef HU_MEMORY_LIFE_CHAPTER_REPO_H
#define HU_MEMORY_LIFE_CHAPTER_REPO_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h" /* hu_memory_t */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* DDD Phase 3: the life_chapters aggregate's repository interface. SQL + the raw
 * sqlite3 handle live ONLY in src/memory/repos/life_chapter_repo_sqlite.c;
 * domain code (src/memory/life_chapters.c) depends on this vtable, never on
 * <sqlite3.h>. The repo exposes SQL PRIMITIVES; the key_threads JSON
 * (de)serialization and the directive builder stay in the domain layer.
 * life_chapters is HU_ENABLE_SQLITE-gated (see life_chapters.h), so this
 * interface is gated to match. */
#ifdef HU_ENABLE_SQLITE

/* Raw active-chapter row, exactly as stored (key_threads is the serialized TEXT
 * column — the domain parses it). Strings are NUL-terminated and allocated via
 * the allocator passed to get_active; the caller frees each non-NULL string
 * with hu_str_free. Any field may be NULL when its column was NULL/empty. */
typedef struct hu_life_chapter_row {
    char *theme;
    char *mood;
    int64_t started_at;
    char *key_threads_json;
} hu_life_chapter_row_t;

struct hu_life_chapter_repo_vtable;
typedef struct hu_life_chapter_repo {
    void *ctx;
    const struct hu_life_chapter_repo_vtable *vtable;
} hu_life_chapter_repo_t;

typedef struct hu_life_chapter_repo_vtable {
    /* Fetch the most-recently-started active chapter's raw row. On no active
     * chapter, *found=false and *out is left zeroed. On found, *out's strings
     * are allocated via `alloc` (caller frees the non-NULL ones). */
    hu_error_t (*get_active)(void *ctx, hu_allocator_t *alloc, bool *found,
                             hu_life_chapter_row_t *out);
    /* Atomically (one transaction): deactivate every chapter, then insert a new
     * active chapter. key_threads_json is the serialized TEXT (e.g. "[]"). theme
     * and mood may be empty strings; NULL is treated as empty. */
    hu_error_t (*store_active)(void *ctx, const char *theme, const char *mood, int64_t started_at,
                               const char *key_threads_json);
    void (*deinit)(void *ctx);
} hu_life_chapter_repo_vtable_t;

/* Factory: sqlite-backed repo from `mem`. HU_ERR_NOT_SUPPORTED for non-sqlite.
 * Caller owns *out and must call out->vtable->deinit. */
hu_error_t hu_life_chapter_repo_create(hu_memory_t *mem, hu_allocator_t *alloc,
                                       hu_life_chapter_repo_t *out);

#endif /* HU_ENABLE_SQLITE */
#endif /* HU_MEMORY_LIFE_CHAPTER_REPO_H */
