#include "human/memory/erasure.h"

#ifdef HU_ENABLE_SQLITE
#include "human/memory/sql_transaction.h"
#include <sqlite3.h>
#endif

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

/* Run a single DELETE statement bound with the given int64 parameter. Returns
 * the number of rows changed (best-effort). */
static int delete_with_int(struct sqlite3 *db, const char *sql, int64_t v) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int64(st, 1, v);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return 0;
    }
    sqlite3_finalize(st);
    return sqlite3_changes(db);
}

/* Whether a table exists. We do this gracefully because community_summaries
 * and case_records are created lazily by their respective modules. */
static bool table_exists(struct sqlite3 *db, const char *name) {
    sqlite3_stmt *st = NULL;
    bool found = false;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        found = sqlite3_step(st) == SQLITE_ROW;
        sqlite3_finalize(st);
    }
    return found;
}

hu_error_t hu_memory_erase_entity(hu_graph_t *graph, int64_t entity_id,
                                  hu_erase_report_t *out_report) {
    if (!graph || !out_report || entity_id <= 0)
        return HU_ERR_INVALID_ARGUMENT;
    struct sqlite3 *db = hu_graph_sqlite_connection(graph);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out_report, 0, sizeof(*out_report));
    out_report->entity_id = entity_id;

    /* Quick existence check for clearer caller error path. */
    sqlite3_stmt *exist = NULL;
    bool exists = false;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM entities WHERE id = ?", -1, &exist, NULL) ==
        SQLITE_OK) {
        sqlite3_bind_int64(exist, 1, entity_id);
        exists = sqlite3_step(exist) == SQLITE_ROW;
        sqlite3_finalize(exist);
    }
    if (!exists)
        return HU_ERR_NOT_FOUND;

    hu_sql_txn_t txn = {0};
    if (hu_sql_txn_begin(&txn, db) != HU_OK)
        return HU_ERR_IO;

    /* Live relations both directions. Both calls are idempotent. */
    out_report->relations_deleted +=
        (size_t)delete_with_int(db, "DELETE FROM relations WHERE source_id = ?", entity_id);
    out_report->relations_deleted +=
        (size_t)delete_with_int(db, "DELETE FROM relations WHERE target_id = ?", entity_id);

    /* Cross-graph edges that reference this entity in the entity subgraph. */
    if (table_exists(db, "cross_edges")) {
        out_report->cross_edges_deleted += (size_t)delete_with_int(
            db, "DELETE FROM cross_edges WHERE src_graph = 'entity' AND src_id = ?", entity_id);
        out_report->cross_edges_deleted += (size_t)delete_with_int(
            db, "DELETE FROM cross_edges WHERE dst_graph = 'entity' AND dst_id = ?", entity_id);
    }

    /* Quarantine. */
    if (table_exists(db, "quarantine_relations")) {
        out_report->quarantine_deleted += (size_t)delete_with_int(
            db, "DELETE FROM quarantine_relations WHERE source_id = ?", entity_id);
        out_report->quarantine_deleted += (size_t)delete_with_int(
            db, "DELETE FROM quarantine_relations WHERE target_id = ?", entity_id);
    }

    /* Case records: the anchors are stored as a comma-joined string. We can't
     * cheaply LIKE-match a number without false positives ("10" matching
     * "100"), so we fetch and filter. Bounded by a sane cap. */
    if (table_exists(db, "case_records")) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, "SELECT id, anchor_entity_ids FROM case_records LIMIT 4096",
                               -1, &st, NULL) == SQLITE_OK) {
            char target[32];
            snprintf(target, sizeof(target), "%lld", (long long)entity_id);
            size_t tlen = strlen(target);
            int64_t to_delete[256];
            size_t ndel = 0;
            while (sqlite3_step(st) == SQLITE_ROW && ndel < 256) {
                const char *anchors = (const char *)sqlite3_column_text(st, 1);
                if (!anchors)
                    continue;
                /* Token-aware match: bracketed by commas or string boundary. */
                bool match = false;
                size_t alen = strlen(anchors);
                for (size_t i = 0; i + tlen <= alen; i++) {
                    if (strncmp(anchors + i, target, tlen) != 0)
                        continue;
                    bool left_ok = (i == 0 || anchors[i - 1] == ',');
                    bool right_ok =
                        (i + tlen == alen || anchors[i + tlen] == ',');
                    if (left_ok && right_ok) {
                        match = true;
                        break;
                    }
                }
                if (match)
                    to_delete[ndel++] = sqlite3_column_int64(st, 0);
            }
            sqlite3_finalize(st);
            for (size_t i = 0; i < ndel; i++)
                out_report->case_records_deleted += (size_t)delete_with_int(
                    db, "DELETE FROM case_records WHERE id = ?", to_delete[i]);
        }
    }

    /* Community summaries: invalidate (delete) summaries whose mentioned
     * entities included this one. Stale summaries get regenerated on the next
     * AutoDream cycle. Best-effort: we delete summaries that reference the
     * entity's name. */
    if (table_exists(db, "community_summaries")) {
        sqlite3_stmt *st = NULL;
        char name_buf[128] = {0};
        if (sqlite3_prepare_v2(db, "SELECT name FROM entities WHERE id = ?", -1, &st, NULL) ==
            SQLITE_OK) {
            sqlite3_bind_int64(st, 1, entity_id);
            if (sqlite3_step(st) == SQLITE_ROW) {
                const char *nm = (const char *)sqlite3_column_text(st, 0);
                if (nm)
                    snprintf(name_buf, sizeof(name_buf), "%s", nm);
            }
            sqlite3_finalize(st);
        }
        if (name_buf[0]) {
            sqlite3_stmt *del = NULL;
            char like[160];
            snprintf(like, sizeof(like), "%%%s%%", name_buf);
            if (sqlite3_prepare_v2(db,
                                   "DELETE FROM community_summaries WHERE summary_text LIKE ?",
                                   -1, &del, NULL) == SQLITE_OK) {
                sqlite3_bind_text(del, 1, like, -1, SQLITE_STATIC);
                if (sqlite3_step(del) == SQLITE_DONE)
                    out_report->community_summaries_invalidated =
                        (size_t)sqlite3_changes(db);
                sqlite3_finalize(del);
            }
        }
    }

    /* Finally remove the entity itself. */
    int n = delete_with_int(db, "DELETE FROM entities WHERE id = ?", entity_id);
    out_report->entity_deleted = n > 0;

    return hu_sql_txn_commit(&txn);
}

hu_error_t hu_memory_erase_by_provenance(hu_graph_t *graph, const char *provenance_substring,
                                         size_t substring_len, hu_erase_report_t *out_report) {
    if (!graph || !provenance_substring || substring_len == 0 || !out_report)
        return HU_ERR_INVALID_ARGUMENT;
    struct sqlite3 *db = hu_graph_sqlite_connection(graph);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out_report, 0, sizeof(*out_report));

    char like[256];
    int written = snprintf(like, sizeof(like), "%%%.*s%%",
                           (int)(substring_len < 240 ? substring_len : 240),
                           provenance_substring);
    if (written < 0)
        return HU_ERR_INVALID_ARGUMENT;

    hu_sql_txn_t txn = {0};
    if (hu_sql_txn_begin(&txn, db) != HU_OK)
        return HU_ERR_IO;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM relations WHERE provenance LIKE ?", -1, &st, NULL) ==
        SQLITE_OK) {
        sqlite3_bind_text(st, 1, like, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_DONE)
            out_report->relations_deleted = (size_t)sqlite3_changes(db);
        sqlite3_finalize(st);
    }
    if (table_exists(db, "quarantine_relations")) {
        sqlite3_stmt *q = NULL;
        if (sqlite3_prepare_v2(db, "DELETE FROM quarantine_relations WHERE provenance LIKE ?", -1,
                               &q, NULL) == SQLITE_OK) {
            sqlite3_bind_text(q, 1, like, -1, SQLITE_STATIC);
            if (sqlite3_step(q) == SQLITE_DONE)
                out_report->quarantine_deleted = (size_t)sqlite3_changes(db);
            sqlite3_finalize(q);
        }
    }
    return hu_sql_txn_commit(&txn);
}

#else /* !HU_ENABLE_SQLITE */

hu_error_t hu_memory_erase_entity(hu_graph_t *graph, int64_t entity_id,
                                  hu_erase_report_t *out_report) {
    (void)graph;
    (void)entity_id;
    (void)out_report;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_memory_erase_by_provenance(hu_graph_t *graph, const char *provenance_substring,
                                         size_t substring_len, hu_erase_report_t *out_report) {
    (void)graph;
    (void)provenance_substring;
    (void)substring_len;
    (void)out_report;
    return HU_ERR_NOT_SUPPORTED;
}

#endif
