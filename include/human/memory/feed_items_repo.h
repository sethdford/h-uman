#ifndef HU_MEMORY_FEED_ITEMS_REPO_H
#define HU_MEMORY_FEED_ITEMS_REPO_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h" /* hu_memory_t */
#include <stddef.h>
#include <stdint.h>

/* A feed item: external content ingested into the inbox (a file, a feed
 * entry). Pure domain value object — no storage detail leaks here. */
typedef struct hu_feed_item {
    const char *source; /* file path / source identifier */
    size_t source_len;
    const char *content; /* ingested content (or filename) */
    size_t content_len;
    int64_t ingested_at; /* unix seconds */
} hu_feed_item_t;

struct hu_feed_items_repo_vtable;
typedef struct hu_feed_items_repo {
    void *ctx;
    const struct hu_feed_items_repo_vtable *vtable;
} hu_feed_items_repo_t;

typedef struct hu_feed_items_repo_vtable {
    /* Record a feed item. Idempotent: duplicate (source, content-prefix) are
     * ignored (matches the prior INSERT OR IGNORE behavior). */
    hu_error_t (*record)(void *ctx, const hu_feed_item_t *item);
    void (*deinit)(void *ctx);
} hu_feed_items_repo_vtable_t;

/* Factory: build a repo backed by `mem`. Returns a sqlite-backed repo when
 * `mem` is sqlite; HU_ERR_NOT_SUPPORTED for non-SQL backends. This is the
 * ONLY entry point domain code uses — it never sees sqlite3. Caller owns *out
 * and must call out->vtable->deinit. */
hu_error_t hu_feed_items_repo_create(hu_memory_t *mem, hu_allocator_t *alloc,
                                     hu_feed_items_repo_t *out);

#endif /* HU_MEMORY_FEED_ITEMS_REPO_H */
