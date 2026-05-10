typedef int hu_social_graph_unused_;

#ifdef HU_ENABLE_SQLITE

#include "human/context/social_graph.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/memory/graph.h"
#include "human/persona.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

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

hu_error_t hu_social_graph_store(hu_allocator_t *alloc, hu_memory_t *memory,
                                 const char *contact_id, size_t cid_len,
                                 const hu_relationship_t *rel) {
    if (!alloc || !memory || !contact_id || !rel)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "INSERT INTO contact_relationships(contact_id,person_name,role,"
                                "last_mentioned,notes) VALUES(?,?,?,?,?) "
                                "ON CONFLICT(contact_id,person_name) DO UPDATE SET "
                                "role=excluded.role, last_mentioned=excluded.last_mentioned, "
                                "notes=excluded.notes",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)cid_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, rel->name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, rel->role, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4, 0); /* last_mentioned - hu_relationship_t has no field */
    sqlite3_bind_text(stmt, 5, rel->notes, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

hu_error_t hu_social_graph_get(hu_allocator_t *alloc, hu_memory_t *memory,
                               const char *contact_id, size_t cid_len,
                               hu_relationship_t **out, size_t *out_count) {
    if (!alloc || !memory || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT person_name, role, notes "
                                "FROM contact_relationships WHERE contact_id=?",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)cid_len, SQLITE_STATIC);

    size_t cap = 16;
    size_t count = 0;
    hu_relationship_t *arr =
        (hu_relationship_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_relationship_t));
    if (!arr) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            size_t old_cap = cap;
            cap *= 2;
            hu_relationship_t *n =
                (hu_relationship_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_relationship_t));
            if (!n) {
                hu_social_graph_free(alloc, arr, count);
                sqlite3_finalize(stmt);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(n, arr, count * sizeof(hu_relationship_t));
            alloc->free(alloc->ctx, arr, old_cap * sizeof(hu_relationship_t));
            arr = n;
        }

        hu_relationship_t *r = &arr[count];
        memset(r, 0, sizeof(*r));

        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        if (name) {
            size_t len = (size_t)sqlite3_column_bytes(stmt, 0);
            if (len > 63)
                len = 63;
            snprintf(r->name, sizeof(r->name), "%.*s", (int)len, name);
        }
        const char *role = (const char *)sqlite3_column_text(stmt, 1);
        if (role) {
            size_t len = (size_t)sqlite3_column_bytes(stmt, 1);
            if (len > 31)
                len = 31;
            snprintf(r->role, sizeof(r->role), "%.*s", (int)len, role);
        }
        const char *notes = (const char *)sqlite3_column_text(stmt, 2);
        if (notes) {
            size_t len = (size_t)sqlite3_column_bytes(stmt, 2);
            if (len > 255)
                len = 255;
            snprintf(r->notes, sizeof(r->notes), "%.*s", (int)len, notes);
        }
        count++;
    }
    sqlite3_finalize(stmt);

    if (count == 0 && arr) {
        alloc->free(alloc->ctx, arr, cap * sizeof(hu_relationship_t));
        arr = NULL;
    }
    *out = arr;
    *out_count = count;
    return HU_OK;
}

char *hu_social_graph_build_directive(hu_allocator_t *alloc,
                                      const char *contact_name, size_t name_len,
                                      const hu_relationship_t *rels, size_t count,
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
