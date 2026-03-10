#include "human/memory/graph.h"
#include "human/core/string.h"
#include "human/memory/consolidation.h"
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HU_ENABLE_SQLITE
#include "human/memory/sql_common.h"
#include <sqlite3.h>

#define HU_SQLITE_BUSY_TIMEOUT_MS 5000
#if !defined(_WIN32) && !defined(__CYGWIN__)
#include <sys/stat.h>
#include <unistd.h>
#endif
#endif

struct hu_graph {
    hu_allocator_t *alloc;
#ifdef HU_ENABLE_SQLITE
    sqlite3 *db;
#endif
};

#ifdef HU_ENABLE_SQLITE

static int64_t now_ms(void) {
    return (int64_t)time(NULL) * 1000;
}

#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
static hu_error_t ensure_parent_dir(hu_allocator_t *alloc, const char *path, size_t path_len) {
#if !defined(_WIN32) && !defined(__CYGWIN__)
    const char *last_slash = NULL;
    for (size_t i = 0; i < path_len; i++) {
        if (path[i] == '/')
            last_slash = &path[i];
    }
    if (last_slash && last_slash > path) {
        size_t dir_len = (size_t)(last_slash - path);
        char *dir = (char *)alloc->alloc(alloc->ctx, dir_len + 1);
        if (!dir)
            return HU_ERR_OUT_OF_MEMORY;
        memcpy(dir, path, dir_len);
        dir[dir_len] = '\0';
        int rc = mkdir(dir, 0755);
        alloc->free(alloc->ctx, dir, dir_len + 1);
        if (rc != 0 && errno != EEXIST)
            return HU_ERR_IO;
    }
#endif
    (void)alloc;
    (void)path;
    (void)path_len;
    return HU_OK;
}
#endif /* !HU_IS_TEST */

static const char *const SCHEMA[] = {
    "CREATE TABLE IF NOT EXISTS entities ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "name TEXT NOT NULL UNIQUE,"
    "type INTEGER NOT NULL DEFAULT 6,"
    "first_seen INTEGER NOT NULL,"
    "last_seen INTEGER NOT NULL,"
    "mention_count INTEGER NOT NULL DEFAULT 1,"
    "metadata_json TEXT)",
    "CREATE TABLE IF NOT EXISTS relations ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "source_id INTEGER NOT NULL REFERENCES entities(id),"
    "target_id INTEGER NOT NULL REFERENCES entities(id),"
    "relation_type INTEGER NOT NULL,"
    "weight REAL NOT NULL DEFAULT 1.0,"
    "first_seen INTEGER NOT NULL,"
    "last_seen INTEGER NOT NULL,"
    "context TEXT,"
    "UNIQUE(source_id, target_id, relation_type))",
    "CREATE INDEX IF NOT EXISTS idx_relations_source ON relations(source_id)",
    "CREATE INDEX IF NOT EXISTS idx_relations_target ON relations(target_id)",
    "CREATE INDEX IF NOT EXISTS idx_entities_name ON entities(name)",
    NULL,
};

#endif /* HU_ENABLE_SQLITE */

hu_error_t hu_graph_open(hu_allocator_t *alloc, const char *db_path, size_t db_path_len,
                         hu_graph_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

#ifdef HU_ENABLE_SQLITE
    hu_graph_t *g = (hu_graph_t *)alloc->alloc(alloc->ctx, sizeof(hu_graph_t));
    if (!g)
        return HU_ERR_OUT_OF_MEMORY;
    memset(g, 0, sizeof(*g));
    g->alloc = alloc;

    const char *path_to_use;

#if defined(HU_IS_TEST) && HU_IS_TEST == 1
    (void)db_path;
    (void)db_path_len;
    path_to_use = ":memory:";
#else
    char path_buf[1024];
    if (ensure_parent_dir(alloc, db_path, db_path_len) != HU_OK) {
        alloc->free(alloc->ctx, g, sizeof(hu_graph_t));
        return HU_ERR_IO;
    }
    if (db_path_len >= sizeof(path_buf)) {
        alloc->free(alloc->ctx, g, sizeof(hu_graph_t));
        return HU_ERR_INVALID_ARGUMENT;
    }
    memcpy(path_buf, db_path, db_path_len);
    path_buf[db_path_len] = '\0';
    path_to_use = path_buf;
#endif

    int rc = sqlite3_open(path_to_use, &g->db);
    if (rc != SQLITE_OK) {
        if (g->db)
            sqlite3_close(g->db);
        alloc->free(alloc->ctx, g, sizeof(hu_graph_t));
        return HU_ERR_IO;
    }
    sqlite3_exec(g->db, HU_SQL_PRAGMA_INIT, NULL, NULL, NULL);
    sqlite3_busy_timeout(g->db, HU_SQLITE_BUSY_TIMEOUT_MS);

    for (size_t i = 0; SCHEMA[i] != NULL; i++) {
        char *errmsg = NULL;
        rc = sqlite3_exec(g->db, SCHEMA[i], NULL, NULL, &errmsg);
        if (rc != SQLITE_OK) {
            if (errmsg)
                sqlite3_free(errmsg);
            sqlite3_close(g->db);
            alloc->free(alloc->ctx, g, sizeof(hu_graph_t));
            return HU_ERR_IO;
        }
    }

    *out = g;
    return HU_OK;
#else
    (void)db_path;
    (void)db_path_len;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

void hu_graph_close(hu_graph_t *g, hu_allocator_t *alloc) {
    if (!g || !alloc)
        return;
#ifdef HU_ENABLE_SQLITE
    if (g->db) {
        sqlite3_close(g->db);
        g->db = NULL;
    }
#endif
    alloc->free(alloc->ctx, g, sizeof(hu_graph_t));
}

#ifdef HU_ENABLE_SQLITE

hu_error_t hu_graph_upsert_entity(hu_graph_t *g, const char *name, size_t name_len,
                                  hu_entity_type_t type, const char *metadata_json,
                                  int64_t *out_id) {
    if (!g || !g->db || !name || name_len == 0 || !out_id)
        return HU_ERR_INVALID_ARGUMENT;

    int rc = sqlite3_exec(g->db, "BEGIN", NULL, NULL, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;

    int64_t ts = now_ms();
    sqlite3_stmt *ins = NULL;
    const char *ins_sql =
        "INSERT INTO entities (name, type, first_seen, last_seen, mention_count, metadata_json) "
        "VALUES (?, ?, ?, ?, 1, ?)";
    rc = sqlite3_prepare_v2(g->db, ins_sql, -1, &ins, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_exec(g->db, "ROLLBACK", NULL, NULL, NULL);
        return HU_ERR_IO;
    }

    sqlite3_bind_text(ins, 1, name, (int)name_len, SQLITE_STATIC);
    sqlite3_bind_int(ins, 2, (int)type);
    sqlite3_bind_int64(ins, 3, ts);
    sqlite3_bind_int64(ins, 4, ts);
    if (metadata_json)
        sqlite3_bind_text(ins, 5, metadata_json, (int)strlen(metadata_json), SQLITE_STATIC);
    else
        sqlite3_bind_null(ins, 5);

    rc = sqlite3_step(ins);
    sqlite3_finalize(ins);
    if (rc == SQLITE_DONE) {
        *out_id = sqlite3_last_insert_rowid(g->db);
        sqlite3_exec(g->db, "COMMIT", NULL, NULL, NULL);
        return HU_OK;
    }
    if (rc != SQLITE_CONSTRAINT) {
        sqlite3_exec(g->db, "ROLLBACK", NULL, NULL, NULL);
        return HU_ERR_IO;
    }

    sqlite3_stmt *upd = NULL;
    const char *upd_sql =
        "UPDATE entities SET last_seen = ?, mention_count = mention_count + 1 WHERE name = ?";
    rc = sqlite3_prepare_v2(g->db, upd_sql, -1, &upd, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_exec(g->db, "ROLLBACK", NULL, NULL, NULL);
        return HU_ERR_IO;
    }
    sqlite3_bind_int64(upd, 1, ts);
    sqlite3_bind_text(upd, 2, name, (int)name_len, SQLITE_STATIC);
    rc = sqlite3_step(upd);
    sqlite3_finalize(upd);
    if (rc != SQLITE_DONE) {
        sqlite3_exec(g->db, "ROLLBACK", NULL, NULL, NULL);
        return HU_ERR_IO;
    }

    sqlite3_stmt *sel = NULL;
    const char *sel_sql = "SELECT id FROM entities WHERE name = ?";
    rc = sqlite3_prepare_v2(g->db, sel_sql, -1, &sel, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_exec(g->db, "ROLLBACK", NULL, NULL, NULL);
        return HU_ERR_IO;
    }
    sqlite3_bind_text(sel, 1, name, (int)name_len, SQLITE_STATIC);
    rc = sqlite3_step(sel);
    int got_row = (rc == SQLITE_ROW);
    if (got_row)
        *out_id = sqlite3_column_int64(sel, 0);
    sqlite3_finalize(sel);
    rc = sqlite3_exec(g->db, "COMMIT", NULL, NULL, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;
    return got_row ? HU_OK : HU_ERR_NOT_FOUND;
}

#else

hu_error_t hu_graph_upsert_entity(hu_graph_t *g, const char *name, size_t name_len,
                                  hu_entity_type_t type, const char *metadata_json,
                                  int64_t *out_id) {
    (void)g;
    (void)name;
    (void)name_len;
    (void)type;
    (void)metadata_json;
    (void)out_id;
    return HU_ERR_NOT_SUPPORTED;
}

#endif

#ifdef HU_ENABLE_SQLITE

hu_error_t hu_graph_find_entity(hu_graph_t *g, const char *name, size_t name_len,
                                hu_graph_entity_t *out) {
    if (!g || !g->db || !name || !out)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    const char *sql = "SELECT id, name, type, first_seen, last_seen, mention_count, metadata_json "
                      "FROM entities WHERE name = ?";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_text(stmt, 1, name, (int)name_len, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return HU_ERR_NOT_FOUND;
    }

    out->id = sqlite3_column_int64(stmt, 0);
    const char *n = (const char *)sqlite3_column_text(stmt, 1);
    size_t n_len = n ? (size_t)sqlite3_column_bytes(stmt, 1) : 0;
    out->type = (hu_entity_type_t)sqlite3_column_int(stmt, 2);
    out->first_seen = sqlite3_column_int64(stmt, 3);
    out->last_seen = sqlite3_column_int64(stmt, 4);
    out->mention_count = sqlite3_column_int(stmt, 5);
    const char *meta = (const char *)sqlite3_column_text(stmt, 6);

    if (n && n_len > 0 && g->alloc) {
        out->name = hu_strndup(g->alloc, n, n_len);
        out->name_len = n_len;
    }
    if (meta && g->alloc) {
        size_t meta_len = (size_t)sqlite3_column_bytes(stmt, 6);
        out->metadata_json = hu_strndup(g->alloc, meta, meta_len);
    }
    sqlite3_finalize(stmt);
    return HU_OK;
}

#else

hu_error_t hu_graph_find_entity(hu_graph_t *g, const char *name, size_t name_len,
                                hu_graph_entity_t *out) {
    (void)g;
    (void)name;
    (void)name_len;
    (void)out;
    return HU_ERR_NOT_SUPPORTED;
}

#endif

#ifdef HU_ENABLE_SQLITE

hu_error_t hu_graph_upsert_relation(hu_graph_t *g, int64_t source_id, int64_t target_id,
                                    hu_relation_type_t type, float weight, const char *context,
                                    size_t context_len) {
    if (!g || !g->db)
        return HU_ERR_INVALID_ARGUMENT;

    int64_t ts = now_ms();
    const char *sql = "INSERT INTO relations (source_id, target_id, relation_type, weight, "
                      "first_seen, last_seen, "
                      "context) VALUES (?, ?, ?, ?, ?, ?, ?) "
                      "ON CONFLICT(source_id, target_id, relation_type) DO UPDATE SET "
                      "weight = (weight + excluded.weight) / 2.0, last_seen = excluded.last_seen, "
                      "context = excluded.context";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_int64(stmt, 1, source_id);
    sqlite3_bind_int64(stmt, 2, target_id);
    sqlite3_bind_int(stmt, 3, (int)type);
    sqlite3_bind_double(stmt, 4, (double)weight);
    sqlite3_bind_int64(stmt, 5, ts);
    sqlite3_bind_int64(stmt, 6, ts);
    if (context && context_len > 0)
        sqlite3_bind_text(stmt, 7, context, (int)context_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 7);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_IO;
}

#else

hu_error_t hu_graph_upsert_relation(hu_graph_t *g, int64_t source_id, int64_t target_id,
                                    hu_relation_type_t type, float weight, const char *context,
                                    size_t context_len) {
    (void)g;
    (void)source_id;
    (void)target_id;
    (void)type;
    (void)weight;
    (void)context;
    (void)context_len;
    return HU_ERR_NOT_SUPPORTED;
}

#endif

#ifdef HU_ENABLE_SQLITE

#define HU_GRAPH_MAX_NEIGHBORS 256

hu_error_t hu_graph_neighbors(hu_graph_t *g, hu_allocator_t *alloc, int64_t entity_id,
                              size_t max_hops, size_t max_results, hu_graph_entity_t **out_entities,
                              hu_graph_relation_t **out_relations, size_t *out_count) {
    if (!g || !g->db || !alloc || !out_entities || !out_relations || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

    *out_entities = NULL;
    *out_relations = NULL;
    *out_count = 0;

    int64_t frontier[HU_GRAPH_MAX_NEIGHBORS];
    size_t frontier_count = 1;
    frontier[0] = entity_id;

    int64_t visited[HU_GRAPH_MAX_NEIGHBORS];
    size_t visited_count = 1;
    visited[0] = entity_id;

    hu_graph_entity_t *entities = NULL;
    hu_graph_relation_t *relations = NULL;
    size_t entity_cap = 0;
    size_t entity_count = 0;
    size_t relation_cap = 0;
    size_t relation_count = 0;
    sqlite3_stmt *stmt = NULL;

    const char *neighbor_sql =
        "SELECT e.id, e.name, e.type, e.first_seen, e.last_seen, e.mention_count, e.metadata_json, "
        "r.id, r.source_id, r.target_id, r.relation_type, r.weight, r.first_seen, r.last_seen, "
        "r.context "
        "FROM entities e "
        "JOIN relations r ON (r.target_id = e.id AND r.source_id = ?) OR (r.source_id = e.id AND "
        "r.target_id = ?) "
        "WHERE e.id != ?";

    for (size_t hop = 0; hop < max_hops && frontier_count > 0 && entity_count < max_results;
         hop++) {
        int64_t next_frontier[HU_GRAPH_MAX_NEIGHBORS];
        size_t next_count = 0;

        for (size_t f = 0; f < frontier_count && entity_count < max_results; f++) {
            int64_t cur = frontier[f];
            stmt = NULL;
            int rc = sqlite3_prepare_v2(g->db, neighbor_sql, -1, &stmt, NULL);
            if (rc != SQLITE_OK)
                goto fail;

            sqlite3_bind_int64(stmt, 1, cur);
            sqlite3_bind_int64(stmt, 2, cur);
            sqlite3_bind_int64(stmt, 3, cur);

            while (sqlite3_step(stmt) == SQLITE_ROW && entity_count < max_results) {
                int64_t eid = sqlite3_column_int64(stmt, 0);
                bool already_visited = false;
                for (size_t v = 0; v < visited_count; v++) {
                    if (visited[v] == eid) {
                        already_visited = true;
                        break;
                    }
                }
                if (already_visited)
                    continue;

                if (visited_count < HU_GRAPH_MAX_NEIGHBORS)
                    visited[visited_count++] = eid;
                if (next_count < HU_GRAPH_MAX_NEIGHBORS)
                    next_frontier[next_count++] = eid;

                if (entity_count >= entity_cap) {
                    size_t new_cap = entity_cap == 0 ? 16 : entity_cap * 2;
                    if (new_cap > max_results)
                        new_cap = max_results;
                    hu_graph_entity_t *n = (hu_graph_entity_t *)alloc->realloc(
                        alloc->ctx, entities, entity_cap * sizeof(hu_graph_entity_t),
                        new_cap * sizeof(hu_graph_entity_t));
                    if (!n)
                        goto fail;
                    entities = n;
                    entity_cap = new_cap;
                }
                hu_graph_entity_t *ent = &entities[entity_count];
                memset(ent, 0, sizeof(*ent));
                ent->id = eid;
                const char *n = (const char *)sqlite3_column_text(stmt, 1);
                size_t n_len = n ? (size_t)sqlite3_column_bytes(stmt, 1) : 0;
                ent->type = (hu_entity_type_t)sqlite3_column_int(stmt, 2);
                ent->first_seen = sqlite3_column_int64(stmt, 3);
                ent->last_seen = sqlite3_column_int64(stmt, 4);
                ent->mention_count = sqlite3_column_int(stmt, 5);
                const char *meta = (const char *)sqlite3_column_text(stmt, 6);
                if (n && n_len > 0)
                    ent->name = hu_strndup(alloc, n, n_len);
                ent->name_len = n_len;
                if (meta)
                    ent->metadata_json =
                        hu_strndup(alloc, meta, (size_t)sqlite3_column_bytes(stmt, 6));

                if (relation_count >= relation_cap) {
                    size_t new_cap = relation_cap == 0 ? 16 : relation_cap * 2;
                    hu_graph_relation_t *nr = (hu_graph_relation_t *)alloc->realloc(
                        alloc->ctx, relations, relation_cap * sizeof(hu_graph_relation_t),
                        new_cap * sizeof(hu_graph_relation_t));
                    if (!nr)
                        goto fail;
                    relations = nr;
                    relation_cap = new_cap;
                }
                hu_graph_relation_t *rel = &relations[relation_count];
                memset(rel, 0, sizeof(*rel));
                rel->id = sqlite3_column_int64(stmt, 7);
                rel->source_id = sqlite3_column_int64(stmt, 8);
                rel->target_id = sqlite3_column_int64(stmt, 9);
                rel->type = (hu_relation_type_t)sqlite3_column_int(stmt, 10);
                rel->weight = (float)sqlite3_column_double(stmt, 11);
                rel->first_seen = sqlite3_column_int64(stmt, 12);
                rel->last_seen = sqlite3_column_int64(stmt, 13);
                const char *ctx = (const char *)sqlite3_column_text(stmt, 14);
                if (ctx) {
                    rel->context_len = (size_t)sqlite3_column_bytes(stmt, 14);
                    rel->context = hu_strndup(alloc, ctx, rel->context_len);
                }
                relation_count++;
                entity_count++;
            }
            sqlite3_finalize(stmt);
        }

        memcpy(frontier, next_frontier, next_count * sizeof(int64_t));
        frontier_count = next_count;
    }

    *out_entities = entities;
    *out_relations = relations;
    *out_count = entity_count;
    return HU_OK;

fail:
    if (stmt)
        sqlite3_finalize(stmt);
    if (entities) {
        for (size_t i = 0; i < entity_count; i++) {
            if (entities[i].name)
                alloc->free(alloc->ctx, entities[i].name, entities[i].name_len + 1);
            if (entities[i].metadata_json)
                alloc->free(alloc->ctx, entities[i].metadata_json,
                            strlen(entities[i].metadata_json) + 1);
        }
        alloc->free(alloc->ctx, entities, entity_cap * sizeof(hu_graph_entity_t));
    }
    if (relations) {
        for (size_t i = 0; i < relation_count; i++) {
            if (relations[i].context)
                alloc->free(alloc->ctx, relations[i].context, relations[i].context_len + 1);
        }
        alloc->free(alloc->ctx, relations, relation_cap * sizeof(hu_graph_relation_t));
    }
    return HU_ERR_IO;
}

#else

hu_error_t hu_graph_neighbors(hu_graph_t *g, hu_allocator_t *alloc, int64_t entity_id,
                              size_t max_hops, size_t max_results, hu_graph_entity_t **out_entities,
                              hu_graph_relation_t **out_relations, size_t *out_count) {
    (void)g;
    (void)alloc;
    (void)entity_id;
    (void)max_hops;
    (void)max_results;
    (void)out_entities;
    (void)out_relations;
    (void)out_count;
    return HU_ERR_NOT_SUPPORTED;
}

#endif

#ifdef HU_ENABLE_SQLITE

static bool is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

hu_error_t hu_graph_build_context(hu_graph_t *g, hu_allocator_t *alloc, const char *query,
                                  size_t query_len, size_t max_hops, size_t max_chars, char **out,
                                  size_t *out_len) {
    if (!g || !g->db || !alloc || !query || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_len = 0;

    int64_t seed_ids[32];
    size_t seed_count = 0;

    for (size_t i = 0; i < query_len && seed_count < 32; i++) {
        while (i < query_len && !is_word_char(query[i]))
            i++;
        if (i >= query_len)
            break;
        size_t start = i;
        while (i < query_len && is_word_char(query[i]))
            i++;
        size_t word_len = i - start;
        if (word_len < 2)
            continue;

        hu_graph_entity_t ent;
        if (hu_graph_find_entity(g, query + start, word_len, &ent) != HU_OK)
            continue;
        if (ent.name)
            g->alloc->free(g->alloc->ctx, ent.name, ent.name_len + 1);
        if (ent.metadata_json)
            g->alloc->free(g->alloc->ctx, ent.metadata_json, strlen(ent.metadata_json) + 1);

        bool dup = false;
        for (size_t s = 0; s < seed_count; s++) {
            if (seed_ids[s] == ent.id) {
                dup = true;
                break;
            }
        }
        if (!dup)
            seed_ids[seed_count++] = ent.id;
    }

    if (seed_count == 0) {
        *out = hu_strndup(alloc, "", 0);
        *out_len = 0;
        return HU_OK;
    }

    size_t total_len = 0;
    char *buf = (char *)alloc->alloc(alloc->ctx, max_chars + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    int n = snprintf(buf, max_chars + 1, "### Knowledge Graph (from your conversations)\n");
    if (n > 0)
        total_len = (size_t)n;

    for (size_t s = 0; s < seed_count && total_len < max_chars; s++) {
        hu_graph_entity_t *entities = NULL;
        hu_graph_relation_t *relations = NULL;
        size_t count = 0;
        if (hu_graph_neighbors(g, alloc, seed_ids[s], max_hops, 20, &entities, &relations,
                               &count) != HU_OK)
            continue;

        for (size_t i = 0; i < count && total_len < max_chars; i++) {
            const char *src_name = "?";
            size_t src_len = 1;
            const char *tgt_name = entities[i].name ? entities[i].name : "?";
            size_t tgt_len = entities[i].name_len;
            if (relations[i].source_id == seed_ids[s]) {
                src_name = "query";
                src_len = 5;
            } else {
                for (size_t j = 0; j < count; j++) {
                    if (entities[j].id == relations[i].source_id && entities[j].name) {
                        src_name = entities[j].name;
                        src_len = entities[j].name_len;
                        break;
                    }
                }
            }
            const char *rel_str = hu_relation_type_to_string(relations[i].type);
            int w =
                snprintf(buf + total_len, max_chars - total_len + 1, "- [%.*s] (%s) -> [%.*s]\n",
                         (int)src_len, src_name, rel_str, (int)tgt_len, tgt_name);
            if (w > 0 && total_len + (size_t)w <= max_chars)
                total_len += (size_t)w;
        }
        hu_graph_entities_free(alloc, entities, count);
        hu_graph_relations_free(alloc, relations, count);
    }

    buf[total_len] = '\0';
    *out = buf;
    *out_len = total_len;
    return HU_OK;
}

hu_error_t hu_graph_build_communities(hu_graph_t *g, hu_allocator_t *alloc, size_t max_communities,
                                      size_t max_chars, char **out, size_t *out_len) {
    if (!g || !g->db || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_len = 0;

    const char *top_sql = "SELECT e.id, e.name, e.type FROM entities e "
                          "JOIN relations r ON (r.source_id = e.id OR r.target_id = e.id) "
                          "GROUP BY e.id ORDER BY COUNT(r.id) DESC LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g->db, top_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;

    size_t limit = max_communities > 0 ? max_communities : 20;
    if (limit > 64)
        limit = 64;
    sqlite3_bind_int(stmt, 1, (int)limit);

    char *buf = (char *)alloc->alloc(alloc->ctx, max_chars + 1);
    if (!buf) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t total_len = 0;

    int n = snprintf(buf, max_chars + 1, "### Topic Clusters\n");
    if (n > 0)
        total_len = (size_t)n;

    /* Group entities by type as we collect neighbors */
    const char *type_labels[] = {"People", "Places",   "Organizations", "Events",
                                 "Topics", "Emotions", "Other"};
    char *type_names[7][32];
    size_t type_counts[7] = {0};
    memset(type_names, 0, sizeof(type_names));

    while (sqlite3_step(stmt) == SQLITE_ROW && total_len < max_chars) {
        int64_t eid = sqlite3_column_int64(stmt, 0);

        hu_graph_entity_t *neighbors = NULL;
        hu_graph_relation_t *relations = NULL;
        size_t ncount = 0;
        if (hu_graph_neighbors(g, alloc, eid, 1, 10, &neighbors, &relations, &ncount) != HU_OK)
            continue;

        for (size_t i = 0; i < ncount; i++) {
            if (!neighbors[i].name || neighbors[i].name_len == 0)
                continue;
            int ntype = (int)neighbors[i].type;
            if (ntype < 0 || ntype > 6)
                ntype = 6;
            if (type_counts[ntype] >= 32)
                continue;
            char *dup = hu_strndup(alloc, neighbors[i].name, neighbors[i].name_len);
            if (dup)
                type_names[ntype][type_counts[ntype]++] = dup;
        }
        hu_graph_entities_free(alloc, neighbors, ncount);
        hu_graph_relations_free(alloc, relations, ncount);
    }
    sqlite3_finalize(stmt);

    for (int t = 0; t < 7; t++) {
        if (type_counts[t] == 0)
            continue;

        if (total_len < max_chars) {
            const char *label = type_labels[t];
            int w = snprintf(buf + total_len, max_chars - total_len + 1, "- %s: [", label);
            if (w <= 0)
                goto free_remaining;
            total_len += (size_t)w;
        }

        for (size_t i = 0; i < type_counts[t]; i++) {
            char *nm = type_names[t][i];
            if (!nm)
                continue;
            size_t nlen = strlen(nm);
            if (total_len < max_chars) {
                if (i > 0) {
                    int c = snprintf(buf + total_len, max_chars - total_len + 1, ", ");
                    if (c > 0)
                        total_len += (size_t)c;
                }
                if (total_len + nlen + 2 <= max_chars) {
                    memcpy(buf + total_len, nm, nlen + 1);
                    total_len += nlen;
                }
            }
            alloc->free(alloc->ctx, nm, nlen + 1);
        }

        if (total_len < max_chars) {
            int c = snprintf(buf + total_len, max_chars - total_len + 1, "]\n");
            if (c > 0)
                total_len += (size_t)c;
        }
    }
    goto done;

free_remaining:
    for (int t = 0; t < 7; t++) {
        for (size_t i = 0; i < type_counts[t]; i++) {
            if (type_names[t][i]) {
                size_t nlen = strlen(type_names[t][i]);
                alloc->free(alloc->ctx, type_names[t][i], nlen + 1);
            }
        }
    }

done:

    buf[total_len] = '\0';
    *out = buf;
    *out_len = total_len;
    return HU_OK;
}

hu_error_t hu_graph_build_contact_context(hu_graph_t *g, hu_allocator_t *alloc, const char *query,
                                          size_t query_len, const char *contact_id,
                                          size_t contact_id_len, size_t max_hops, size_t max_chars,
                                          char **out, size_t *out_len) {
    if (!g || !alloc || !query || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    hu_error_t err =
        hu_graph_build_context(g, alloc, query, query_len, max_hops, max_chars, out, out_len);
    if (err != HU_OK || !*out || *out_len == 0)
        return err;

    if (!contact_id || contact_id_len == 0)
        return HU_OK;

    /* Prepend contact-aware note for cross-contact knowledge synthesis */
    size_t prefix_max = 64 + contact_id_len;
    if (prefix_max > max_chars)
        prefix_max = max_chars;
    char *prefix = (char *)alloc->alloc(alloc->ctx, prefix_max + 1);
    if (!prefix)
        return HU_ERR_OUT_OF_MEMORY;

    int n = snprintf(prefix, prefix_max + 1,
                     "Knowledge relevant to this contact (drawn from all conversations):\n");
    if (n <= 0 || (size_t)n >= prefix_max) {
        alloc->free(alloc->ctx, prefix, prefix_max + 1);
        return HU_OK;
    }
    size_t prefix_len = (size_t)n;

    size_t total = prefix_len + *out_len + 1;
    char *merged = (char *)alloc->alloc(alloc->ctx, total);
    if (!merged) {
        alloc->free(alloc->ctx, prefix, prefix_max + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(merged, prefix, prefix_len);
    memcpy(merged + prefix_len, *out, *out_len);
    merged[total - 1] = '\0';
    alloc->free(alloc->ctx, prefix, prefix_max + 1);
    alloc->free(alloc->ctx, *out, *out_len + 1);
    *out = merged;
    *out_len = total - 1;
    return HU_OK;
}

#else

hu_error_t hu_graph_build_context(hu_graph_t *g, hu_allocator_t *alloc, const char *query,
                                  size_t query_len, size_t max_hops, size_t max_chars, char **out,
                                  size_t *out_len) {
    (void)g;
    (void)alloc;
    (void)query;
    (void)query_len;
    (void)max_hops;
    (void)max_chars;
    (void)out;
    (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_graph_build_contact_context(hu_graph_t *g, hu_allocator_t *alloc, const char *query,
                                          size_t query_len, const char *contact_id,
                                          size_t contact_id_len, size_t max_hops, size_t max_chars,
                                          char **out, size_t *out_len) {
    (void)g;
    (void)alloc;
    (void)query;
    (void)query_len;
    (void)contact_id;
    (void)contact_id_len;
    (void)max_hops;
    (void)max_chars;
    (void)out;
    (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_graph_build_communities(hu_graph_t *g, hu_allocator_t *alloc, size_t max_communities,
                                      size_t max_chars, char **out, size_t *out_len) {
    (void)g;
    (void)alloc;
    (void)max_communities;
    (void)max_chars;
    (void)out;
    (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
}

#endif

#ifdef HU_ENABLE_SQLITE

hu_error_t hu_graph_list_entities(hu_graph_t *g, hu_allocator_t *alloc, size_t limit,
                                  hu_graph_entity_t **out, size_t *out_count) {
    if (!g || !g->db || !alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    if (limit == 0)
        limit = 100;

    const char *sql = "SELECT id, name, type, first_seen, last_seen, mention_count "
                      "FROM entities ORDER BY mention_count DESC LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_int64(stmt, 1, (int64_t)limit);

    size_t cap = limit < 64 ? limit : 64;
    hu_graph_entity_t *arr = alloc->alloc(alloc->ctx, cap * sizeof(hu_graph_entity_t));
    if (!arr) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            size_t new_cap = cap * 2;
            hu_graph_entity_t *tmp = alloc->alloc(alloc->ctx, new_cap * sizeof(hu_graph_entity_t));
            if (!tmp)
                break;
            memcpy(tmp, arr, count * sizeof(hu_graph_entity_t));
            alloc->free(alloc->ctx, arr, cap * sizeof(hu_graph_entity_t));
            arr = tmp;
            cap = new_cap;
        }
        hu_graph_entity_t *e = &arr[count];
        memset(e, 0, sizeof(*e));
        e->id = sqlite3_column_int64(stmt, 0);
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        e->name_len = name ? strlen(name) : 0;
        e->name = e->name_len ? hu_strndup(alloc, name, e->name_len) : NULL;
        e->type = (hu_entity_type_t)sqlite3_column_int(stmt, 2);
        e->first_seen = sqlite3_column_int64(stmt, 3);
        e->last_seen = sqlite3_column_int64(stmt, 4);
        e->mention_count = (int32_t)sqlite3_column_int(stmt, 5);
        e->metadata_json = NULL;
        count++;
    }
    sqlite3_finalize(stmt);
    *out = arr;
    *out_count = count;
    return HU_OK;
}

hu_error_t hu_graph_list_relations(hu_graph_t *g, hu_allocator_t *alloc, size_t limit,
                                   hu_graph_relation_t **out, size_t *out_count) {
    if (!g || !g->db || !alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    if (limit == 0)
        limit = 200;

    const char *sql = "SELECT r.id, r.source_id, r.target_id, r.relation_type, r.weight, "
                      "r.first_seen, r.last_seen, r.context "
                      "FROM relations r ORDER BY r.weight DESC LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_int64(stmt, 1, (int64_t)limit);

    size_t cap = limit < 64 ? limit : 64;
    hu_graph_relation_t *arr = alloc->alloc(alloc->ctx, cap * sizeof(hu_graph_relation_t));
    if (!arr) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            size_t new_cap = cap * 2;
            hu_graph_relation_t *tmp =
                alloc->alloc(alloc->ctx, new_cap * sizeof(hu_graph_relation_t));
            if (!tmp)
                break;
            memcpy(tmp, arr, count * sizeof(hu_graph_relation_t));
            alloc->free(alloc->ctx, arr, cap * sizeof(hu_graph_relation_t));
            arr = tmp;
            cap = new_cap;
        }
        hu_graph_relation_t *r = &arr[count];
        memset(r, 0, sizeof(*r));
        r->id = sqlite3_column_int64(stmt, 0);
        r->source_id = sqlite3_column_int64(stmt, 1);
        r->target_id = sqlite3_column_int64(stmt, 2);
        r->type = (hu_relation_type_t)sqlite3_column_int(stmt, 3);
        r->weight = (float)sqlite3_column_double(stmt, 4);
        r->first_seen = sqlite3_column_int64(stmt, 5);
        r->last_seen = sqlite3_column_int64(stmt, 6);
        const char *ctx = (const char *)sqlite3_column_text(stmt, 7);
        r->context_len = ctx ? strlen(ctx) : 0;
        r->context = r->context_len ? hu_strndup(alloc, ctx, r->context_len) : NULL;
        count++;
    }
    sqlite3_finalize(stmt);
    *out = arr;
    *out_count = count;
    return HU_OK;
}

#else

hu_error_t hu_graph_list_entities(hu_graph_t *g, hu_allocator_t *alloc, size_t limit,
                                  hu_graph_entity_t **out, size_t *out_count) {
    (void)g;
    (void)alloc;
    (void)limit;
    (void)out;
    (void)out_count;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_graph_list_relations(hu_graph_t *g, hu_allocator_t *alloc, size_t limit,
                                   hu_graph_relation_t **out, size_t *out_count) {
    (void)g;
    (void)alloc;
    (void)limit;
    (void)out;
    (void)out_count;
    return HU_ERR_NOT_SUPPORTED;
}

#endif

void hu_graph_entities_free(hu_allocator_t *alloc, hu_graph_entity_t *entities, size_t count) {
    if (!alloc || !entities)
        return;
    for (size_t i = 0; i < count; i++) {
        if (entities[i].name)
            alloc->free(alloc->ctx, entities[i].name, entities[i].name_len + 1);
        if (entities[i].metadata_json)
            alloc->free(alloc->ctx, entities[i].metadata_json,
                        strlen(entities[i].metadata_json) + 1);
    }
    alloc->free(alloc->ctx, entities, count * sizeof(hu_graph_entity_t));
}

void hu_graph_relations_free(hu_allocator_t *alloc, hu_graph_relation_t *relations, size_t count) {
    if (!alloc || !relations)
        return;
    for (size_t i = 0; i < count; i++) {
        if (relations[i].context)
            alloc->free(alloc->ctx, relations[i].context, relations[i].context_len + 1);
    }
    alloc->free(alloc->ctx, relations, count * sizeof(hu_graph_relation_t));
}

hu_entity_type_t hu_entity_type_from_string(const char *s, size_t len) {
    if (!s || len == 0)
        return HU_ENTITY_UNKNOWN;
    if (len == 6 && strncasecmp(s, "person", 6) == 0)
        return HU_ENTITY_PERSON;
    if (len == 5 && strncasecmp(s, "place", 5) == 0)
        return HU_ENTITY_PLACE;
    if (len == 12 && strncasecmp(s, "organization", 12) == 0)
        return HU_ENTITY_ORGANIZATION;
    if (len == 5 && strncasecmp(s, "event", 5) == 0)
        return HU_ENTITY_EVENT;
    if (len == 5 && strncasecmp(s, "topic", 5) == 0)
        return HU_ENTITY_TOPIC;
    if (len == 7 && strncasecmp(s, "emotion", 7) == 0)
        return HU_ENTITY_EMOTION;
    return HU_ENTITY_UNKNOWN;
}

const char *hu_entity_type_to_string(hu_entity_type_t t) {
    switch (t) {
    case HU_ENTITY_PERSON:
        return "person";
    case HU_ENTITY_PLACE:
        return "place";
    case HU_ENTITY_ORGANIZATION:
        return "organization";
    case HU_ENTITY_EVENT:
        return "event";
    case HU_ENTITY_TOPIC:
        return "topic";
    case HU_ENTITY_EMOTION:
        return "emotion";
    default:
        return "unknown";
    }
}

static int pred_match(const char *s, size_t len, const char *pattern) {
    size_t plen = strlen(pattern);
    if (len != plen)
        return 0;
    return strncasecmp(s, pattern, len) == 0;
}

hu_relation_type_t hu_relation_type_from_string(const char *s, size_t len) {
    if (!s || len == 0)
        return HU_REL_RELATED_TO;
    if (pred_match(s, len, "knows"))
        return HU_REL_KNOWS;
    if (pred_match(s, len, "family_of") || pred_match(s, len, "family"))
        return HU_REL_FAMILY_OF;
    if (pred_match(s, len, "works_at") || pred_match(s, len, "job"))
        return HU_REL_WORKS_AT;
    if (pred_match(s, len, "lives_in"))
        return HU_REL_LIVES_IN;
    if (pred_match(s, len, "interested_in") || pred_match(s, len, "likes") ||
        pred_match(s, len, "loves") || pred_match(s, len, "hates"))
        return HU_REL_INTERESTED_IN;
    if (pred_match(s, len, "discussed_with"))
        return HU_REL_DISCUSSED_WITH;
    if (pred_match(s, len, "feels_about"))
        return HU_REL_FEELS_ABOUT;
    if (pred_match(s, len, "promised_to"))
        return HU_REL_PROMISED_TO;
    if (pred_match(s, len, "shared_experience"))
        return HU_REL_SHARED_EXPERIENCE;
    if (pred_match(s, len, "is_a") || pred_match(s, len, "name"))
        return HU_REL_RELATED_TO;
    return HU_REL_RELATED_TO;
}

const char *hu_relation_type_to_string(hu_relation_type_t t) {
    switch (t) {
    case HU_REL_KNOWS:
        return "knows";
    case HU_REL_FAMILY_OF:
        return "family_of";
    case HU_REL_WORKS_AT:
        return "works_at";
    case HU_REL_LIVES_IN:
        return "lives_in";
    case HU_REL_INTERESTED_IN:
        return "interested_in";
    case HU_REL_DISCUSSED_WITH:
        return "discussed_with";
    case HU_REL_FEELS_ABOUT:
        return "feels_about";
    case HU_REL_PROMISED_TO:
        return "promised_to";
    case HU_REL_SHARED_EXPERIENCE:
        return "shared_experience";
    default:
        return "related_to";
    }
}

/* ── Phase 3a: Temporal events ──────────────────────────────────────── */

#ifdef HU_ENABLE_SQLITE
hu_error_t hu_graph_add_temporal_event(hu_graph_t *g, int64_t entity_id, const char *description,
                                       size_t desc_len, int64_t occurred_at, int64_t duration_sec) {
    if (!g || !g->db || !description || desc_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    const char *sql =
        "INSERT INTO temporal_events(entity_id, description, occurred_at, duration_sec)"
        " VALUES(?, ?, ?, ?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_int64(stmt, 1, entity_id);
    sqlite3_bind_text(stmt, 2, description, (int)desc_len, NULL);
    sqlite3_bind_int64(stmt, 3, occurred_at);
    sqlite3_bind_int64(stmt, 4, duration_sec);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_IO;
}

hu_error_t hu_graph_query_temporal(hu_graph_t *g, hu_allocator_t *alloc, int64_t from_ts,
                                   int64_t to_ts, size_t limit, char **out, size_t *out_len) {
    if (!g || !g->db || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (limit == 0)
        limit = 50;
    const char *sql = "SELECT e.name, t.description, t.occurred_at, t.duration_sec "
                      "FROM temporal_events t JOIN entities e ON e.id = t.entity_id "
                      "WHERE t.occurred_at BETWEEN ? AND ? ORDER BY t.occurred_at DESC LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_int64(stmt, 1, from_ts);
    sqlite3_bind_int64(stmt, 2, to_ts);
    sqlite3_bind_int(stmt, 3, (int)limit);
    size_t cap = 2048;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t pos = 0;
    int w = snprintf(buf, cap, "### Timeline\n");
    if (w > 0)
        pos = (size_t)w;
    while (sqlite3_step(stmt) == SQLITE_ROW && pos < cap - 128) {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        const char *desc = (const char *)sqlite3_column_text(stmt, 1);
        int64_t ts = sqlite3_column_int64(stmt, 2);
        if (!name || !desc)
            continue;
        w = snprintf(buf + pos, cap - pos, "- [%lld] %s: %s\n", (long long)ts, name, desc);
        if (w > 0)
            pos += (size_t)w;
    }
    sqlite3_finalize(stmt);
    buf[pos] = '\0';
    *out = buf;
    *out_len = pos;
    return HU_OK;
}

hu_error_t hu_graph_add_causal_link(hu_graph_t *g, int64_t action_entity_id,
                                    int64_t outcome_entity_id, const char *context,
                                    size_t context_len, float confidence) {
    if (!g || !g->db)
        return HU_ERR_INVALID_ARGUMENT;
    const char *sql =
        "INSERT OR REPLACE INTO causal_links(action_entity_id, outcome_entity_id, context,"
        " confidence, created_at) VALUES(?, ?, ?, ?, ?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_int64(stmt, 1, action_entity_id);
    sqlite3_bind_int64(stmt, 2, outcome_entity_id);
    if (context && context_len > 0)
        sqlite3_bind_text(stmt, 3, context, (int)context_len, NULL);
    else
        sqlite3_bind_null(stmt, 3);
    sqlite3_bind_double(stmt, 4, (double)confidence);
    sqlite3_bind_int64(stmt, 5, (int64_t)time(NULL));
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_IO;
}

hu_error_t hu_graph_query_causal(hu_graph_t *g, hu_allocator_t *alloc, int64_t entity_id,
                                 size_t max_results, char **out, size_t *out_len) {
    if (!g || !g->db || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (max_results == 0)
        max_results = 20;
    const char *sql = "SELECT a.name, o.name, c.context, c.confidence "
                      "FROM causal_links c "
                      "JOIN entities a ON a.id = c.action_entity_id "
                      "JOIN entities o ON o.id = c.outcome_entity_id "
                      "WHERE c.action_entity_id = ? OR c.outcome_entity_id = ? "
                      "ORDER BY c.confidence DESC LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_int64(stmt, 1, entity_id);
    sqlite3_bind_int64(stmt, 2, entity_id);
    sqlite3_bind_int(stmt, 3, (int)max_results);
    size_t cap = 2048;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t pos = 0;
    int w = snprintf(buf, cap, "### Cause-Effect\n");
    if (w > 0)
        pos = (size_t)w;
    while (sqlite3_step(stmt) == SQLITE_ROW && pos < cap - 128) {
        const char *action = (const char *)sqlite3_column_text(stmt, 0);
        const char *outcome = (const char *)sqlite3_column_text(stmt, 1);
        const char *ctx = (const char *)sqlite3_column_text(stmt, 2);
        double conf = sqlite3_column_double(stmt, 3);
        if (!action || !outcome)
            continue;
        w = snprintf(buf + pos, cap - pos, "- %s -> %s (%.0f%%", action, outcome, conf * 100);
        if (w > 0)
            pos += (size_t)w;
        if (ctx && pos < cap - 64) {
            w = snprintf(buf + pos, cap - pos, ", %s", ctx);
            if (w > 0)
                pos += (size_t)w;
        }
        if (pos < cap - 4) {
            w = snprintf(buf + pos, cap - pos, ")\n");
            if (w > 0)
                pos += (size_t)w;
        }
    }
    sqlite3_finalize(stmt);
    buf[pos] = '\0';
    *out = buf;
    *out_len = pos;
    return HU_OK;
}

#else

hu_error_t hu_graph_query_temporal(hu_graph_t *g, hu_allocator_t *alloc, int64_t from_ts,
                                   int64_t to_ts, size_t limit, char **out, size_t *out_len) {
    (void)g;
    (void)alloc;
    (void)from_ts;
    (void)to_ts;
    (void)limit;
    (void)out;
    (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_graph_query_causal(hu_graph_t *g, hu_allocator_t *alloc, int64_t entity_id,
                                 size_t max_results, char **out, size_t *out_len) {
    (void)g;
    (void)alloc;
    (void)entity_id;
    (void)max_results;
    (void)out;
    (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
}

#endif /* HU_ENABLE_SQLITE */

/* ── Phase 3b: Leiden-inspired hierarchical community detection ─────── */

#ifdef HU_ENABLE_SQLITE
hu_error_t hu_graph_leiden_communities(hu_graph_t *g, hu_allocator_t *alloc, size_t max_communities,
                                       size_t max_iterations, char **out, size_t *out_len) {
    if (!g || !g->db || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (max_communities == 0)
        max_communities = 20;
    if (max_iterations == 0)
        max_iterations = 10;

    const char *count_sql = "SELECT COUNT(*) FROM entities";
    sqlite3_stmt *cnt_stmt = NULL;
    if (sqlite3_prepare_v2(g->db, count_sql, -1, &cnt_stmt, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    size_t entity_count = 0;
    if (sqlite3_step(cnt_stmt) == SQLITE_ROW)
        entity_count = (size_t)sqlite3_column_int64(cnt_stmt, 0);
    sqlite3_finalize(cnt_stmt);

    if (entity_count == 0)
        return HU_OK;
    if (entity_count > 4096)
        entity_count = 4096;

    int64_t *ids = (int64_t *)alloc->alloc(alloc->ctx, entity_count * sizeof(int64_t));
    int32_t *labels = (int32_t *)alloc->alloc(alloc->ctx, entity_count * sizeof(int32_t));
    if (!ids || !labels) {
        if (ids)
            alloc->free(alloc->ctx, ids, entity_count * sizeof(int64_t));
        if (labels)
            alloc->free(alloc->ctx, labels, entity_count * sizeof(int32_t));
        return HU_ERR_OUT_OF_MEMORY;
    }

    const char *id_sql = "SELECT id FROM entities ORDER BY mention_count DESC LIMIT ?";
    sqlite3_stmt *id_stmt = NULL;
    if (sqlite3_prepare_v2(g->db, id_sql, -1, &id_stmt, NULL) != SQLITE_OK) {
        alloc->free(alloc->ctx, ids, entity_count * sizeof(int64_t));
        alloc->free(alloc->ctx, labels, entity_count * sizeof(int32_t));
        return HU_ERR_IO;
    }
    sqlite3_bind_int(id_stmt, 1, (int)entity_count);
    size_t n = 0;
    while (sqlite3_step(id_stmt) == SQLITE_ROW && n < entity_count) {
        ids[n] = sqlite3_column_int64(id_stmt, 0);
        labels[n] = (int32_t)n;
        n++;
    }
    sqlite3_finalize(id_stmt);
    entity_count = n;

    /* Label propagation: each entity adopts the most common label among neighbors */
    for (size_t iter = 0; iter < max_iterations; iter++) {
        bool changed = false;
        for (size_t i = 0; i < entity_count; i++) {
            const char *nbr_sql =
                "SELECT CASE WHEN source_id = ? THEN target_id ELSE source_id END AS nbr_id "
                "FROM relations WHERE source_id = ? OR target_id = ? "
                "ORDER BY weight DESC LIMIT 50";
            sqlite3_stmt *nbr_stmt = NULL;
            if (sqlite3_prepare_v2(g->db, nbr_sql, -1, &nbr_stmt, NULL) != SQLITE_OK)
                continue;
            sqlite3_bind_int64(nbr_stmt, 1, ids[i]);
            sqlite3_bind_int64(nbr_stmt, 2, ids[i]);
            sqlite3_bind_int64(nbr_stmt, 3, ids[i]);

            int32_t lbl_counts[64];
            int32_t lbl_values[64];
            size_t lc = 0;
            memset(lbl_counts, 0, sizeof(lbl_counts));

            while (sqlite3_step(nbr_stmt) == SQLITE_ROW) {
                int64_t nbr_id = sqlite3_column_int64(nbr_stmt, 0);
                int32_t nbr_label = -1;
                for (size_t j = 0; j < entity_count; j++) {
                    if (ids[j] == nbr_id) {
                        nbr_label = labels[j];
                        break;
                    }
                }
                if (nbr_label < 0)
                    continue;
                bool found = false;
                for (size_t k = 0; k < lc; k++) {
                    if (lbl_values[k] == nbr_label) {
                        lbl_counts[k]++;
                        found = true;
                        break;
                    }
                }
                if (!found && lc < 64) {
                    lbl_values[lc] = nbr_label;
                    lbl_counts[lc] = 1;
                    lc++;
                }
            }
            sqlite3_finalize(nbr_stmt);

            if (lc > 0) {
                int32_t best_label = labels[i];
                int32_t best_count = 0;
                for (size_t k = 0; k < lc; k++) {
                    if (lbl_counts[k] > best_count) {
                        best_count = lbl_counts[k];
                        best_label = lbl_values[k];
                    }
                }
                if (best_label != labels[i]) {
                    labels[i] = best_label;
                    changed = true;
                }
            }
        }
        if (!changed)
            break;
    }

    /* Persist community assignments */
    const char *up_sql = "UPDATE entities SET community_id = ? WHERE id = ?";
    sqlite3_stmt *up_stmt = NULL;
    if (sqlite3_prepare_v2(g->db, up_sql, -1, &up_stmt, NULL) == SQLITE_OK) {
        for (size_t i = 0; i < entity_count; i++) {
            sqlite3_bind_int(up_stmt, 1, labels[i]);
            sqlite3_bind_int64(up_stmt, 2, ids[i]);
            sqlite3_step(up_stmt);
            sqlite3_reset(up_stmt);
        }
        sqlite3_finalize(up_stmt);
    }

    /* Build hierarchical summary grouped by community */
    int32_t unique_labels[64];
    size_t unique_count = 0;
    for (size_t i = 0; i < entity_count && unique_count < 64; i++) {
        bool dup = false;
        for (size_t j = 0; j < unique_count; j++) {
            if (unique_labels[j] == labels[i]) {
                dup = true;
                break;
            }
        }
        if (!dup)
            unique_labels[unique_count++] = labels[i];
    }
    if (unique_count > max_communities)
        unique_count = max_communities;

    size_t cap = 4096;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) {
        alloc->free(alloc->ctx, ids, entity_count * sizeof(int64_t));
        alloc->free(alloc->ctx, labels, entity_count * sizeof(int32_t));
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t pos = 0;
    int w = snprintf(buf, cap, "### Knowledge Communities\n");
    if (w > 0)
        pos = (size_t)w;

    for (size_t c = 0; c < unique_count && pos < cap - 128; c++) {
        w = snprintf(buf + pos, cap - pos, "\n**Community %zu**: ", c + 1);
        if (w > 0)
            pos += (size_t)w;
        bool first = true;
        size_t members = 0;
        for (size_t i = 0; i < entity_count && pos < cap - 64; i++) {
            if (labels[i] != unique_labels[c])
                continue;
            const char *name_sql = "SELECT name FROM entities WHERE id = ?";
            sqlite3_stmt *nm_stmt = NULL;
            if (sqlite3_prepare_v2(g->db, name_sql, -1, &nm_stmt, NULL) != SQLITE_OK)
                continue;
            sqlite3_bind_int64(nm_stmt, 1, ids[i]);
            if (sqlite3_step(nm_stmt) == SQLITE_ROW) {
                const char *name = (const char *)sqlite3_column_text(nm_stmt, 0);
                if (name && pos < cap - 64) {
                    if (!first) {
                        w = snprintf(buf + pos, cap - pos, ", ");
                        if (w > 0)
                            pos += (size_t)w;
                    }
                    w = snprintf(buf + pos, cap - pos, "%s", name);
                    if (w > 0)
                        pos += (size_t)w;
                    first = false;
                    members++;
                }
            }
            sqlite3_finalize(nm_stmt);
            if (members >= 10)
                break;
        }
        if (pos < cap - 4) {
            w = snprintf(buf + pos, cap - pos, "\n");
            if (w > 0)
                pos += (size_t)w;
        }
    }

    alloc->free(alloc->ctx, ids, entity_count * sizeof(int64_t));
    alloc->free(alloc->ctx, labels, entity_count * sizeof(int32_t));
    buf[pos] = '\0';
    *out = buf;
    *out_len = pos;
    return HU_OK;
}
#endif /* HU_ENABLE_SQLITE */

/* ── Phase 3c: Ebbinghaus recall tracking ───────────────────────────── */

#ifdef HU_ENABLE_SQLITE
hu_error_t hu_graph_record_recall(hu_graph_t *g, int64_t entity_id) {
    if (!g || !g->db)
        return HU_ERR_INVALID_ARGUMENT;
    const char *sql = "UPDATE entities SET recall_count = recall_count + 1,"
                      " last_recalled = ? WHERE id = ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_int64(stmt, 1, (int64_t)time(NULL));
    sqlite3_bind_int64(stmt, 2, entity_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_IO;
}
#endif /* HU_ENABLE_SQLITE */

#include <math.h>

double hu_graph_retention_score(int64_t last_recalled_ts, int32_t recall_count, int64_t now_ts) {
    if (recall_count <= 0 || last_recalled_ts <= 0)
        return 0.0;
    double elapsed_days = (double)(now_ts - last_recalled_ts) / 86400.0;
    if (elapsed_days < 0.0)
        elapsed_days = 0.0;
    double stability = 1.0 + (double)recall_count * 0.5;
    return exp(-elapsed_days / stability);
}

/* ── Phase 3d: Conflict-aware reconsolidation ───────────────────────── */

#ifdef HU_ENABLE_SQLITE
bool hu_graph_detect_conflict(hu_graph_t *g, hu_allocator_t *alloc, const char *entity_name,
                              size_t name_len, const char *new_context, size_t new_context_len) {
    if (!g || !g->db || !alloc || !entity_name || !new_context)
        return false;
    const char *sql = "SELECT r.context FROM relations r "
                      "JOIN entities e ON (e.id = r.source_id OR e.id = r.target_id) "
                      "WHERE e.name = ? AND r.context IS NOT NULL LIMIT 10";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, entity_name, (int)name_len, NULL);
    bool conflict = false;
    while (sqlite3_step(stmt) == SQLITE_ROW && !conflict) {
        const char *old_ctx = (const char *)sqlite3_column_text(stmt, 0);
        size_t old_len = (size_t)sqlite3_column_bytes(stmt, 0);
        if (!old_ctx || old_len == 0)
            continue;
        uint32_t sim = hu_similarity_score(old_ctx, old_len, new_context, new_context_len);
        if (sim < 30 && old_len > 10 && new_context_len > 10)
            conflict = true;
    }
    sqlite3_finalize(stmt);
    return conflict;
}

hu_error_t hu_graph_reconsolidate(hu_graph_t *g, hu_allocator_t *alloc, const char *entity_name,
                                  size_t name_len, const char *new_context,
                                  size_t new_context_len) {
    if (!g || !g->db || !alloc || !entity_name || !new_context)
        return HU_ERR_INVALID_ARGUMENT;

    hu_graph_entity_t entity;
    hu_error_t err = hu_graph_find_entity(g, entity_name, name_len, &entity);
    if (err != HU_OK)
        return err;

    bool has_conflict =
        hu_graph_detect_conflict(g, alloc, entity_name, name_len, new_context, new_context_len);
    if (has_conflict) {
        const char *dup_sql = "INSERT INTO entities(name, type, first_seen, last_seen,"
                              " mention_count, supersedes_id) "
                              "VALUES(?, ?, ?, ?, 1, ?)";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(g->db, dup_sql, -1, &stmt, NULL) == SQLITE_OK) {
            char versioned[256];
            int vn = snprintf(versioned, sizeof(versioned), "%.*s [superseded]", (int)name_len,
                              entity_name);
            if (vn > 0 && (size_t)vn < sizeof(versioned)) {
                int64_t now = (int64_t)time(NULL) * 1000;
                sqlite3_bind_text(stmt, 1, versioned, vn, NULL);
                sqlite3_bind_int(stmt, 2, (int)entity.type);
                sqlite3_bind_int64(stmt, 3, entity.first_seen);
                sqlite3_bind_int64(stmt, 4, now);
                sqlite3_bind_int64(stmt, 5, entity.id);
                if (sqlite3_step(stmt) != SQLITE_DONE)
                    fprintf(stderr, "[graph] supersede entity %lld: step failed\n",
                            (long long)entity.id);
            }
            sqlite3_finalize(stmt);
        }
    }

    const char *up_sql = "UPDATE relations SET context = ?, last_seen = ? "
                         "WHERE (source_id = ? OR target_id = ?) AND context IS NOT NULL";
    sqlite3_stmt *up_stmt = NULL;
    if (sqlite3_prepare_v2(g->db, up_sql, -1, &up_stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(up_stmt, 1, new_context, (int)new_context_len, NULL);
        sqlite3_bind_int64(up_stmt, 2, (int64_t)time(NULL) * 1000);
        sqlite3_bind_int64(up_stmt, 3, entity.id);
        sqlite3_bind_int64(up_stmt, 4, entity.id);
        if (sqlite3_step(up_stmt) != SQLITE_DONE)
            fprintf(stderr, "[graph] update relations for entity %lld: step failed\n",
                    (long long)entity.id);
        sqlite3_finalize(up_stmt);
    }
    if (entity.name)
        alloc->free(alloc->ctx, entity.name, entity.name_len + 1);
    if (entity.metadata_json)
        alloc->free(alloc->ctx, (void *)entity.metadata_json, strlen(entity.metadata_json) + 1);
    return HU_OK;
}
#endif /* HU_ENABLE_SQLITE */
