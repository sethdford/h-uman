#include "human/core/log.h"
#include "human/memory/graph.h"
#include "human/core/string.h"
#include "human/memory/conflict_resolver.h"
#include "human/memory/consolidation.h"

/* P2 #7 — write→invalidate hook. We forward-declare the W9 invalidate
 * function instead of including world_model.h to avoid the agent/memory
 * header cycle. The function lives in src/agent/world_model.c and is
 * always linked (it's part of human_core). When the world-model cache
 * has no slot for this contact, the call is a 1-line array scan; cost
 * is negligible. */
extern void hu_world_model_invalidate(const char *contact_id, size_t cid_len);
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HU_ENABLE_SQLITE
#include "human/memory/sql_common.h"
#include "human/memory/sql_transaction.h"
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
    "contact_id TEXT NOT NULL DEFAULT '',"
    "name TEXT NOT NULL,"
    "type INTEGER NOT NULL DEFAULT 6,"
    "first_seen INTEGER NOT NULL,"
    "last_seen INTEGER NOT NULL,"
    "mention_count INTEGER NOT NULL DEFAULT 1,"
    "metadata_json TEXT,"
    "community_id INTEGER DEFAULT NULL,"
    "recall_count INTEGER NOT NULL DEFAULT 0,"
    "last_recalled INTEGER DEFAULT NULL,"
    "supersedes_id INTEGER DEFAULT NULL,"
    "UNIQUE(contact_id, name))",
    "CREATE TABLE IF NOT EXISTS relations ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "contact_id TEXT NOT NULL DEFAULT '',"
    "source_id INTEGER NOT NULL REFERENCES entities(id),"
    "target_id INTEGER NOT NULL REFERENCES entities(id),"
    "relation_type INTEGER NOT NULL,"
    "weight REAL NOT NULL DEFAULT 1.0,"
    "first_seen INTEGER NOT NULL,"
    "last_seen INTEGER NOT NULL,"
    "context TEXT,"
    "UNIQUE(source_id, target_id, relation_type))",
    "CREATE TABLE IF NOT EXISTS temporal_events ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "contact_id TEXT NOT NULL DEFAULT '',"
    "entity_id INTEGER REFERENCES entities(id),"
    "description TEXT NOT NULL,"
    "occurred_at INTEGER NOT NULL,"
    "duration_sec INTEGER DEFAULT 0)",
    "CREATE TABLE IF NOT EXISTS causal_links ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "contact_id TEXT NOT NULL DEFAULT '',"
    "action_entity_id INTEGER REFERENCES entities(id),"
    "outcome_entity_id INTEGER REFERENCES entities(id),"
    "context TEXT,"
    "confidence REAL NOT NULL DEFAULT 0.5,"
    "created_at INTEGER NOT NULL,"
    "UNIQUE(action_entity_id, outcome_entity_id))",
    "CREATE INDEX IF NOT EXISTS idx_relations_source ON relations(source_id)",
    "CREATE INDEX IF NOT EXISTS idx_relations_target ON relations(target_id)",
    "CREATE INDEX IF NOT EXISTS idx_entities_name ON entities(name)",
    "CREATE INDEX IF NOT EXISTS idx_entities_contact ON entities(contact_id, name)",
    "CREATE INDEX IF NOT EXISTS idx_relations_contact ON relations(contact_id)",
    "CREATE INDEX IF NOT EXISTS idx_temporal_events_entity ON temporal_events(entity_id)",
    "CREATE INDEX IF NOT EXISTS idx_temporal_events_occurred ON temporal_events(occurred_at)",
    "CREATE INDEX IF NOT EXISTS idx_temporal_events_contact ON temporal_events(contact_id)",
    "CREATE INDEX IF NOT EXISTS idx_causal_links_action ON causal_links(action_entity_id)",
    "CREATE INDEX IF NOT EXISTS idx_causal_links_outcome ON causal_links(outcome_entity_id)",
    "CREATE INDEX IF NOT EXISTS idx_causal_links_contact ON causal_links(contact_id)",
    NULL,
};

/* Migration: add contact_id (legacy) and W1 bitemporal columns. Each ALTER is
 * idempotent on reopen via the duplicate-column-name benign-error path. */
static const char *const MIGRATION[] = {
    "ALTER TABLE entities ADD COLUMN contact_id TEXT NOT NULL DEFAULT ''",
    "ALTER TABLE relations ADD COLUMN contact_id TEXT NOT NULL DEFAULT ''",
    "ALTER TABLE temporal_events ADD COLUMN contact_id TEXT NOT NULL DEFAULT ''",
    "ALTER TABLE causal_links ADD COLUMN contact_id TEXT NOT NULL DEFAULT ''",
    "ALTER TABLE relations ADD COLUMN event_start INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE relations ADD COLUMN event_end INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE relations ADD COLUMN confidence REAL NOT NULL DEFAULT 1.0",
    "ALTER TABLE relations ADD COLUMN supersedes_id INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE relations ADD COLUMN provenance TEXT",
    "ALTER TABLE entities ADD COLUMN event_start INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE entities ADD COLUMN event_end INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE entities ADD COLUMN confidence REAL NOT NULL DEFAULT 1.0",
    "ALTER TABLE entities ADD COLUMN provenance TEXT",
    "UPDATE relations SET event_start = first_seen WHERE event_start = 0",
    "UPDATE entities SET event_start = first_seen WHERE event_start = 0",
    /* W8 P2A — promote scalar confidence to a (mean, variance) Bayesian
     * posterior. Old rows get variance = 0 (the deterministic-truth
     * default), and confidence_mean is back-filled from the existing
     * confidence column. New writes populate both columns; legacy
     * readers that look only at `confidence` keep working because we
     * keep that column in sync (UPDATE below + write paths). */
    "ALTER TABLE relations ADD COLUMN confidence_mean REAL NOT NULL DEFAULT 1.0",
    "ALTER TABLE relations ADD COLUMN confidence_variance REAL NOT NULL DEFAULT 0.0",
    "UPDATE relations SET confidence_mean = confidence WHERE confidence_mean = 1.0 AND confidence != 1.0",
    "ALTER TABLE entities ADD COLUMN confidence_mean REAL NOT NULL DEFAULT 1.0",
    "ALTER TABLE entities ADD COLUMN confidence_variance REAL NOT NULL DEFAULT 0.0",
    "UPDATE entities SET confidence_mean = confidence WHERE confidence_mean = 1.0 AND confidence != 1.0",
    "CREATE INDEX IF NOT EXISTS idx_entities_contact ON entities(contact_id, name)",
    "CREATE INDEX IF NOT EXISTS idx_relations_contact ON relations(contact_id)",
    "CREATE INDEX IF NOT EXISTS idx_temporal_events_contact ON temporal_events(contact_id)",
    "CREATE INDEX IF NOT EXISTS idx_causal_links_contact ON causal_links(contact_id)",
    "CREATE INDEX IF NOT EXISTS idx_relations_event_window ON relations(event_start, event_end)",
    "CREATE INDEX IF NOT EXISTS idx_relations_supersedes ON relations(supersedes_id)",
    "CREATE TABLE IF NOT EXISTS quarantine_relations ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "contact_id TEXT NOT NULL DEFAULT '',"
    "source_id INTEGER NOT NULL,"
    "target_id INTEGER NOT NULL,"
    "relation_type INTEGER NOT NULL,"
    "weight REAL NOT NULL DEFAULT 1.0,"
    "first_seen INTEGER NOT NULL,"
    "last_seen INTEGER NOT NULL,"
    "event_start INTEGER NOT NULL DEFAULT 0,"
    "event_end INTEGER NOT NULL DEFAULT 0,"
    "confidence REAL NOT NULL DEFAULT 1.0,"
    "context TEXT,"
    "provenance TEXT,"
    "trust_score REAL NOT NULL DEFAULT 0.0,"
    "trust_reason TEXT,"
    "quarantined_at INTEGER NOT NULL)",
    "CREATE INDEX IF NOT EXISTS idx_quarantine_contact ON quarantine_relations(contact_id)",
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
    if (!db_path && db_path_len > 0) {
        alloc->free(alloc->ctx, g, sizeof(hu_graph_t));
        return HU_ERR_INVALID_ARGUMENT;
    }
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

    /* Migrations: duplicate column / existing object errors are benign on reopen. */
    for (size_t i = 0; MIGRATION[i] != NULL; i++) {
        char *mig_err = NULL;
        int mig_rc = sqlite3_exec(g->db, MIGRATION[i], NULL, NULL, &mig_err);
        if (mig_rc != SQLITE_OK) {
            bool benign = false;
            if (mig_err && strstr(mig_err, "duplicate column name") != NULL)
                benign = true;
            if (mig_err)
                sqlite3_free(mig_err);
            if (!benign) {
                sqlite3_close(g->db);
                alloc->free(alloc->ctx, g, sizeof(hu_graph_t));
                return HU_ERR_IO;
            }
        } else if (mig_err) {
            sqlite3_free(mig_err);
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

hu_error_t hu_graph_upsert_entity(hu_graph_t *g, const char *contact_id, size_t contact_id_len,
                                  const char *name, size_t name_len, hu_entity_type_t type,
                                  const char *metadata_json, int64_t *out_id) {
    if (!g || !g->db || !name || name_len == 0 || !out_id)
        return HU_ERR_INVALID_ARGUMENT;

    const char *cid = contact_id ? contact_id : "";
    int cid_len = contact_id ? (int)contact_id_len : 0;

    hu_sql_txn_t txn = {0};
    if (hu_sql_txn_begin(&txn, g->db) != HU_OK)
        return HU_ERR_IO;

    int64_t ts = now_ms();
    sqlite3_stmt *ins = NULL;
    const char *ins_sql =
        "INSERT INTO entities (contact_id, name, type, first_seen, last_seen, mention_count,"
        " metadata_json) VALUES (?, ?, ?, ?, ?, 1, ?)";
    int rc = sqlite3_prepare_v2(g->db, ins_sql, -1, &ins, NULL);
    if (rc != SQLITE_OK) {
        hu_sql_txn_rollback(&txn);
        return HU_ERR_IO;
    }

    sqlite3_bind_text(ins, 1, cid, cid_len, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, name, (int)name_len, SQLITE_STATIC);
    sqlite3_bind_int(ins, 3, (int)type);
    sqlite3_bind_int64(ins, 4, ts);
    sqlite3_bind_int64(ins, 5, ts);
    if (metadata_json)
        sqlite3_bind_text(ins, 6, metadata_json, (int)strlen(metadata_json), SQLITE_STATIC);
    else
        sqlite3_bind_null(ins, 6);

    rc = sqlite3_step(ins);
    sqlite3_finalize(ins);
    if (rc == SQLITE_DONE) {
        int64_t new_id = sqlite3_last_insert_rowid(g->db);
        if (hu_sql_txn_commit(&txn) != HU_OK) {
            hu_sql_txn_rollback(&txn);
            return HU_ERR_MEMORY_BACKEND;
        }
        *out_id = new_id;
        /* P2 #7 — new entity → world-model cache for this contact is stale. */
        hu_world_model_invalidate(cid, (size_t)cid_len);
        return HU_OK;
    }
    if (rc != SQLITE_CONSTRAINT) {
        hu_sql_txn_rollback(&txn);
        return HU_ERR_IO;
    }

    sqlite3_stmt *upd = NULL;
    const char *upd_sql =
        "UPDATE entities SET last_seen = ?, mention_count = mention_count + 1"
        " WHERE contact_id = ? AND name = ?";
    rc = sqlite3_prepare_v2(g->db, upd_sql, -1, &upd, NULL);
    if (rc != SQLITE_OK) {
        hu_sql_txn_rollback(&txn);
        return HU_ERR_IO;
    }
    sqlite3_bind_int64(upd, 1, ts);
    sqlite3_bind_text(upd, 2, cid, cid_len, SQLITE_STATIC);
    sqlite3_bind_text(upd, 3, name, (int)name_len, SQLITE_STATIC);
    rc = sqlite3_step(upd);
    sqlite3_finalize(upd);
    if (rc != SQLITE_DONE) {
        hu_sql_txn_rollback(&txn);
        return HU_ERR_IO;
    }

    sqlite3_stmt *sel = NULL;
    const char *sel_sql = "SELECT id FROM entities WHERE contact_id = ? AND name = ?";
    rc = sqlite3_prepare_v2(g->db, sel_sql, -1, &sel, NULL);
    if (rc != SQLITE_OK) {
        hu_sql_txn_rollback(&txn);
        return HU_ERR_IO;
    }
    sqlite3_bind_text(sel, 1, cid, cid_len, SQLITE_STATIC);
    sqlite3_bind_text(sel, 2, name, (int)name_len, SQLITE_STATIC);
    int sel_rc = sqlite3_step(sel);
    if (sel_rc == SQLITE_ROW) {
        *out_id = sqlite3_column_int64(sel, 0);
        sqlite3_finalize(sel);
        if (hu_sql_txn_commit(&txn) != HU_OK) {
            hu_sql_txn_rollback(&txn);
            return HU_ERR_IO;
        }
        return HU_OK;
    }
    sqlite3_finalize(sel);
    if (sel_rc == SQLITE_DONE) {
        hu_sql_txn_rollback(&txn);
        return HU_ERR_NOT_FOUND;
    }
    hu_sql_txn_rollback(&txn);
    return HU_ERR_IO;
}

#else

hu_error_t hu_graph_upsert_entity(hu_graph_t *g, const char *contact_id, size_t contact_id_len,
                                  const char *name, size_t name_len, hu_entity_type_t type,
                                  const char *metadata_json, int64_t *out_id) {
    (void)g;
    (void)contact_id;
    (void)contact_id_len;
    (void)name;
    (void)name_len;
    (void)type;
    (void)metadata_json;
    (void)out_id;
    return HU_ERR_NOT_SUPPORTED;
}

#endif

#ifdef HU_ENABLE_SQLITE

hu_error_t hu_graph_find_entity(hu_graph_t *g, const char *contact_id, size_t contact_id_len,
                                const char *name, size_t name_len, hu_graph_entity_t *out) {
    if (!g || !g->db || !name || !out)
        return HU_ERR_INVALID_ARGUMENT;

    const char *cid = contact_id ? contact_id : "";
    int cid_len = contact_id ? (int)contact_id_len : 0;

    memset(out, 0, sizeof(*out));
    const char *sql = "SELECT id, name, type, first_seen, last_seen, mention_count, metadata_json "
                      "FROM entities WHERE contact_id = ? AND name = ?";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_text(stmt, 1, cid, cid_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, name, (int)name_len, SQLITE_STATIC);
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

hu_error_t hu_graph_find_entity(hu_graph_t *g, const char *contact_id, size_t contact_id_len,
                                const char *name, size_t name_len, hu_graph_entity_t *out) {
    (void)g;
    (void)contact_id;
    (void)contact_id_len;
    (void)name;
    (void)name_len;
    (void)out;
    return HU_ERR_NOT_SUPPORTED;
}

#endif

#ifdef HU_ENABLE_SQLITE

hu_error_t hu_graph_upsert_relation(hu_graph_t *g, const char *contact_id, size_t contact_id_len,
                                    int64_t source_id, int64_t target_id,
                                    hu_relation_type_t type, float weight, const char *context,
                                    size_t context_len) {
    if (!g || !g->db)
        return HU_ERR_INVALID_ARGUMENT;

    const char *cid = contact_id ? contact_id : "";
    int cid_len = contact_id ? (int)contact_id_len : 0;

    int64_t ts = now_ms();
    /* Legacy upsert: bitemporal columns get sane defaults (event_start = ts,
     * event_end = 0 = "still true", confidence = 1.0, no provenance). New
     * callers should prefer hu_graph_upsert_relation_ex which goes through the
     * conflict resolver. ON CONFLICT keeps backward semantics for legacy paths.
     * W8: writes confidence_mean/confidence_variance explicitly to stay in
     * sync with the scalar confidence column. */
    const char *sql =
        "INSERT INTO relations (contact_id, source_id, target_id, relation_type,"
        " weight, first_seen, last_seen, context, event_start, event_end,"
        " confidence, supersedes_id, provenance,"
        " confidence_mean, confidence_variance)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 0, 1.0, 0, NULL, 1.0, 0.0) "
        "ON CONFLICT(source_id, target_id, relation_type) DO UPDATE SET "
        "weight = (weight + excluded.weight) / 2.0, last_seen = excluded.last_seen, "
        "context = excluded.context";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_text(stmt, 1, cid, cid_len, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, source_id);
    sqlite3_bind_int64(stmt, 3, target_id);
    sqlite3_bind_int(stmt, 4, (int)type);
    sqlite3_bind_double(stmt, 5, (double)weight);
    sqlite3_bind_int64(stmt, 6, ts);
    sqlite3_bind_int64(stmt, 7, ts);
    if (context && context_len > 0)
        sqlite3_bind_text(stmt, 8, context, (int)context_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 8);
    sqlite3_bind_int64(stmt, 9, ts); /* event_start = now (legacy path) */

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_IO;
}

/* W1 — Bitemporal upsert. Differs from legacy in three ways:
 *   1. Caller supplies event_start, event_end, confidence, provenance.
 *   2. Before INSERT, we fetch the strongest existing open relation with the
 *      same (contact_id, source_id, type) and ask the conflict resolver what
 *      to do. SUPERSEDE closes the prior (event_end = cutover) and links new
 *      to old via supersedes_id.
 *   3. We do NOT use ON CONFLICT(source,target,type) because supersession may
 *      need to insert a new row with a different target while keeping the old.
 *      Instead, we INSERT-or-UPDATE based on the resolver's decision.
 */
hu_error_t hu_graph_upsert_relation_ex(hu_graph_t *g, const char *contact_id,
                                       size_t contact_id_len, int64_t source_id, int64_t target_id,
                                       hu_relation_type_t type, float weight, int64_t event_start,
                                       int64_t event_end, float confidence, const char *context,
                                       size_t context_len, const char *provenance,
                                       size_t provenance_len) {
    if (!g || !g->db || source_id <= 0 || target_id <= 0)
        return HU_ERR_INVALID_ARGUMENT;

    const char *cid = contact_id ? contact_id : "";
    int cid_len = contact_id ? (int)contact_id_len : 0;
    int64_t ts = now_ms();
    if (event_start <= 0)
        event_start = ts;
    if (confidence < 0.0f)
        confidence = 1.0f;
    if (confidence > 1.0f)
        confidence = 1.0f;

    /* Pre-read the strongest open existing relation with the same (source,
     * type) on the same contact. Used by the resolver. */
    hu_graph_relation_t existing = {0};
    {
        const char *peek_sql = "SELECT id, source_id, target_id, relation_type, weight,"
                               " first_seen, last_seen, event_start, event_end, confidence,"
                               " supersedes_id "
                               "FROM relations "
                               "WHERE contact_id = ? AND source_id = ? AND relation_type = ?"
                               " AND event_end = 0 "
                               "ORDER BY confidence DESC, last_seen DESC LIMIT 1";
        sqlite3_stmt *peek = NULL;
        if (sqlite3_prepare_v2(g->db, peek_sql, -1, &peek, NULL) == SQLITE_OK) {
            sqlite3_bind_text(peek, 1, cid, cid_len, SQLITE_STATIC);
            sqlite3_bind_int64(peek, 2, source_id);
            sqlite3_bind_int(peek, 3, (int)type);
            if (sqlite3_step(peek) == SQLITE_ROW) {
                existing.id = sqlite3_column_int64(peek, 0);
                existing.source_id = sqlite3_column_int64(peek, 1);
                existing.target_id = sqlite3_column_int64(peek, 2);
                existing.type = (hu_relation_type_t)sqlite3_column_int(peek, 3);
                existing.weight = (float)sqlite3_column_double(peek, 4);
                existing.first_seen = sqlite3_column_int64(peek, 5);
                existing.last_seen = sqlite3_column_int64(peek, 6);
                existing.event_start = sqlite3_column_int64(peek, 7);
                existing.event_end = sqlite3_column_int64(peek, 8);
                existing.confidence = (float)sqlite3_column_double(peek, 9);
                existing.supersedes_id = sqlite3_column_int64(peek, 10);
            }
            sqlite3_finalize(peek);
        }
    }

    hu_graph_relation_t proposed = {0};
    proposed.source_id = source_id;
    proposed.target_id = target_id;
    proposed.type = type;
    proposed.weight = weight;
    proposed.event_start = event_start;
    proposed.event_end = event_end;
    proposed.confidence = confidence;

    hu_conflict_resolution_t decision = hu_conflict_classify(&proposed, &existing);

    /* INSERT the proposed row. Always a fresh row — the resolver may close the
     * prior afterwards. We don't use ON CONFLICT here: bitemporal history
     * requires multiple rows, so we want a new id even if (source,target,type)
     * already exists. W8 P2A: also seed confidence_mean = confidence (variance
     * = 0 by default per migration); upserts that want a non-zero variance
     * should call hu_graph_set_relation_belief after upsert. */
    sqlite3_stmt *ins = NULL;
    const char *ins_sql =
        "INSERT INTO relations (contact_id, source_id, target_id, relation_type, weight,"
        " first_seen, last_seen, context, event_start, event_end, confidence, supersedes_id,"
        " provenance, confidence_mean, confidence_variance)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?, 0.0)";
    int rc = sqlite3_prepare_v2(g->db, ins_sql, -1, &ins, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_text(ins, 1, cid, cid_len, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 2, source_id);
    sqlite3_bind_int64(ins, 3, target_id);
    sqlite3_bind_int(ins, 4, (int)type);
    sqlite3_bind_double(ins, 5, (double)weight);
    sqlite3_bind_int64(ins, 6, ts);
    sqlite3_bind_int64(ins, 7, ts);
    if (context && context_len > 0)
        sqlite3_bind_text(ins, 8, context, (int)context_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(ins, 8);
    sqlite3_bind_int64(ins, 9, event_start);
    sqlite3_bind_int64(ins, 10, event_end);
    sqlite3_bind_double(ins, 11, (double)confidence);
    if (provenance && provenance_len > 0)
        sqlite3_bind_text(ins, 12, provenance, (int)provenance_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(ins, 12);
    sqlite3_bind_double(ins, 13, (double)confidence);

    rc = sqlite3_step(ins);
    int64_t proposed_id = (rc == SQLITE_DONE) ? sqlite3_last_insert_rowid(g->db) : 0;
    sqlite3_finalize(ins);
    if (rc != SQLITE_DONE)
        return HU_ERR_IO;

    /* Apply the resolution. SUPERSEDE closes the prior + links via supersedes_id. */
    if (decision == HU_CONFLICT_SUPERSEDE) {
        hu_error_t apply_rc =
            hu_conflict_apply(g, decision, proposed_id, existing.id, event_start);
        if (apply_rc != HU_OK)
            return apply_rc;
    }

    /* P2 #7 — invalidate the W9 world-model cache for this contact.
     * Wildcard invalidation (NULL contact_id) is also wired so global
     * facts invalidate every cached slot. */
    hu_world_model_invalidate(cid, (size_t)cid_len);
    return HU_OK;
}

hu_error_t hu_graph_relations_in_window(hu_graph_t *g, hu_allocator_t *alloc,
                                        const char *contact_id, size_t contact_id_len,
                                        int64_t from_ts, int64_t to_ts, size_t limit,
                                        hu_graph_relation_t **out, size_t *out_count) {
    if (!g || !g->db || !alloc || !out || !out_count || limit == 0)
        return HU_ERR_INVALID_ARGUMENT;

    const char *cid = contact_id ? contact_id : "";
    int cid_len = contact_id ? (int)contact_id_len : 0;

    /* Overlap predicate: the relation's [event_start, event_end_or_now] overlaps
     * the requested [from_ts, to_ts] as a half-open interval at the cutover.
     * event_end = 0 is treated as "still true". A relation that ended *exactly*
     * at from_ts no longer overlaps (the cutover instant belongs to the next
     * window) — hence event_end > ?, not >=. */
    const char *sql =
        "SELECT id, source_id, target_id, relation_type, weight, first_seen, last_seen, context,"
        " event_start, event_end, confidence, supersedes_id, provenance "
        "FROM relations WHERE contact_id = ? "
        "AND event_start <= ? "
        "AND (event_end = 0 OR event_end > ?) "
        "ORDER BY event_start DESC LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(stmt, 1, cid, cid_len, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, to_ts);
    sqlite3_bind_int64(stmt, 3, from_ts);
    sqlite3_bind_int64(stmt, 4, (int64_t)limit);

    size_t cap = limit < 16 ? limit : 16;
    hu_graph_relation_t *arr = alloc->alloc(alloc->ctx, cap * sizeof(hu_graph_relation_t));
    if (!arr) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t count = 0;
    bool oom = false;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            size_t new_cap = cap * 2 > limit ? limit : cap * 2;
            hu_graph_relation_t *tmp =
                alloc->alloc(alloc->ctx, new_cap * sizeof(hu_graph_relation_t));
            if (!tmp) {
                oom = true;
                break;
            }
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
        if (ctx) {
            r->context_len = (size_t)sqlite3_column_bytes(stmt, 7);
            r->context = hu_strndup(alloc, ctx, r->context_len);
        }
        r->event_start = sqlite3_column_int64(stmt, 8);
        r->event_end = sqlite3_column_int64(stmt, 9);
        r->confidence = (float)sqlite3_column_double(stmt, 10);
        r->supersedes_id = sqlite3_column_int64(stmt, 11);
        const char *prov = (const char *)sqlite3_column_text(stmt, 12);
        if (prov) {
            r->provenance_len = (size_t)sqlite3_column_bytes(stmt, 12);
            r->provenance = hu_strndup(alloc, prov, r->provenance_len);
        }
        count++;
    }
    sqlite3_finalize(stmt);
    if (oom) {
        hu_graph_relations_free(alloc, arr, count);
        *out = NULL;
        *out_count = 0;
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = arr;
    *out_count = count;
    return HU_OK;
}

/* Internal accessor for sibling modules (conflict_resolver, write_trust). Not
 * declared in a public header — sibling .c files forward-declare it. */
sqlite3 *hu_graph__db_handle(hu_graph_t *g) {
    return g ? g->db : NULL;
}

hu_error_t hu_graph_set_entity_community(hu_graph_t *g, int64_t entity_id, int64_t community_id) {
    if (!g || !g->db || entity_id <= 0)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(g->db, "UPDATE entities SET community_id = ? WHERE id = ?", -1,
                                &st, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_int64(st, 1, community_id);
    sqlite3_bind_int64(st, 2, entity_id);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_IO;
}

#else

hu_error_t hu_graph_upsert_relation(hu_graph_t *g, const char *contact_id, size_t contact_id_len,
                                    int64_t source_id, int64_t target_id,
                                    hu_relation_type_t type, float weight, const char *context,
                                    size_t context_len) {
    (void)g;
    (void)contact_id;
    (void)contact_id_len;
    (void)source_id;
    (void)target_id;
    (void)type;
    (void)weight;
    (void)context;
    (void)context_len;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_graph_upsert_relation_ex(hu_graph_t *g, const char *contact_id,
                                       size_t contact_id_len, int64_t source_id, int64_t target_id,
                                       hu_relation_type_t type, float weight, int64_t event_start,
                                       int64_t event_end, float confidence, const char *context,
                                       size_t context_len, const char *provenance,
                                       size_t provenance_len) {
    (void)g;
    (void)contact_id;
    (void)contact_id_len;
    (void)source_id;
    (void)target_id;
    (void)type;
    (void)weight;
    (void)event_start;
    (void)event_end;
    (void)confidence;
    (void)context;
    (void)context_len;
    (void)provenance;
    (void)provenance_len;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_graph_relations_in_window(hu_graph_t *g, hu_allocator_t *alloc,
                                        const char *contact_id, size_t contact_id_len,
                                        int64_t from_ts, int64_t to_ts, size_t limit,
                                        hu_graph_relation_t **out, size_t *out_count) {
    (void)g;
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
    (void)from_ts;
    (void)to_ts;
    (void)limit;
    (void)out;
    (void)out_count;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_graph_set_entity_community(hu_graph_t *g, int64_t entity_id, int64_t community_id) {
    (void)g;
    (void)entity_id;
    (void)community_id;
    return HU_ERR_NOT_SUPPORTED;
}

#endif

#ifdef HU_ENABLE_SQLITE

#define HU_GRAPH_MAX_NEIGHBORS 256

hu_error_t hu_graph_neighbors(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                              size_t contact_id_len, int64_t entity_id, size_t max_hops,
                              size_t max_results, hu_graph_entity_t **out_entities,
                              hu_graph_relation_t **out_relations, size_t *out_count) {
    if (!g || !g->db || !alloc || !out_entities || !out_relations || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

    const char *cid = contact_id ? contact_id : "";
    int cid_len = contact_id ? (int)contact_id_len : 0;

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
        "r.context, r.event_start, r.event_end, r.confidence, r.supersedes_id, r.provenance "
        "FROM entities e "
        "JOIN relations r ON (r.target_id = e.id AND r.source_id = ?) OR (r.source_id = e.id AND "
        "r.target_id = ?) "
        "WHERE e.id != ? AND e.contact_id = ?";

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
            sqlite3_bind_text(stmt, 4, cid, cid_len, SQLITE_STATIC);

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
                rel->event_start = sqlite3_column_int64(stmt, 15);
                rel->event_end = sqlite3_column_int64(stmt, 16);
                rel->confidence = (float)sqlite3_column_double(stmt, 17);
                rel->supersedes_id = sqlite3_column_int64(stmt, 18);
                const char *prov = (const char *)sqlite3_column_text(stmt, 19);
                if (prov) {
                    rel->provenance_len = (size_t)sqlite3_column_bytes(stmt, 19);
                    rel->provenance = hu_strndup(alloc, prov, rel->provenance_len);
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

hu_error_t hu_graph_neighbors(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                              size_t contact_id_len, int64_t entity_id, size_t max_hops,
                              size_t max_results, hu_graph_entity_t **out_entities,
                              hu_graph_relation_t **out_relations, size_t *out_count) {
    (void)g;
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
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

hu_error_t hu_graph_build_context(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                  size_t contact_id_len, const char *query, size_t query_len,
                                  size_t max_hops, size_t max_chars, char **out,
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
        if (hu_graph_find_entity(g, contact_id, contact_id_len, query + start, word_len, &ent) !=
            HU_OK)
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

    size_t rem = max_chars + 1;
    int n = snprintf(buf, rem, "### Knowledge Graph (from your conversations)\n");
    if (n <= 0) {
        alloc->free(alloc->ctx, buf, max_chars + 1);
        return HU_ERR_IO;
    }
    total_len = ((size_t)n < rem) ? (size_t)n : rem - 1;

    for (size_t s = 0; s < seed_count && total_len < max_chars; s++) {
        hu_graph_entity_t *entities = NULL;
        hu_graph_relation_t *relations = NULL;
        size_t count = 0;
        if (hu_graph_neighbors(g, alloc, contact_id, contact_id_len, seed_ids[s], max_hops, 20,
                               &entities, &relations, &count) != HU_OK)
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
            rem = max_chars - total_len + 1;
            int w =
                snprintf(buf + total_len, rem, "- [%.*s] (%s) -> [%.*s]\n",
                         (int)src_len, src_name, rel_str, (int)tgt_len, tgt_name);
            if (w <= 0)
                continue;
            size_t written = ((size_t)w < rem) ? (size_t)w : rem - 1;
            total_len += written;
        }
        hu_graph_entities_free(alloc, entities, count);
        hu_graph_relations_free(alloc, relations, count);
    }

    buf[total_len] = '\0';
    *out = buf;
    *out_len = total_len;
    return HU_OK;
}

hu_error_t hu_graph_build_communities(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                      size_t contact_id_len, size_t max_communities,
                                      size_t max_chars, char **out, size_t *out_len) {
    if (!g || !g->db || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_len = 0;

    const char *cid = contact_id ? contact_id : "";
    int cid_len = contact_id ? (int)contact_id_len : 0;

    const char *top_sql = "SELECT e.id, e.name, e.type FROM entities e "
                          "JOIN relations r ON (r.source_id = e.id OR r.target_id = e.id) "
                          "WHERE e.contact_id = ? "
                          "GROUP BY e.id ORDER BY COUNT(r.id) DESC LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g->db, top_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;

    size_t limit = max_communities > 0 ? max_communities : 20;
    if (limit > 64)
        limit = 64;
    sqlite3_bind_text(stmt, 1, cid, cid_len, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, (int)limit);

    char *buf = (char *)alloc->alloc(alloc->ctx, max_chars + 1);
    if (!buf) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t total_len = 0;

    size_t rem = max_chars + 1;
    int n = snprintf(buf, rem, "### Topic Clusters\n");
    if (n <= 0)
        goto free_remaining;
    total_len = ((size_t)n < rem) ? (size_t)n : rem - 1;

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
        if (hu_graph_neighbors(g, alloc, contact_id, contact_id_len, eid, 1, 10, &neighbors,
                               &relations, &ncount) != HU_OK)
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
            rem = max_chars - total_len + 1;
            int w = snprintf(buf + total_len, rem, "- %s: [", label);
            if (w <= 0)
                goto free_remaining;
            size_t written = ((size_t)w < rem) ? (size_t)w : rem - 1;
            total_len += written;
        }

        for (size_t i = 0; i < type_counts[t]; i++) {
            char *nm = type_names[t][i];
            if (!nm)
                continue;
            size_t nlen = strlen(nm);
            if (total_len < max_chars) {
                if (i > 0) {
                    rem = max_chars - total_len + 1;
                    int c = snprintf(buf + total_len, rem, ", ");
                    if (c <= 0)
                        goto free_remaining;
                    size_t cw = ((size_t)c < rem) ? (size_t)c : rem - 1;
                    total_len += cw;
                }
                if (total_len + nlen + 2 <= max_chars) {
                    memcpy(buf + total_len, nm, nlen + 1);
                    total_len += nlen;
                }
            }
            alloc->free(alloc->ctx, nm, nlen + 1);
        }

        if (total_len < max_chars) {
            rem = max_chars - total_len + 1;
            int c = snprintf(buf + total_len, rem, "]\n");
            if (c <= 0)
                goto free_remaining;
            size_t cw = ((size_t)c < rem) ? (size_t)c : rem - 1;
            total_len += cw;
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
    alloc->free(alloc->ctx, buf, max_chars + 1);
    *out = NULL;
    *out_len = 0;
    return HU_ERR_IO;

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

    /* Filter by contact_id — entities and relations are scoped per-contact */
    return hu_graph_build_context(g, alloc, contact_id, contact_id_len, query, query_len, max_hops,
                                  max_chars, out, out_len);
}

#else

hu_error_t hu_graph_build_context(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                  size_t contact_id_len, const char *query, size_t query_len,
                                  size_t max_hops, size_t max_chars, char **out,
                                  size_t *out_len) {
    (void)g;
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
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

hu_error_t hu_graph_build_communities(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                      size_t contact_id_len, size_t max_communities,
                                      size_t max_chars, char **out, size_t *out_len) {
    (void)g;
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
    (void)max_communities;
    (void)max_chars;
    (void)out;
    (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
}

#endif

#ifdef HU_ENABLE_SQLITE

hu_error_t hu_graph_list_entities(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                  size_t contact_id_len, size_t limit, hu_graph_entity_t **out,
                                  size_t *out_count) {
    if (!g || !g->db || !alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    if (limit == 0)
        limit = 100;

    const char *cid = contact_id ? contact_id : "";
    int cid_len = contact_id ? (int)contact_id_len : 0;

    const char *sql = "SELECT id, name, type, first_seen, last_seen, mention_count "
                      "FROM entities WHERE contact_id = ? ORDER BY mention_count DESC LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(stmt, 1, cid, cid_len, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (int64_t)limit);

    size_t cap = limit < 64 ? limit : 64;
    hu_graph_entity_t *arr = alloc->alloc(alloc->ctx, cap * sizeof(hu_graph_entity_t));
    if (!arr) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t count = 0;
    bool oom = false;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            size_t new_cap = cap * 2;
            hu_graph_entity_t *tmp = alloc->alloc(alloc->ctx, new_cap * sizeof(hu_graph_entity_t));
            if (!tmp) {
                oom = true;
                break;
            }
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
    if (oom) {
        hu_graph_entities_free(alloc, arr, count);
        *out = NULL;
        *out_count = 0;
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = arr;
    *out_count = count;
    return HU_OK;
}

hu_error_t hu_graph_list_relations(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                   size_t contact_id_len, size_t limit,
                                   hu_graph_relation_t **out, size_t *out_count) {
    if (!g || !g->db || !alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    if (limit == 0)
        limit = 200;

    const char *cid = contact_id ? contact_id : "";
    int cid_len = contact_id ? (int)contact_id_len : 0;

    const char *sql = "SELECT r.id, r.source_id, r.target_id, r.relation_type, r.weight, "
                      "r.first_seen, r.last_seen, r.context, r.event_start, r.event_end, "
                      "r.confidence, r.supersedes_id, r.provenance "
                      "FROM relations r WHERE r.contact_id = ? ORDER BY r.weight DESC LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(stmt, 1, cid, cid_len, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (int64_t)limit);

    size_t cap = limit < 64 ? limit : 64;
    hu_graph_relation_t *arr = alloc->alloc(alloc->ctx, cap * sizeof(hu_graph_relation_t));
    if (!arr) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t count = 0;
    bool oom = false;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            size_t new_cap = cap * 2;
            hu_graph_relation_t *tmp =
                alloc->alloc(alloc->ctx, new_cap * sizeof(hu_graph_relation_t));
            if (!tmp) {
                oom = true;
                break;
            }
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
        r->event_start = sqlite3_column_int64(stmt, 8);
        r->event_end = sqlite3_column_int64(stmt, 9);
        r->confidence = (float)sqlite3_column_double(stmt, 10);
        r->supersedes_id = sqlite3_column_int64(stmt, 11);
        const char *prov = (const char *)sqlite3_column_text(stmt, 12);
        r->provenance_len = prov ? strlen(prov) : 0;
        r->provenance = r->provenance_len ? hu_strndup(alloc, prov, r->provenance_len) : NULL;
        count++;
    }
    sqlite3_finalize(stmt);
    if (oom) {
        hu_graph_relations_free(alloc, arr, count);
        *out = NULL;
        *out_count = 0;
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = arr;
    *out_count = count;
    return HU_OK;
}

#else

hu_error_t hu_graph_list_entities(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                  size_t contact_id_len, size_t limit, hu_graph_entity_t **out,
                                  size_t *out_count) {
    (void)g;
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
    (void)limit;
    (void)out;
    (void)out_count;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_graph_list_relations(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                   size_t contact_id_len, size_t limit,
                                   hu_graph_relation_t **out, size_t *out_count) {
    (void)g;
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
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
        if (relations[i].provenance)
            alloc->free(alloc->ctx, relations[i].provenance, relations[i].provenance_len + 1);
    }
    alloc->free(alloc->ctx, relations, count * sizeof(hu_graph_relation_t));
}

hu_error_t hu_graph_set_relation_confidence(hu_graph_t *g, int64_t relation_id,
                                            float confidence, int64_t last_seen_now_ms) {
    if (!g || !g->db)
        return HU_ERR_INVALID_ARGUMENT;
    if (relation_id <= 0)
        return HU_OK;
    if (confidence < 0.0f) confidence = 0.0f;
    if (confidence > 1.0f) confidence = 1.0f;
    sqlite3_stmt *stmt = NULL;
    /* W8 P2A — keep the legacy `confidence` column in sync with the
     * new `confidence_mean`. Variance is intentionally NOT touched
     * here: callers who do a Bayesian update should use
     * hu_graph_set_relation_belief() (added below) to write the full
     * posterior. The scalar setter assumes "deterministic update",
     * which is variance = 0. */
    const char *sql =
        "UPDATE relations SET confidence = ?, confidence_mean = ?, "
        "confidence_variance = 0.0, last_seen = "
        "CASE WHEN ? > 0 THEN ? ELSE last_seen END WHERE id = ?";
    int rc = sqlite3_prepare_v2(g->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_double(stmt, 1, (double)confidence);
    sqlite3_bind_double(stmt, 2, (double)confidence);
    sqlite3_bind_int64(stmt, 3, last_seen_now_ms);
    sqlite3_bind_int64(stmt, 4, last_seen_now_ms);
    sqlite3_bind_int64(stmt, 5, relation_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_IO;
    /* P2 #7 — confidence change is a write; invalidate any cached
     * world-model that depends on this relation. We don't have the
     * contact_id at this granularity (it's an indexed by relation_id),
     * so wildcard-invalidate. The cache is small (HU_WM_CACHE_SLOTS),
     * the cost of dropping all slots is bounded. */
    hu_world_model_invalidate(NULL, 0);
    return HU_OK;
}

hu_error_t hu_graph_set_relation_belief(hu_graph_t *g, int64_t relation_id,
                                        float mean, float variance,
                                        int64_t last_seen_now_ms) {
    if (!g || !g->db)
        return HU_ERR_INVALID_ARGUMENT;
    if (relation_id <= 0)
        return HU_OK;
    if (mean < 0.0f) mean = 0.0f;
    if (mean > 1.0f) mean = 1.0f;
    if (variance < 0.0f) variance = 0.0f;
    if (variance > 0.25f) variance = 0.25f;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "UPDATE relations SET confidence = ?, confidence_mean = ?, "
        "confidence_variance = ?, last_seen = "
        "CASE WHEN ? > 0 THEN ? ELSE last_seen END WHERE id = ?";
    int rc = sqlite3_prepare_v2(g->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_double(stmt, 1, (double)mean);
    sqlite3_bind_double(stmt, 2, (double)mean);
    sqlite3_bind_double(stmt, 3, (double)variance);
    sqlite3_bind_int64(stmt, 4, last_seen_now_ms);
    sqlite3_bind_int64(stmt, 5, last_seen_now_ms);
    sqlite3_bind_int64(stmt, 6, relation_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_IO;
    hu_world_model_invalidate(NULL, 0);
    return HU_OK;
}

hu_error_t hu_graph_get_relation_belief(hu_graph_t *g, int64_t relation_id,
                                        float *out_mean, float *out_variance) {
    if (!g || !g->db || relation_id <= 0 || (!out_mean && !out_variance))
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        g->db, "SELECT confidence_mean, confidence_variance FROM relations WHERE id = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_int64(stmt, 1, relation_id);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        if (out_mean)     *out_mean     = (float)sqlite3_column_double(stmt, 0);
        if (out_variance) *out_variance = (float)sqlite3_column_double(stmt, 1);
        sqlite3_finalize(stmt);
        return HU_OK;
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? HU_ERR_NOT_FOUND : HU_ERR_IO;
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
hu_error_t hu_graph_add_temporal_event(hu_graph_t *g, const char *contact_id,
                                       size_t contact_id_len, int64_t entity_id,
                                       const char *description, size_t desc_len,
                                       int64_t occurred_at, int64_t duration_sec) {
    if (!g || !g->db || !description || desc_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    const char *cid = contact_id ? contact_id : "";
    int cid_len = contact_id ? (int)contact_id_len : 0;
    const char *sql =
        "INSERT INTO temporal_events(contact_id, entity_id, description, occurred_at, duration_sec)"
        " VALUES(?, ?, ?, ?, ?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(stmt, 1, cid, cid_len, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, entity_id);
    sqlite3_bind_text(stmt, 3, description, (int)desc_len, NULL);
    sqlite3_bind_int64(stmt, 4, occurred_at);
    sqlite3_bind_int64(stmt, 5, duration_sec);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_IO;
}

hu_error_t hu_graph_query_temporal(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                   size_t contact_id_len, int64_t from_ts, int64_t to_ts,
                                   size_t limit, char **out, size_t *out_len) {
    if (!g || !g->db || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (limit == 0)
        limit = 50;
    const char *cid = contact_id ? contact_id : "";
    int cid_len = contact_id ? (int)contact_id_len : 0;
    const char *sql = "SELECT e.name, t.description, t.occurred_at, t.duration_sec "
                      "FROM temporal_events t JOIN entities e ON e.id = t.entity_id "
                      "WHERE t.contact_id = ? AND t.occurred_at BETWEEN ? AND ?"
                      " ORDER BY t.occurred_at DESC LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(stmt, 1, cid, cid_len, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, from_ts);
    sqlite3_bind_int64(stmt, 3, to_ts);
    sqlite3_bind_int(stmt, 4, (int)limit);
    size_t cap = 2048;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t pos = 0;
    size_t rem = cap;
    int w = snprintf(buf, rem, "### Timeline\n");
    if (w <= 0) {
        sqlite3_finalize(stmt);
        alloc->free(alloc->ctx, buf, cap);
        return HU_ERR_IO;
    }
    pos = ((size_t)w < rem) ? (size_t)w : rem - 1;
    while (sqlite3_step(stmt) == SQLITE_ROW && pos < cap - 128) {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        const char *desc = (const char *)sqlite3_column_text(stmt, 1);
        int64_t ts = sqlite3_column_int64(stmt, 2);
        if (!name || !desc)
            continue;
        rem = cap - pos;
        w = snprintf(buf + pos, rem, "- [%lld] %s: %s\n", (long long)ts, name, desc);
        if (w <= 0)
            break;
        size_t written = ((size_t)w < rem) ? (size_t)w : rem - 1;
        pos += written;
    }
    sqlite3_finalize(stmt);
    buf[pos] = '\0';
    *out = buf;
    *out_len = pos;
    return HU_OK;
}

hu_error_t hu_graph_add_causal_link(hu_graph_t *g, const char *contact_id, size_t contact_id_len,
                                    int64_t action_entity_id, int64_t outcome_entity_id,
                                    const char *context, size_t context_len, float confidence) {
    if (!g || !g->db)
        return HU_ERR_INVALID_ARGUMENT;
    const char *cid = contact_id ? contact_id : "";
    int cid_len = contact_id ? (int)contact_id_len : 0;
    const char *sql =
        "INSERT OR REPLACE INTO causal_links(contact_id, action_entity_id, outcome_entity_id,"
        " context, confidence, created_at) VALUES(?, ?, ?, ?, ?, ?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(stmt, 1, cid, cid_len, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, action_entity_id);
    sqlite3_bind_int64(stmt, 3, outcome_entity_id);
    if (context && context_len > 0)
        sqlite3_bind_text(stmt, 4, context, (int)context_len, NULL);
    else
        sqlite3_bind_null(stmt, 4);
    sqlite3_bind_double(stmt, 5, (double)confidence);
    sqlite3_bind_int64(stmt, 6, (int64_t)time(NULL));
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_IO;
}

hu_error_t hu_graph_query_causal(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                 size_t contact_id_len, int64_t entity_id, size_t max_results,
                                 char **out, size_t *out_len) {
    if (!g || !g->db || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (max_results == 0)
        max_results = 20;
    const char *cid = contact_id ? contact_id : "";
    int cid_len = contact_id ? (int)contact_id_len : 0;
    const char *sql = "SELECT a.name, o.name, c.context, c.confidence "
                      "FROM causal_links c "
                      "JOIN entities a ON a.id = c.action_entity_id "
                      "JOIN entities o ON o.id = c.outcome_entity_id "
                      "WHERE c.contact_id = ? AND"
                      " (c.action_entity_id = ? OR c.outcome_entity_id = ?)"
                      " ORDER BY c.confidence DESC LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(stmt, 1, cid, cid_len, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, entity_id);
    sqlite3_bind_int64(stmt, 3, entity_id);
    sqlite3_bind_int(stmt, 4, (int)max_results);
    size_t cap = 2048;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t pos = 0;
    size_t rem = cap;
    int w = snprintf(buf, rem, "### Cause-Effect\n");
    if (w <= 0) {
        sqlite3_finalize(stmt);
        alloc->free(alloc->ctx, buf, cap);
        return HU_ERR_IO;
    }
    pos = ((size_t)w < rem) ? (size_t)w : rem - 1;
    while (sqlite3_step(stmt) == SQLITE_ROW && pos < cap - 128) {
        const char *action = (const char *)sqlite3_column_text(stmt, 0);
        const char *outcome = (const char *)sqlite3_column_text(stmt, 1);
        const char *ctx = (const char *)sqlite3_column_text(stmt, 2);
        double conf = sqlite3_column_double(stmt, 3);
        if (!action || !outcome)
            continue;
        rem = cap - pos;
        w = snprintf(buf + pos, rem, "- %s -> %s (%.0f%%", action, outcome, conf * 100);
        if (w <= 0)
            break;
        size_t written = ((size_t)w < rem) ? (size_t)w : rem - 1;
        pos += written;
        if (ctx && pos < cap - 64) {
            rem = cap - pos;
            w = snprintf(buf + pos, rem, ", %s", ctx);
            if (w <= 0)
                break;
            written = ((size_t)w < rem) ? (size_t)w : rem - 1;
            pos += written;
        }
        if (pos < cap - 4) {
            rem = cap - pos;
            w = snprintf(buf + pos, rem, ")\n");
            if (w <= 0)
                break;
            written = ((size_t)w < rem) ? (size_t)w : rem - 1;
            pos += written;
        }
    }
    sqlite3_finalize(stmt);
    buf[pos] = '\0';
    *out = buf;
    *out_len = pos;
    return HU_OK;
}

#else

hu_error_t hu_graph_query_temporal(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                   size_t contact_id_len, int64_t from_ts, int64_t to_ts,
                                   size_t limit, char **out, size_t *out_len) {
    (void)g;
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
    (void)from_ts;
    (void)to_ts;
    (void)limit;
    (void)out;
    (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_graph_query_causal(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                 size_t contact_id_len, int64_t entity_id, size_t max_results,
                                 char **out, size_t *out_len) {
    (void)g;
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
    (void)entity_id;
    (void)max_results;
    (void)out;
    (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
}

#endif /* HU_ENABLE_SQLITE */

/* ── Phase 3b: Leiden-inspired hierarchical community detection ─────── */

#ifdef HU_ENABLE_SQLITE
hu_error_t hu_graph_leiden_communities(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                       size_t contact_id_len, size_t max_communities,
                                       size_t max_iterations, char **out, size_t *out_len) {
    if (!g || !g->db || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (max_communities == 0)
        max_communities = 20;
    if (max_iterations == 0)
        max_iterations = 10;

    const char *cid = contact_id ? contact_id : "";
    int cid_len = contact_id ? (int)contact_id_len : 0;

    const char *count_sql = "SELECT COUNT(*) FROM entities WHERE contact_id = ?";
    sqlite3_stmt *cnt_stmt = NULL;
    if (sqlite3_prepare_v2(g->db, count_sql, -1, &cnt_stmt, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(cnt_stmt, 1, cid, cid_len, SQLITE_STATIC);
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

    const char *id_sql = "SELECT id FROM entities WHERE contact_id = ?"
                         " ORDER BY mention_count DESC LIMIT ?";
    sqlite3_stmt *id_stmt = NULL;
    if (sqlite3_prepare_v2(g->db, id_sql, -1, &id_stmt, NULL) != SQLITE_OK) {
        alloc->free(alloc->ctx, ids, entity_count * sizeof(int64_t));
        alloc->free(alloc->ctx, labels, entity_count * sizeof(int32_t));
        return HU_ERR_IO;
    }
    sqlite3_bind_text(id_stmt, 1, cid, cid_len, SQLITE_STATIC);
    sqlite3_bind_int(id_stmt, 2, (int)entity_count);
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
            int up_rc = sqlite3_step(up_stmt);
            if (up_rc != SQLITE_DONE) {
                hu_log_error("graph", NULL, "leiden persist community_id for entity %lld: %s",
                             (long long)ids[i], sqlite3_errmsg(g->db));
                break;
            }
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
hu_error_t hu_graph_record_recall(hu_graph_t *g, const char *contact_id, size_t contact_id_len,
                                  int64_t entity_id) {
    if (!g || !g->db)
        return HU_ERR_INVALID_ARGUMENT;
    const char *cid = contact_id ? contact_id : "";
    size_t cid_len = contact_id ? contact_id_len : 0;
    const char *sql = "UPDATE entities SET recall_count = recall_count + 1,"
                      " last_recalled = ? WHERE id = ? AND contact_id = ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_int64(stmt, 1, (int64_t)time(NULL));
    sqlite3_bind_int64(stmt, 2, entity_id);
    sqlite3_bind_text(stmt, 3, cid, (int)cid_len, NULL);
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
bool hu_graph_detect_conflict(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                              size_t contact_id_len, const char *entity_name, size_t name_len,
                              const char *new_context, size_t new_context_len) {
    if (!g || !g->db || !alloc || !entity_name || !new_context)
        return false;
    const char *cid = contact_id ? contact_id : "";
    int cid_len = contact_id ? (int)contact_id_len : 0;
    const char *sql = "SELECT r.context FROM relations r "
                      "JOIN entities e ON (e.id = r.source_id OR e.id = r.target_id) "
                      "WHERE e.contact_id = ? AND e.name = ? AND r.context IS NOT NULL LIMIT 10";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, cid, cid_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, entity_name, (int)name_len, NULL);
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

hu_error_t hu_graph_reconsolidate(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                  size_t contact_id_len, const char *entity_name, size_t name_len,
                                  const char *new_context, size_t new_context_len) {
    if (!g || !g->db || !alloc || !entity_name || !new_context)
        return HU_ERR_INVALID_ARGUMENT;

    hu_graph_entity_t entity;
    hu_error_t err = hu_graph_find_entity(g, contact_id, contact_id_len, entity_name, name_len,
                                          &entity);
    if (err != HU_OK)
        return err;

    bool has_conflict = hu_graph_detect_conflict(g, alloc, contact_id, contact_id_len, entity_name,
                                                  name_len, new_context, new_context_len);
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
                    hu_log_error("graph", NULL, "supersede entity %lld: step failed",
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
            hu_log_error("graph", NULL, "update relations for entity %lld: step failed",
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
