#ifndef HU_MEMORY_SOCIAL_GRAPH_REPO_H
#define HU_MEMORY_SOCIAL_GRAPH_REPO_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"  /* hu_memory_t */
#include "human/persona.h" /* hu_relationship_t */
#include <stddef.h>
#include <stdint.h>

/* DDD Phase 3: the contact_relationships aggregate's repository interface. SQL +
 * the raw sqlite3 handle live ONLY in
 * src/memory/repos/social_graph_repo_sqlite.c; domain code
 * (src/context/social_graph.c) depends on this vtable, never on <sqlite3.h>.
 * The build_context (graph) and build_directive (pure) helpers stay in the
 * domain layer. social_graph's persistence API is HU_ENABLE_SQLITE-gated, so
 * this interface is gated to match. */
#ifdef HU_ENABLE_SQLITE

struct hu_social_graph_repo_vtable;
typedef struct hu_social_graph_repo {
    void *ctx;
    const struct hu_social_graph_repo_vtable *vtable;
} hu_social_graph_repo_t;

typedef struct hu_social_graph_repo_vtable {
    /* Upsert one contact relationship (INSERT ... ON CONFLICT(contact_id,
     * person_name) DO UPDATE). */
    hu_error_t (*upsert)(void *ctx, const char *contact_id, size_t cid_len, const char *name,
                         const char *role, int64_t last_mentioned, const char *notes);
    /* All relationships for a contact. Allocates *out (hu_relationship_t array)
     * via `alloc`; caller frees with hu_social_graph_free. 0 rows -> *out NULL. */
    hu_error_t (*get)(void *ctx, hu_allocator_t *alloc, const char *contact_id, size_t cid_len,
                      hu_relationship_t **out, size_t *out_count);
    void (*deinit)(void *ctx);
} hu_social_graph_repo_vtable_t;

/* Factory: sqlite-backed repo from `mem`. HU_ERR_NOT_SUPPORTED for non-sqlite.
 * Caller owns *out and must call out->vtable->deinit. */
hu_error_t hu_social_graph_repo_create(hu_memory_t *mem, hu_allocator_t *alloc,
                                       hu_social_graph_repo_t *out);

#endif /* HU_ENABLE_SQLITE */
#endif /* HU_MEMORY_SOCIAL_GRAPH_REPO_H */
