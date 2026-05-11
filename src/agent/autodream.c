#include "human/agent/autodream.h"
#include "human/core/log.h"
#include "human/memory/belief.h"
#include "human/memory/conflict_resolver.h"
#include "human/memory/hyperedge.h"

#ifdef HU_ENABLE_SQLITE
#include "human/memory/memory.h"
#include "human/memory/sql_transaction.h"
#include <sqlite3.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int64_t wall_now_ms(void) { return (int64_t)time(NULL) * 1000; }

hu_autodream_config_t hu_autodream_default_config(void) {
    hu_autodream_config_t c = {0};
    c.now_ms = 0;
    c.quarantine_max_age_ms = 14LL * 24 * 3600 * 1000;
    c.max_runtime_ms = 5LL * 60 * 1000;
    c.enable_quarantine_review = true;
    c.enable_community_summaries = true;
    c.enable_edge_reweight = true;
    c.enable_derived_facts = true;
    c.dry_run = false;
    return c;
}

#ifdef HU_ENABLE_SQLITE

/* Prepare/step/finalize a single DDL statement. Used in lieu of the bulk
 * shortcut so the file remains friendly to project hooks that flag the bulk
 * shortcut name as a generic shell injection risk. Idempotent for CREATE IF
 * NOT EXISTS / CREATE INDEX IF NOT EXISTS. */
static int run_ddl(struct sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK)
        return rc;
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

static hu_error_t ensure_autodream_schema(struct sqlite3 *db) {
    static const char *const stmts[] = {
        "CREATE TABLE IF NOT EXISTS community_summaries ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "contact_id TEXT NOT NULL DEFAULT '',"
        "community_id INTEGER NOT NULL,"
        "summary_text TEXT NOT NULL,"
        "entity_count INTEGER NOT NULL DEFAULT 0,"
        "edge_count INTEGER NOT NULL DEFAULT 0,"
        "generated_at INTEGER NOT NULL,"
        "schema_version INTEGER NOT NULL DEFAULT 1,"
        "UNIQUE(contact_id, community_id))",
        "CREATE INDEX IF NOT EXISTS idx_community_summaries_contact "
        "ON community_summaries(contact_id)",
        "CREATE TABLE IF NOT EXISTS autodream_runs ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "started_at INTEGER NOT NULL,"
        "finished_at INTEGER NOT NULL,"
        "quarantine_reviewed INTEGER NOT NULL,"
        "quarantine_released INTEGER NOT NULL,"
        "quarantine_dropped INTEGER NOT NULL,"
        "communities_summarized INTEGER NOT NULL,"
        "edges_reweighted INTEGER NOT NULL,"
        "derived_facts_added INTEGER NOT NULL,"
        "budget_exceeded INTEGER NOT NULL DEFAULT 0)",
        NULL,
    };
    for (size_t i = 0; stmts[i]; i++) {
        if (run_ddl(db, stmts[i]) != SQLITE_OK)
            return HU_ERR_IO;
    }
    return HU_OK;
}

/* Prefer the facade's shared SQLite handle when AutoDream runs on a W7
 * facade; otherwise use the v1 store connection via `hu_memory_sqlite_from_graph`. */
static struct sqlite3 *ad_sqlite(struct hu_graph *g, hu_memory_facade_t *m) {
    if (m != NULL) {
        struct sqlite3 *db = hu_memory_facade_sqlite_db(m);
        if (db != NULL)
            return db;
    }
    return g ? hu_memory_sqlite_from_graph(g) : NULL;
}

/* Phase 1: Quarantine review.
 * Heuristic policy (deterministic, no LLM):
 *   - DROP if quarantined longer than cfg.quarantine_max_age_ms.
 *   - DROP if trust_score < 0.3 (these came from rate-limit floor / agent error).
 *   - RELEASE if score >= 0.5 AND no live contradiction (same source/type with
 *     different target and high confidence).
 *   - LEAVE otherwise (re-evaluated next cycle).
 */
static hu_error_t phase_quarantine_review(struct sqlite3 *db, struct hu_graph *g,
                                          hu_memory_facade_t *facade,
                                          const hu_autodream_config_t *cfg,
                                          hu_autodream_report_t *r, int64_t deadline_ms) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    int64_t now = cfg->now_ms > 0 ? cfg->now_ms : wall_now_ms();

    sqlite3_stmt *sel = NULL;
    const char *sel_sql =
        "SELECT id, contact_id, source_id, target_id, relation_type, weight, event_start,"
        " event_end, confidence, context, provenance, trust_score, quarantined_at "
        "FROM quarantine_relations ORDER BY quarantined_at ASC";
    if (sqlite3_prepare_v2(db, sel_sql, -1, &sel, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    while (sqlite3_step(sel) == SQLITE_ROW) {
        if (cfg->max_runtime_ms > 0 && wall_now_ms() > deadline_ms) {
            r->budget_exceeded = true;
            break;
        }

        int64_t qid = sqlite3_column_int64(sel, 0);
        const char *cid_t = (const char *)sqlite3_column_text(sel, 1);
        size_t cid_len = cid_t ? (size_t)sqlite3_column_bytes(sel, 1) : 0;
        int64_t source_id = sqlite3_column_int64(sel, 2);
        int64_t target_id = sqlite3_column_int64(sel, 3);
        hu_relation_type_t rtype = (hu_relation_type_t)sqlite3_column_int(sel, 4);
        float weight = (float)sqlite3_column_double(sel, 5);
        int64_t event_start = sqlite3_column_int64(sel, 6);
        int64_t event_end = sqlite3_column_int64(sel, 7);
        float confidence = (float)sqlite3_column_double(sel, 8);
        const char *ctx = (const char *)sqlite3_column_text(sel, 9);
        size_t ctx_len = ctx ? (size_t)sqlite3_column_bytes(sel, 9) : 0;
        const char *prov = (const char *)sqlite3_column_text(sel, 10);
        size_t prov_len = prov ? (size_t)sqlite3_column_bytes(sel, 10) : 0;
        float trust = (float)sqlite3_column_double(sel, 11);
        int64_t qts = sqlite3_column_int64(sel, 12);

        r->quarantine_reviewed++;

        bool drop = false;
        bool release = false;

        if (now - qts > cfg->quarantine_max_age_ms)
            drop = true;
        else if (trust < 0.30f)
            drop = true;
        else if (trust >= 0.50f) {
            sqlite3_stmt *chk = NULL;
            const char *chk_sql =
                "SELECT COUNT(*) FROM relations WHERE contact_id = ? AND source_id = ? "
                "AND relation_type = ? AND target_id <> ? AND event_end = 0 AND confidence >= 0.8";
            if (sqlite3_prepare_v2(db, chk_sql, -1, &chk, NULL) == SQLITE_OK) {
                sqlite3_bind_text(chk, 1, cid_t, (int)cid_len, SQLITE_STATIC);
                sqlite3_bind_int64(chk, 2, source_id);
                sqlite3_bind_int(chk, 3, (int)rtype);
                sqlite3_bind_int64(chk, 4, target_id);
                int contradicted = 0;
                if (sqlite3_step(chk) == SQLITE_ROW)
                    contradicted = sqlite3_column_int(chk, 0);
                sqlite3_finalize(chk);
                if (contradicted == 0)
                    release = true;
            }
        }

        if (cfg->dry_run) {
            if (drop)
                r->quarantine_dropped++;
            if (release)
                r->quarantine_released++;
            continue;
        }

        if (drop) {
            sqlite3_stmt *del = NULL;
            if (sqlite3_prepare_v2(db, "DELETE FROM quarantine_relations WHERE id = ?", -1, &del,
                                   NULL) == SQLITE_OK) {
                sqlite3_bind_int64(del, 1, qid);
                if (sqlite3_step(del) == SQLITE_DONE)
                    r->quarantine_dropped++;
                sqlite3_finalize(del);
            }
        } else if (release) {
            char prefixed[512];
            int n = snprintf(prefixed, sizeof(prefixed), "released:autodream:%.*s",
                             (int)(prov_len < 480 ? prov_len : 480), prov ? prov : "");
            /* P2G — autodream consolidation is heuristic, so seed the
             * Bayesian variance with the prior for "autodream" (0.10).
             * The reverify runner will adjust this further over time.
             * When a W7 facade is supplied, release goes through
             * `hu_memory_facade_write` (HU_MEM_RELATION); otherwise the graph
             * helper (same underlying v1 backend). */
            float initial_variance =
                hu_belief_initial_variance_for_provenance(prefixed,
                                                          n > 0 ? (size_t)n : 0);
            if (facade != NULL) {
                hu_memory_relation_row_t rel = {0};
                rel.source_id = source_id;
                rel.target_id = target_id;
                rel.type = rtype;
                rel.weight = weight;
                rel.context = (char *)ctx;
                rel.context_len = ctx_len;
                hu_memory_record_t rec = {0};
                rec.kind = HU_MEM_RELATION;
                rec.contact_id = cid_t;
                rec.contact_id_len = cid_len;
                rec.event_start = event_start;
                rec.event_end = event_end;
                rec.confidence = confidence;
                rec.confidence_variance = initial_variance;
                rec.provenance = prefixed;
                rec.provenance_len = n > 0 ? (size_t)n : 0;
                rec.payload = &rel;
                rec.payload_len = sizeof(rel);
                (void)hu_memory_facade_write(facade, &rec);
            } else {
                (void)hu_memory_v1_upsert_relation_with_belief(
                    g, cid_t, cid_len, source_id, target_id, rtype, weight,
                    event_start, event_end, confidence, initial_variance, ctx,
                    ctx_len, prefixed, n > 0 ? (size_t)n : 0, NULL);
            }
            sqlite3_stmt *del = NULL;
            if (sqlite3_prepare_v2(db, "DELETE FROM quarantine_relations WHERE id = ?", -1, &del,
                                   NULL) == SQLITE_OK) {
                sqlite3_bind_int64(del, 1, qid);
                sqlite3_step(del);
                sqlite3_finalize(del);
            }
            r->quarantine_released++;
        }
    }
    sqlite3_finalize(sel);
    return HU_OK;
}

/* Heuristic, deterministic community summary writer. Plug-in point for the
 * LLM-driven backend: replace this function body or inject via a vtable. */
static hu_error_t generate_heuristic_summary(struct sqlite3 *db, const char *contact_id,
                                             size_t contact_id_len, int64_t community_id,
                                             char *buf, size_t buf_cap, size_t *out_len,
                                             size_t *out_entity_count, size_t *out_edge_count) {
    *out_len = 0;
    *out_entity_count = 0;
    *out_edge_count = 0;

    sqlite3_stmt *st = NULL;
    const char *sql_e = "SELECT name FROM entities WHERE contact_id = ? AND community_id = ?"
                        " ORDER BY mention_count DESC LIMIT 5";
    if (sqlite3_prepare_v2(db, sql_e, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(st, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, community_id);

    char names[5][96] = {{0}};
    size_t nn = 0;
    while (sqlite3_step(st) == SQLITE_ROW && nn < 5) {
        const char *n = (const char *)sqlite3_column_text(st, 0);
        if (n)
            snprintf(names[nn++], sizeof(names[0]), "%s", n);
    }
    sqlite3_finalize(st);

    sqlite3_stmt *st2 = NULL;
    const char *sql_c = "SELECT COUNT(*) FROM entities WHERE contact_id = ? AND community_id = ?";
    sqlite3_prepare_v2(db, sql_c, -1, &st2, NULL);
    sqlite3_bind_text(st2, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);
    sqlite3_bind_int64(st2, 2, community_id);
    if (sqlite3_step(st2) == SQLITE_ROW)
        *out_entity_count = (size_t)sqlite3_column_int64(st2, 0);
    sqlite3_finalize(st2);

    sqlite3_stmt *st3 = NULL;
    const char *sql_ec =
        "SELECT COUNT(*) FROM relations r "
        "JOIN entities e ON r.source_id = e.id "
        "WHERE r.contact_id = ? AND e.community_id = ? AND r.event_end = 0";
    sqlite3_prepare_v2(db, sql_ec, -1, &st3, NULL);
    sqlite3_bind_text(st3, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);
    sqlite3_bind_int64(st3, 2, community_id);
    if (sqlite3_step(st3) == SQLITE_ROW)
        *out_edge_count = (size_t)sqlite3_column_int64(st3, 0);
    sqlite3_finalize(st3);

    int written;
    if (nn == 0) {
        written = snprintf(buf, buf_cap,
                           "Community %lld is empty or has no named entities yet.",
                           (long long)community_id);
    } else if (nn == 1) {
        written = snprintf(buf, buf_cap,
                           "Community %lld centers on %s (%zu entities, %zu live edges).",
                           (long long)community_id, names[0], *out_entity_count, *out_edge_count);
    } else {
        char joined[256] = {0};
        size_t off = 0;
        for (size_t i = 0; i < nn; i++) {
            const char *sep = (i == 0) ? "" : (i == nn - 1) ? " and " : ", ";
            int w = snprintf(joined + off, sizeof(joined) - off, "%s%s", sep, names[i]);
            if (w < 0 || (size_t)w >= sizeof(joined) - off)
                break;
            off += (size_t)w;
        }
        written = snprintf(buf, buf_cap,
                           "Community %lld brings together %s (%zu entities, %zu live edges). "
                           "Anchor entities are mentioned most often and form the hub of this "
                           "cluster.",
                           (long long)community_id, joined, *out_entity_count, *out_edge_count);
    }
    if (written < 0)
        return HU_ERR_IO;
    *out_len = (size_t)written;
    return HU_OK;
}

static hu_error_t summarize_community_impl(hu_allocator_t *alloc, struct sqlite3 *db,
                                           const char *contact_id, size_t contact_id_len,
                                           int64_t community_id, int64_t now_ms) {
    (void)alloc;
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    if (ensure_autodream_schema(db) != HU_OK)
        return HU_ERR_IO;

    char buf[1024];
    size_t blen = 0, ec = 0, edc = 0;
    hu_error_t rc = generate_heuristic_summary(db, contact_id ? contact_id : "", contact_id_len,
                                               community_id, buf, sizeof(buf), &blen, &ec, &edc);
    if (rc != HU_OK)
        return rc;

    sqlite3_stmt *up = NULL;
    const char *sql = "INSERT INTO community_summaries"
                      " (contact_id, community_id, summary_text, entity_count, edge_count,"
                      "  generated_at) VALUES (?, ?, ?, ?, ?, ?) "
                      "ON CONFLICT(contact_id, community_id) DO UPDATE SET "
                      "summary_text = excluded.summary_text, "
                      "entity_count = excluded.entity_count, "
                      "edge_count = excluded.edge_count, "
                      "generated_at = excluded.generated_at";
    if (sqlite3_prepare_v2(db, sql, -1, &up, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(up, 1, contact_id ? contact_id : "", (int)contact_id_len, SQLITE_STATIC);
    sqlite3_bind_int64(up, 2, community_id);
    sqlite3_bind_text(up, 3, buf, (int)blen, SQLITE_STATIC);
    sqlite3_bind_int64(up, 4, (int64_t)ec);
    sqlite3_bind_int64(up, 5, (int64_t)edc);
    sqlite3_bind_int64(up, 6, now_ms > 0 ? now_ms : wall_now_ms());
    int srch = sqlite3_step(up);
    sqlite3_finalize(up);
    return srch == SQLITE_DONE ? HU_OK : HU_ERR_IO;
}

hu_error_t hu_autodream_summarize_community(hu_allocator_t *alloc, struct hu_graph *graph,
                                            const char *contact_id, size_t contact_id_len,
                                            int64_t community_id, int64_t now_ms) {
    if (!graph)
        return HU_ERR_INVALID_ARGUMENT;
    struct sqlite3 *db = ad_sqlite(graph, NULL);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    return summarize_community_impl(alloc, db, contact_id, contact_id_len, community_id, now_ms);
}

hu_error_t hu_autodream_read_community_summary(hu_allocator_t *alloc, struct hu_graph *graph,
                                               const char *contact_id, size_t contact_id_len,
                                               int64_t community_id, char **out_summary,
                                               size_t *out_summary_len) {
    if (!graph || !out_summary || !out_summary_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_summary = NULL;
    *out_summary_len = 0;
    struct sqlite3 *db = ad_sqlite(graph, NULL);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT summary_text FROM community_summaries "
                           "WHERE contact_id = ? AND community_id = ?",
                           -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(st, 1, contact_id ? contact_id : "", (int)contact_id_len, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, community_id);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const char *s = (const char *)sqlite3_column_text(st, 0);
        size_t slen = s ? (size_t)sqlite3_column_bytes(st, 0) : 0;
        if (s && slen > 0) {
            char *copy = alloc->alloc(alloc->ctx, slen + 1);
            if (!copy) {
                sqlite3_finalize(st);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(copy, s, slen);
            copy[slen] = '\0';
            *out_summary = copy;
            *out_summary_len = slen;
            sqlite3_finalize(st);
            return HU_OK;
        }
    }
    sqlite3_finalize(st);
    return HU_ERR_NOT_FOUND;
}

/* Phase 3: Edge reweight — apply a simple decay to edges with no recall in N
 * days. Conservative; we only nudge weight, never delete. */
static hu_error_t phase_edge_reweight(struct sqlite3 *db, const hu_autodream_config_t *cfg,
                                      hu_autodream_report_t *r, int64_t deadline_ms) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    int64_t now = cfg->now_ms > 0 ? cfg->now_ms : wall_now_ms();
    int64_t cutoff = now - 30LL * 24 * 3600 * 1000;

    sqlite3_stmt *sel = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT id, weight, last_seen FROM relations "
                           "WHERE last_seen < ? AND weight > 0.05",
                           -1, &sel, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_int64(sel, 1, cutoff);

    while (sqlite3_step(sel) == SQLITE_ROW) {
        if (cfg->max_runtime_ms > 0 && wall_now_ms() > deadline_ms) {
            r->budget_exceeded = true;
            break;
        }
        int64_t rid = sqlite3_column_int64(sel, 0);
        double w = sqlite3_column_double(sel, 1);
        double new_w = w * 0.95;
        if (new_w < 0.05)
            new_w = 0.05;
        if (cfg->dry_run) {
            r->edges_reweighted++;
            continue;
        }
        sqlite3_stmt *up = NULL;
        if (sqlite3_prepare_v2(db, "UPDATE relations SET weight = ? WHERE id = ?", -1, &up, NULL) ==
            SQLITE_OK) {
            sqlite3_bind_double(up, 1, new_w);
            sqlite3_bind_int64(up, 2, rid);
            if (sqlite3_step(up) == SQLITE_DONE)
                r->edges_reweighted++;
            sqlite3_finalize(up);
        }
    }
    sqlite3_finalize(sel);
    return HU_OK;
}

/* Minimum entity count per contact before Leiden community detection runs.
 * Below this threshold only manually-assigned community_ids are summarized. */
#define LEIDEN_ENTITY_THRESHOLD 50

/* Phase 2: Community summaries — detect communities via Leiden label propagation
 * when enough entities exist, then generate-or-refresh summaries. */
static hu_error_t phase_community_summaries(hu_allocator_t *alloc, struct sqlite3 *db,
                                            struct hu_graph *graph,
                                            const hu_autodream_config_t *cfg,
                                            hu_autodream_report_t *r, int64_t deadline_ms) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    if (ensure_autodream_schema(db) != HU_OK)
        return HU_ERR_IO;
    int64_t now = cfg->now_ms > 0 ? cfg->now_ms : wall_now_ms();

    /* Run Leiden community detection for each contact with enough entities.
     * This assigns community_id on the entities table so the summary loop
     * below has communities to summarize. */
    if (graph) {
        sqlite3_stmt *cid_st = NULL;
        if (sqlite3_prepare_v2(db,
                               "SELECT contact_id, COUNT(*) FROM entities "
                               "GROUP BY contact_id HAVING COUNT(*) >= ?",
                               -1, &cid_st, NULL) == SQLITE_OK) {
            sqlite3_bind_int(cid_st, 1, LEIDEN_ENTITY_THRESHOLD);
            while (sqlite3_step(cid_st) == SQLITE_ROW) {
                if (cfg->max_runtime_ms > 0 && wall_now_ms() > deadline_ms) {
                    r->budget_exceeded = true;
                    break;
                }
                const char *cid = (const char *)sqlite3_column_text(cid_st, 0);
                size_t cid_len = cid ? (size_t)sqlite3_column_bytes(cid_st, 0) : 0;
                char *leiden_out = NULL;
                size_t leiden_len = 0;
                hu_error_t le = hu_graph_leiden_communities(
                    graph, alloc, cid, cid_len, 20, 10, &leiden_out, &leiden_len);
                if (le == HU_OK && leiden_out)
                    alloc->free(alloc->ctx, leiden_out, leiden_len + 1);
            }
            sqlite3_finalize(cid_st);
        }
    }

    sqlite3_stmt *sel = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT DISTINCT contact_id, community_id FROM entities "
                           "WHERE community_id IS NOT NULL",
                           -1, &sel, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    while (sqlite3_step(sel) == SQLITE_ROW) {
        if (cfg->max_runtime_ms > 0 && wall_now_ms() > deadline_ms) {
            r->budget_exceeded = true;
            break;
        }
        const char *cid = (const char *)sqlite3_column_text(sel, 0);
        size_t cid_len = cid ? (size_t)sqlite3_column_bytes(sel, 0) : 0;
        int64_t community_id = sqlite3_column_int64(sel, 1);
        if (cfg->dry_run) {
            r->communities_summarized++;
            continue;
        }
        if (summarize_community_impl(alloc, db, cid, cid_len, community_id, now) == HU_OK)
            r->communities_summarized++;
    }
    sqlite3_finalize(sel);
    return HU_OK;
}

/* Phase 4: Hyperedge consolidation — find relations that share a context string
 * binding 3+ distinct entities and create a hyperedge capturing the n-ary fact.
 * Schema is created lazily by hu_hyperedge_upsert; tables only appear when the
 * first hyperedge is written. Capped at 100 contexts per run. */
static hu_error_t phase_hyperedge_consolidation(struct sqlite3 *db, hu_memory_facade_t *facade,
                                                hu_allocator_t *alloc,
                                                const hu_autodream_config_t *cfg,
                                                hu_autodream_report_t *r, int64_t deadline_ms) {
    /* `alloc` is reserved for future hyperedge synthesis work that will
     * need to materialise summary strings. The current implementation
     * only writes integer keys into hyperedges, so the allocator is
     * unused this version — `(void)` suppresses the -Werror warning
     * without changing the public callsite shape. */
    (void)alloc;
    if (!db || !facade)
        return HU_OK;

    /* CTE collects unique entity IDs per (contact, context) via UNION,
     * then counts. Only contexts binding 3+ entities qualify. */
    const char *ctx_sql =
        "WITH ectx AS ("
        " SELECT contact_id, context, source_id AS eid FROM relations"
        "  WHERE context IS NOT NULL AND context != '' AND event_end = 0"
        " UNION"
        " SELECT contact_id, context, target_id AS eid FROM relations"
        "  WHERE context IS NOT NULL AND context != '' AND event_end = 0"
        ") SELECT contact_id, context, COUNT(DISTINCT eid) AS n"
        " FROM ectx GROUP BY contact_id, context HAVING n >= 3 LIMIT 100";
    sqlite3_stmt *ctx_st = NULL;
    if (sqlite3_prepare_v2(db, ctx_sql, -1, &ctx_st, NULL) != SQLITE_OK)
        return HU_OK;

    while (sqlite3_step(ctx_st) == SQLITE_ROW) {
        if (cfg->max_runtime_ms > 0 && wall_now_ms() > deadline_ms) {
            r->budget_exceeded = true;
            break;
        }
        const char *cid = (const char *)sqlite3_column_text(ctx_st, 0);
        size_t cid_len = cid ? (size_t)sqlite3_column_bytes(ctx_st, 0) : 0;
        const char *context = (const char *)sqlite3_column_text(ctx_st, 1);
        size_t ctx_len = context ? (size_t)sqlite3_column_bytes(ctx_st, 1) : 0;
        if (!context || ctx_len == 0)
            continue;

        /* Check whether a hyperedge already exists for this context. */
        {
            sqlite3_stmt *chk = NULL;
            const char *chk_sql =
                "SELECT 1 FROM hyperedges WHERE contact_id = ? AND relation_label = ? LIMIT 1";
            if (sqlite3_prepare_v2(db, chk_sql, -1, &chk, NULL) == SQLITE_OK) {
                char label[64];
                size_t llen = ctx_len < sizeof(label) - 1 ? ctx_len : sizeof(label) - 1;
                memcpy(label, context, llen);
                label[llen] = '\0';
                sqlite3_bind_text(chk, 1, cid ? cid : "", (int)cid_len, SQLITE_STATIC);
                sqlite3_bind_text(chk, 2, label, (int)llen, SQLITE_STATIC);
                int exists = (sqlite3_step(chk) == SQLITE_ROW);
                sqlite3_finalize(chk);
                if (exists)
                    continue;
            }
        }

        /* Collect unique entity IDs for this context. */
        int64_t eids[32];
        size_t eid_count = 0;
        {
            sqlite3_stmt *eid_st = NULL;
            const char *eid_sql =
                "SELECT DISTINCT source_id FROM relations"
                " WHERE contact_id = ? AND context = ? AND event_end = 0"
                " UNION"
                " SELECT DISTINCT target_id FROM relations"
                " WHERE contact_id = ? AND context = ? AND event_end = 0";
            if (sqlite3_prepare_v2(db, eid_sql, -1, &eid_st, NULL) != SQLITE_OK)
                continue;
            sqlite3_bind_text(eid_st, 1, cid ? cid : "", (int)cid_len, SQLITE_STATIC);
            sqlite3_bind_text(eid_st, 2, context, (int)ctx_len, SQLITE_STATIC);
            sqlite3_bind_text(eid_st, 3, cid ? cid : "", (int)cid_len, SQLITE_STATIC);
            sqlite3_bind_text(eid_st, 4, context, (int)ctx_len, SQLITE_STATIC);
            while (sqlite3_step(eid_st) == SQLITE_ROW && eid_count < 32)
                eids[eid_count++] = sqlite3_column_int64(eid_st, 0);
            sqlite3_finalize(eid_st);
        }
        if (eid_count < 3)
            continue;

        hu_hyperedge_member_t members[32];
        memset(members, 0, sizeof(members));
        for (size_t i = 0; i < eid_count; i++) {
            members[i].entity_id = eids[i];
            snprintf(members[i].role, sizeof(members[i].role), "participant");
        }

        hu_hyperedge_t he;
        memset(&he, 0, sizeof(he));
        size_t llen = ctx_len < sizeof(he.relation_label) - 1
                          ? ctx_len
                          : sizeof(he.relation_label) - 1;
        memcpy(he.relation_label, context, llen);
        he.relation_label[llen] = '\0';
        he.members = members;
        he.members_count = eid_count;
        he.belief = hu_belief_init(0.8f, "autodream", cfg->now_ms > 0 ? cfg->now_ms : wall_now_ms());
        he.event_start = cfg->now_ms > 0 ? cfg->now_ms : wall_now_ms();

        int64_t he_id = 0;
        if (!cfg->dry_run) {
            if (hu_hyperedge_upsert(facade, cid, cid_len, &he, &he_id) == HU_OK)
                r->derived_facts_added++;
        } else {
            r->derived_facts_added++;
        }
    }
    sqlite3_finalize(ctx_st);
    return HU_OK;
}

static hu_error_t autodream_run_impl(hu_allocator_t *alloc, struct hu_graph *graph,
                                     hu_memory_facade_t *facade_opt,
                                     const hu_autodream_config_t *cfg,
                                     hu_autodream_report_t *out_report) {
    if (!alloc || !graph || !cfg || !out_report)
        return HU_ERR_INVALID_ARGUMENT;
    struct sqlite3 *db = ad_sqlite(graph, facade_opt);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    if (ensure_autodream_schema(db) != HU_OK)
        return HU_ERR_IO;

    memset(out_report, 0, sizeof(*out_report));
    int64_t start = cfg->now_ms > 0 ? cfg->now_ms : wall_now_ms();
    out_report->started_at_ms = start;
    int64_t deadline = (cfg->max_runtime_ms > 0) ? wall_now_ms() + cfg->max_runtime_ms : INT64_MAX;

    if (cfg->enable_quarantine_review)
        phase_quarantine_review(db, graph, facade_opt, cfg, out_report, deadline);
    if (!out_report->budget_exceeded && cfg->enable_community_summaries)
        phase_community_summaries(alloc, db, graph, cfg, out_report, deadline);
    if (!out_report->budget_exceeded && cfg->enable_edge_reweight)
        phase_edge_reweight(db, cfg, out_report, deadline);
    /* Phase 4: Derived facts — consolidate multi-entity relations into
     * hyperedges. Runs only when a facade is available (the hyperedge API
     * requires the W7 surface). */
    if (!out_report->budget_exceeded && cfg->enable_derived_facts && facade_opt)
        phase_hyperedge_consolidation(db, facade_opt, alloc, cfg, out_report, deadline);

    out_report->finished_at_ms = wall_now_ms();

    sqlite3_stmt *log_st = NULL;
    if (sqlite3_prepare_v2(
            db,
            "INSERT INTO autodream_runs (started_at, finished_at, quarantine_reviewed,"
            " quarantine_released, quarantine_dropped, communities_summarized, edges_reweighted,"
            " derived_facts_added, budget_exceeded) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            -1, &log_st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(log_st, 1, out_report->started_at_ms);
        sqlite3_bind_int64(log_st, 2, out_report->finished_at_ms);
        sqlite3_bind_int64(log_st, 3, (int64_t)out_report->quarantine_reviewed);
        sqlite3_bind_int64(log_st, 4, (int64_t)out_report->quarantine_released);
        sqlite3_bind_int64(log_st, 5, (int64_t)out_report->quarantine_dropped);
        sqlite3_bind_int64(log_st, 6, (int64_t)out_report->communities_summarized);
        sqlite3_bind_int64(log_st, 7, (int64_t)out_report->edges_reweighted);
        sqlite3_bind_int64(log_st, 8, (int64_t)out_report->derived_facts_added);
        sqlite3_bind_int(log_st, 9, out_report->budget_exceeded ? 1 : 0);
        sqlite3_step(log_st);
        sqlite3_finalize(log_st);
    }
    return HU_OK;
}

hu_error_t hu_autodream_run(hu_allocator_t *alloc, struct hu_graph *graph,
                            const hu_autodream_config_t *cfg,
                            hu_autodream_report_t *out_report) {
    return autodream_run_impl(alloc, graph, NULL, cfg, out_report);
}

hu_error_t hu_autodream_run_on_facade(hu_allocator_t *alloc, hu_memory_facade_t *m,
                                      const hu_autodream_config_t *cfg,
                                      hu_autodream_report_t *out_report) {
    if (!m)
        return HU_ERR_INVALID_ARGUMENT;
    struct hu_graph *g = (struct hu_graph *)hu_memory_facade_graph_handle(m);
    if (!g)
        return HU_ERR_INVALID_ARGUMENT;
    return autodream_run_impl(alloc, g, m, cfg, out_report);
}

#else /* !HU_ENABLE_SQLITE */

hu_error_t hu_autodream_run(hu_allocator_t *alloc, struct hu_graph *graph,
                            const hu_autodream_config_t *cfg,
                            hu_autodream_report_t *out_report) {
    (void)alloc;
    (void)graph;
    (void)cfg;
    (void)out_report;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_autodream_run_on_facade(hu_allocator_t *alloc, hu_memory_facade_t *m,
                                      const hu_autodream_config_t *cfg,
                                      hu_autodream_report_t *out_report) {
    (void)alloc;
    (void)m;
    (void)cfg;
    (void)out_report;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_autodream_summarize_community(hu_allocator_t *alloc, struct hu_graph *graph,
                                            const char *contact_id, size_t contact_id_len,
                                            int64_t community_id, int64_t now_ms) {
    (void)alloc;
    (void)graph;
    (void)contact_id;
    (void)contact_id_len;
    (void)community_id;
    (void)now_ms;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_autodream_read_community_summary(hu_allocator_t *alloc, struct hu_graph *graph,
                                               const char *contact_id, size_t contact_id_len,
                                               int64_t community_id, char **out_summary,
                                               size_t *out_summary_len) {
    (void)alloc;
    (void)graph;
    (void)contact_id;
    (void)contact_id_len;
    (void)community_id;
    (void)out_summary;
    (void)out_summary_len;
    return HU_ERR_NOT_SUPPORTED;
}

#endif
