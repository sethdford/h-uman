#ifndef HU_MEMORY_BOUNDARY_REPO_H
#define HU_MEMORY_BOUNDARY_REPO_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h" /* hu_memory_t */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A protective "boundary": this contact + topic is off-limits. Pure domain
 * value object — no storage detail leaks here. */
typedef struct hu_boundary {
    const char *contact_id;
    size_t contact_id_len;
    const char *topic;
    size_t topic_len;
    const char *type;
    size_t type_len; /* e.g. "hard", "soft" */
    const char *source;
    size_t source_len; /* provenance */
    int64_t created_at;
} hu_boundary_t;

struct hu_boundary_repo_vtable;
typedef struct hu_boundary_repo {
    void *ctx;
    const struct hu_boundary_repo_vtable *vtable;
} hu_boundary_repo_t;

typedef struct hu_boundary_repo_vtable {
    /* True if (contact, topic) is a recorded boundary. */
    hu_error_t (*is_boundary)(void *ctx, const char *contact_id, size_t contact_id_len,
                              const char *topic, size_t topic_len, bool *out);
    /* Record a boundary (idempotent). */
    hu_error_t (*add)(void *ctx, const hu_boundary_t *b);
    void (*deinit)(void *ctx);
} hu_boundary_repo_vtable_t;

/* Factory: build a repo backed by `mem`. Returns a sqlite-backed repo when
 * `mem` is sqlite; HU_ERR_NOT_SUPPORTED for non-SQL backends (until a native
 * impl exists). This is the ONLY entry point domain code uses — it never sees
 * sqlite3. Caller owns *out and must call out->vtable->deinit. */
hu_error_t hu_boundary_repo_create(hu_memory_t *mem, hu_allocator_t *alloc,
                                   hu_boundary_repo_t *out);

#endif /* HU_MEMORY_BOUNDARY_REPO_H */
