#include "human/agent/graph_grounding.h"
#include "human/memory.h"
#include <stdlib.h>
#include <string.h>

hu_graph_grounding_mode_t hu_graph_grounding_mode(void) {
    const char *v = getenv("HU_GRAPH_GROUNDING");
    if (!v || !*v)
        return HU_GRAPH_GROUNDING_OFF;
    if (strcmp(v, "shadow") == 0)
        return HU_GRAPH_GROUNDING_SHADOW;
    if (strcmp(v, "on") == 0 || strcmp(v, "1") == 0)
        return HU_GRAPH_GROUNDING_ON;
    return HU_GRAPH_GROUNDING_OFF;
}

hu_error_t hu_graph_ground_load(hu_memory_loader_t *loader, const char *contact_id,
                                size_t contact_id_len, size_t max_chars, char **out,
                                size_t *out_len) {
    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;
    if (!loader || !out || !out_len || !contact_id || contact_id_len == 0)
        return HU_OK;
    if (max_chars == 0)
        max_chars = 600;
#ifdef HU_ENABLE_SQLITE
    sqlite3 *db = loader->memory ? hu_sqlite_memory_get_db(loader->memory) : NULL;
    if (!db)
        return HU_OK;
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT summary_text FROM community_summaries WHERE contact_id = ?1 "
                      "ORDER BY (entity_count + edge_count) DESC, generated_at DESC LIMIT 3";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_OK;
    sqlite3_bind_text(st, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);
    char *buf = loader->alloc->alloc(loader->alloc->ctx, max_chars + 1);
    if (!buf) {
        sqlite3_finalize(st);
        return HU_OK;
    }
    size_t pos = 0;
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *s = sqlite3_column_text(st, 0);
        if (!s)
            continue;
        size_t slen = strlen((const char *)s);
        const char *sep = (n == 0) ? "- " : "\n- ";
        size_t seplen = strlen(sep);
        if (pos + seplen + slen >= max_chars)
            break;
        memcpy(buf + pos, sep, seplen);
        pos += seplen;
        memcpy(buf + pos, s, slen);
        pos += slen;
        n++;
    }
    sqlite3_finalize(st);
    if (n == 0) {
        loader->alloc->free(loader->alloc->ctx, buf, max_chars + 1);
        return HU_OK;
    }
    buf[pos] = '\0';
    /* Return a buffer sized EXACTLY to the content so callers freeing
     * (*out_len + 1) match the allocation size (codebase free-size contract). */
    char *exact = loader->alloc->alloc(loader->alloc->ctx, pos + 1);
    if (!exact) {
        loader->alloc->free(loader->alloc->ctx, buf, max_chars + 1);
        return HU_OK; /* fail-open */
    }
    memcpy(exact, buf, pos);
    exact[pos] = '\0';
    loader->alloc->free(loader->alloc->ctx, buf, max_chars + 1);
    *out = exact;
    *out_len = pos;
#endif
    return HU_OK;
}
