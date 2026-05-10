#include "human/agent/scheduler.h"

#include "human/memory/graph.h"
#include "human/memory/memory.h"

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
struct sqlite3 *hu_graph__db_handle(hu_graph_t *g);
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* W14 counterfactual rehearsal runner.
 *
 * For each recent reasoning trace from W10 with outcome='ok', look for
 * any new fact in `relations` that arrived AFTER the trace was recorded
 * AND shares the trace's contact_id, and write a deterministic
 * placeholder diff_text "would have mentioned: <relation excerpt>"
 * into `counterfactual_replays`.
 *
 * The placeholder text format is intentionally fixed so tests can
 * regex-check it; a richer NLG is a follow-up.
 *
 * Per the W14 spec the runner caps at HU_COUNTERFACTUAL_PER_TICK
 * rehearsals per tick — preventing the runner from devouring any
 * remaining tick budget on a long-running graph. */

#define HU_COUNTERFACTUAL_PER_TICK 5

#ifdef HU_ENABLE_SQLITE

static int counterfactual_run_ddl(struct sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK)
        return rc;
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE || rc == SQLITE_OK) ? SQLITE_OK : rc;
}

static hu_error_t ensure_counterfactual_schema(struct sqlite3 *db) {
    static const char *const stmts[] = {
        "CREATE TABLE IF NOT EXISTS counterfactual_replays ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "contact_id TEXT NOT NULL DEFAULT '',"
        "trace_id INTEGER NOT NULL,"
        "new_fact_relation_id INTEGER NOT NULL,"
        "diff_text TEXT NOT NULL,"
        "confidence_mean REAL NOT NULL DEFAULT 0.5,"
        "rehearsed_at INTEGER NOT NULL)",

        "CREATE INDEX IF NOT EXISTS idx_counterfactual_trace "
        "ON counterfactual_replays(trace_id)",

        NULL,
    };
    for (size_t i = 0; stmts[i]; i++) {
        if (counterfactual_run_ddl(db, stmts[i]) != SQLITE_OK)
            return HU_ERR_IO;
    }
    return HU_OK;
}

hu_error_t hu_counterfactual_rehearsal_runner(hu_memory_facade_t *m, const hu_job_spec_t *spec,
                                              int64_t budget_ms, void *user_data) {
    (void)spec;
    (void)budget_ms;
    (void)user_data;
    if (!m)
        return HU_ERR_INVALID_ARGUMENT;
    hu_graph_t *g = hu_memory_facade_graph_handle(m);
    if (!g)
        return HU_OK;
    struct sqlite3 *db = hu_graph__db_handle(g);
    if (!db)
        return HU_OK;

    if (ensure_counterfactual_schema(db) != HU_OK)
        return HU_ERR_IO;

    /* Pull up to N candidate (trace, relation) pairs in a single join.
     * Ordering by trace_id then relation_id keeps the output stable
     * across runs so tests can pin the resulting row count without
     * caring about insertion order. */
    static const char *const sel_sql =
        "SELECT t.id, t.contact_id, t.recorded_at, t.confidence_mean, "
        "       r.id, r.relation_type, r.context "
        "FROM neural_reasoning_traces t "
        "JOIN relations r ON r.contact_id = t.contact_id "
        "WHERE t.outcome = 'ok' "
        "  AND r.last_seen > t.recorded_at "
        "  AND NOT EXISTS ("
        "      SELECT 1 FROM counterfactual_replays c "
        "      WHERE c.trace_id = t.id AND c.new_fact_relation_id = r.id) "
        "ORDER BY t.id ASC, r.id ASC "
        "LIMIT ?";
    sqlite3_stmt *sel = NULL;
    if (sqlite3_prepare_v2(db, sel_sql, -1, &sel, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_int(sel, 1, HU_COUNTERFACTUAL_PER_TICK);

    /* Buffer rows out of the SELECT so the INSERT statements below can
     * reuse the connection without stepping on the open cursor. */
    typedef struct {
        int64_t trace_id;
        int64_t relation_id;
        int relation_type;
        double confidence;
        char contact_id[128];
        char context[256];
    } cand_t;
    cand_t cands[HU_COUNTERFACTUAL_PER_TICK];
    size_t ncands = 0;
    while (sqlite3_step(sel) == SQLITE_ROW && ncands < HU_COUNTERFACTUAL_PER_TICK) {
        cand_t *c = &cands[ncands++];
        c->trace_id = sqlite3_column_int64(sel, 0);
        const unsigned char *cid = sqlite3_column_text(sel, 1);
        snprintf(c->contact_id, sizeof(c->contact_id), "%s", cid ? (const char *)cid : "");
        /* recorded_at and confidence carried only for the diff payload. */
        (void)sqlite3_column_int64(sel, 2);
        c->confidence = sqlite3_column_double(sel, 3);
        c->relation_id = sqlite3_column_int64(sel, 4);
        c->relation_type = sqlite3_column_int(sel, 5);
        const unsigned char *ctx = sqlite3_column_text(sel, 6);
        snprintf(c->context, sizeof(c->context), "%s", ctx ? (const char *)ctx : "");
    }
    sqlite3_finalize(sel);

    int64_t now_ms = (int64_t)time(NULL) * 1000;

    static const char *const ins_sql =
        "INSERT INTO counterfactual_replays "
        "(contact_id, trace_id, new_fact_relation_id, diff_text, "
        " confidence_mean, rehearsed_at) "
        "VALUES (?, ?, ?, ?, ?, ?)";
    sqlite3_stmt *ins = NULL;
    if (sqlite3_prepare_v2(db, ins_sql, -1, &ins, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    char diff_text[512];
    for (size_t i = 0; i < ncands; i++) {
        cand_t *c = &cands[i];
        snprintf(diff_text, sizeof(diff_text),
                 "would have mentioned: relation_type=%d context=\"%s\"",
                 c->relation_type, c->context[0] ? c->context : "(none)");

        sqlite3_reset(ins);
        sqlite3_clear_bindings(ins);
        sqlite3_bind_text(ins, 1, c->contact_id, -1, SQLITE_STATIC);
        sqlite3_bind_int64(ins, 2, c->trace_id);
        sqlite3_bind_int64(ins, 3, c->relation_id);
        sqlite3_bind_text(ins, 4, diff_text, -1, SQLITE_STATIC);
        sqlite3_bind_double(ins, 5, c->confidence > 0.0 ? c->confidence : 0.5);
        sqlite3_bind_int64(ins, 6, now_ms);
        int rc = sqlite3_step(ins);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(ins);
            return HU_ERR_IO;
        }
    }
    sqlite3_finalize(ins);
    return HU_OK;
}

#else /* !HU_ENABLE_SQLITE */

hu_error_t hu_counterfactual_rehearsal_runner(hu_memory_facade_t *m, const hu_job_spec_t *spec,
                                              int64_t budget_ms, void *user_data) {
    (void)m;
    (void)spec;
    (void)budget_ms;
    (void)user_data;
    return HU_ERR_NOT_SUPPORTED;
}

#endif /* HU_ENABLE_SQLITE */
