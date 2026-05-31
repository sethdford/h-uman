typedef int hu_social_graph_unused_;

#ifdef HU_ENABLE_SQLITE

#include "human/context/social_graph.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/memory/graph.h"
#include "human/memory/social_graph_repo.h"
#include "human/persona.h"
#include <stdio.h>
#include <string.h>

/* DDD Phase 3: the contact_relationships SQL + raw sqlite3 handle now live behind
 * hu_social_graph_repo_t (src/memory/repos/social_graph_repo_sqlite.c). This file
 * keeps the graph-backed build_context and the pure build_directive, and depends
 * only on the backend-agnostic repo for persistence — no <sqlite3.h>. */

hu_error_t hu_social_graph_build_context(hu_allocator_t *alloc, hu_graph_t *graph,
                                         const char *query, size_t query_len,
                                         const char *contact_id, size_t contact_id_len,
                                         size_t max_hops, size_t max_chars, char **out,
                                         size_t *out_len) {
    if (!alloc || !graph || !query || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    if (contact_id && contact_id_len > 0)
        return hu_graph_build_contact_context(graph, alloc, query, query_len, contact_id,
                                              contact_id_len, max_hops, max_chars, out, out_len);
    return hu_graph_build_context(graph, alloc, "", 0, query, query_len, max_hops, max_chars, out,
                                  out_len);
}

hu_error_t hu_social_graph_store(hu_allocator_t *alloc, hu_memory_t *memory, const char *contact_id,
                                 size_t cid_len, const hu_relationship_t *rel) {
    if (!alloc || !memory || !contact_id || !rel)
        return HU_ERR_INVALID_ARGUMENT;

    hu_social_graph_repo_t repo;
    hu_error_t e = hu_social_graph_repo_create(memory, alloc, &repo);
    if (e != HU_OK)
        return e; /* HU_ERR_NOT_SUPPORTED for non-sqlite, as before */
    /* last_mentioned is 0: hu_relationship_t has no such field (unchanged). */
    e = repo.vtable->upsert(repo.ctx, contact_id, cid_len, rel->name, rel->role, 0, rel->notes);
    repo.vtable->deinit(repo.ctx);
    return e;
}

hu_error_t hu_social_graph_get(hu_allocator_t *alloc, hu_memory_t *memory, const char *contact_id,
                               size_t cid_len, hu_relationship_t **out, size_t *out_count) {
    if (!alloc || !memory || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    hu_social_graph_repo_t repo;
    hu_error_t e = hu_social_graph_repo_create(memory, alloc, &repo);
    if (e != HU_OK)
        return e;
    e = repo.vtable->get(repo.ctx, alloc, contact_id, cid_len, out, out_count);
    repo.vtable->deinit(repo.ctx);
    return e;
}

char *hu_social_graph_build_directive(hu_allocator_t *alloc, const char *contact_name,
                                      size_t name_len, const hu_relationship_t *rels, size_t count,
                                      size_t *out_len) {
    if (!alloc || !out_len || count == 0 || !rels)
        return NULL;
    *out_len = 0;

    size_t cap = 512;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return NULL;

    size_t pos = hu_buf_appendf(buf, cap, 0, "[SOCIAL: ");

    for (size_t i = 0; i < count; i++) {
        const char *name = rels[i].name[0] ? rels[i].name : "unnamed";
        const char *role = rels[i].role[0] ? rels[i].role : "";
        const char *notes = rels[i].notes[0] ? rels[i].notes : "";

        if (i > 0)
            pos = hu_buf_appendf(buf, cap, pos, " ");
        if (i == 0 && name_len > 0 && contact_name) {
            pos = hu_buf_appendf(buf, cap, pos, "%.*s's ", (int)name_len, contact_name);
        } else if (i > 0) {
            pos = hu_buf_appendf(buf, cap, pos, "Her ");
        }
        if (role[0])
            pos = hu_buf_appendf(buf, cap, pos, "%s ", role);
        pos = hu_buf_appendf(buf, cap, pos, "%s", name);
        if (notes[0])
            pos = hu_buf_appendf(buf, cap, pos, " — %s.", notes);
        else
            pos = hu_buf_appendf(buf, cap, pos, ".");
    }
    pos = hu_buf_appendf(buf, cap, pos, "]");

    char *result = hu_strndup(alloc, buf, pos);
    alloc->free(alloc->ctx, buf, cap);
    if (!result)
        return NULL;
    *out_len = pos;
    return result;
}

void hu_social_graph_free(hu_allocator_t *alloc, hu_relationship_t *rels, size_t count) {
    if (!alloc || !rels)
        return;
    alloc->free(alloc->ctx, rels, count * sizeof(hu_relationship_t));
}

#endif /* HU_ENABLE_SQLITE */
