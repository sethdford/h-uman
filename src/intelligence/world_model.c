#ifdef HU_ENABLE_SQLITE

#include "human/intelligence/world_model.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <ctype.h>
#include <sqlite3.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Extract first word (keyword) from action for LIKE query */
static void extract_keyword(const char *action, size_t action_len, char *keyword_buf,
                            size_t buf_cap, size_t *out_len) {
    *out_len = 0;
    if (!action || buf_cap == 0)
        return;

    size_t i = 0;
    while (i < action_len && (action[i] == ' ' || action[i] == '\t'))
        i++;
    size_t start = i;
    while (i < action_len && action[i] != ' ' && action[i] != '\t' && action[i] != '\0')
        i++;

    size_t word_len = i - start;
    if (word_len >= buf_cap)
        word_len = buf_cap - 1;
    if (word_len > 0)
        memcpy(keyword_buf, action + start, word_len);
    keyword_buf[word_len] = '\0';
    *out_len = word_len;
}

static uint64_t simple_hash(const char *str, size_t len) {
    uint64_t hash = 5381;
    for (size_t i = 0; i < len; i++)
        hash = ((hash << 5) + hash) + (uint64_t)(unsigned char)str[i];
    return hash;
}

hu_error_t hu_causal_world_model_create(hu_allocator_t *alloc, sqlite3 *db,
                                        hu_causal_world_model_t *out) {
    if (!alloc || !db || !out)
        return HU_ERR_INVALID_ARGUMENT;
    out->alloc = alloc;
    out->db = db;
    return HU_OK;
}

void hu_causal_world_model_deinit(hu_causal_world_model_t *model) {
    (void)model;
}

hu_error_t hu_causal_world_model_init_tables(hu_causal_world_model_t *model) {
    if (!model || !model->db)
        return HU_ERR_INVALID_ARGUMENT;

    const char *sql1 = "CREATE TABLE IF NOT EXISTS causal_observations("
                       "id INTEGER PRIMARY KEY, action TEXT, outcome TEXT, context TEXT, "
                       "confidence REAL, observed_at INTEGER)";
    const char *sql2 = "CREATE TABLE IF NOT EXISTS simulation_cache("
                       "id INTEGER PRIMARY KEY, action_hash INTEGER, context_hash INTEGER, "
                       "predicted_outcome TEXT, confidence REAL, created_at INTEGER)";
    const char *sql3 =
        "CREATE TABLE IF NOT EXISTS causal_nodes ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, label TEXT NOT NULL, type TEXT NOT NULL "
        "DEFAULT 'entity', properties TEXT, created_at INTEGER NOT NULL)";
    const char *sql4 =
        "CREATE TABLE IF NOT EXISTS causal_edges ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, source_id INTEGER NOT NULL "
        "REFERENCES causal_nodes(id), target_id INTEGER NOT NULL REFERENCES causal_nodes(id), "
        "edge_type INTEGER NOT NULL, confidence REAL NOT NULL DEFAULT 0.5, "
        "evidence_count INTEGER NOT NULL DEFAULT 1, last_observed INTEGER NOT NULL, "
        "UNIQUE(source_id, target_id, edge_type))";
    const char *sql5 = "CREATE INDEX IF NOT EXISTS idx_edges_source ON causal_edges(source_id)";
    const char *sql6 = "CREATE INDEX IF NOT EXISTS idx_edges_target ON causal_edges(target_id)";
    const char *sql7 =
        "CREATE TABLE IF NOT EXISTS prediction_accuracy("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, action TEXT, predicted TEXT, actual TEXT, "
        "predicted_confidence REAL, matched INTEGER, created_at INTEGER)";

    char *err = NULL;
    int rc = sqlite3_exec(model->db, sql1, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (err)
            sqlite3_free(err);
        return HU_ERR_MEMORY_STORE;
    }
    err = NULL;
    rc = sqlite3_exec(model->db, sql2, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (err)
            sqlite3_free(err);
        return HU_ERR_MEMORY_STORE;
    }
    err = NULL;
    rc = sqlite3_exec(model->db, sql3, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (err)
            sqlite3_free(err);
        return HU_ERR_MEMORY_STORE;
    }
    err = NULL;
    rc = sqlite3_exec(model->db, sql4, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (err)
            sqlite3_free(err);
        return HU_ERR_MEMORY_STORE;
    }
    err = NULL;
    rc = sqlite3_exec(model->db, sql5, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (err)
            sqlite3_free(err);
        return HU_ERR_MEMORY_STORE;
    }
    err = NULL;
    rc = sqlite3_exec(model->db, sql6, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (err)
            sqlite3_free(err);
        return HU_ERR_MEMORY_STORE;
    }
    err = NULL;
    rc = sqlite3_exec(model->db, sql7, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (err)
            sqlite3_free(err);
        return HU_ERR_MEMORY_STORE;
    }
    return HU_OK;
}

static void copy_to_prediction(hu_wm_prediction_t *out, const char *outcome, size_t olen,
                               double confidence, const char *reasoning, size_t rlen) {
    memset(out, 0, sizeof(*out));
    if (outcome && olen > 0) {
        size_t cp = olen > sizeof(out->outcome) - 1 ? sizeof(out->outcome) - 1 : olen;
        memcpy(out->outcome, outcome, cp);
        out->outcome[cp] = '\0';
        out->outcome_len = cp;
    }
    out->confidence = confidence;
    if (reasoning && rlen > 0) {
        size_t cp = rlen > sizeof(out->reasoning) - 1 ? sizeof(out->reasoning) - 1 : rlen;
        memcpy(out->reasoning, reasoning, cp);
        out->reasoning[cp] = '\0';
        out->reasoning_len = cp;
    }
}

hu_error_t hu_world_simulate(hu_causal_world_model_t *model, const char *action, size_t action_len,
                             const char *context, size_t context_len, hu_wm_prediction_t *out) {
    if (!model || !model->db || !out)
        return HU_ERR_INVALID_ARGUMENT;

    char keyword[256];
    size_t kw_len = 0;
    extract_keyword(action, action_len, keyword, sizeof(keyword), &kw_len);

    if (kw_len == 0) {
        copy_to_prediction(out, "Unknown outcome (no action keyword)", 34, 0.1, "", 0);
        return HU_OK;
    }

    char ctx_keyword[256];
    size_t ctx_kw_len = 0;
    int has_context = (context && context_len > 0);
    if (has_context)
        extract_keyword(context, context_len, ctx_keyword, sizeof(ctx_keyword), &ctx_kw_len);

    uint64_t action_hash = simple_hash(keyword, kw_len);
    uint64_t context_hash =
        has_context && ctx_kw_len > 0 ? simple_hash(ctx_keyword, ctx_kw_len) : 0;

    /* Check simulation cache (entries < 1 hour old) */
    int64_t now_ts = (int64_t)time(NULL);
    int64_t cache_cutoff = now_ts - 3600;
    sqlite3_stmt *cache_stmt = NULL;
    if (sqlite3_prepare_v2(model->db,
                           "SELECT predicted_outcome, confidence FROM simulation_cache "
                           "WHERE action_hash = ?1 AND context_hash = ?2 AND created_at > ?3",
                           -1, &cache_stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(cache_stmt, 1, (int64_t)action_hash);
        sqlite3_bind_int64(cache_stmt, 2, (int64_t)context_hash);
        sqlite3_bind_int64(cache_stmt, 3, cache_cutoff);
        if (sqlite3_step(cache_stmt) == SQLITE_ROW) {
            const char *cached_outcome = (const char *)sqlite3_column_text(cache_stmt, 0);
            double cached_conf = sqlite3_column_double(cache_stmt, 1);
            if (cached_outcome && cached_conf > 0.0) {
                size_t olen = (size_t)sqlite3_column_bytes(cache_stmt, 0);
                copy_to_prediction(out, cached_outcome, olen, cached_conf, "", 0);
                sqlite3_finalize(cache_stmt);
                return HU_OK;
            }
        }
        sqlite3_finalize(cache_stmt);
    }

    /* Build LIKE pattern: %keyword% */
    char pattern[512];
    int n = snprintf(pattern, sizeof(pattern), "%%%s%%", keyword);
    if (n < 0 || (size_t)n >= sizeof(pattern)) {
        copy_to_prediction(out, "Unknown outcome", 14, 0.1, "", 0);
        return HU_OK;
    }

    char ctx_pattern[512];
    int ctx_n = 0;
    int context_filter_used = 0;
    if (has_context && ctx_kw_len > 0) {
        ctx_n = snprintf(ctx_pattern, sizeof(ctx_pattern), "%%%s%%", ctx_keyword);
        context_filter_used = (ctx_n >= 0 && (size_t)ctx_n < sizeof(ctx_pattern));
    }

    const char *best_outcome = NULL;
    size_t best_outcome_len = 0;
    double best_confidence = 0.0;
    int context_matched = 0;

    /* Try context-filtered query first when context provided */
    if (context_filter_used) {
        sqlite3_stmt *stmt = NULL;
        int rc = sqlite3_prepare_v2(model->db,
                                    "SELECT outcome, confidence, context FROM causal_observations "
                                    "WHERE action LIKE ?1 AND (context IS NULL OR context LIKE ?2) "
                                    "ORDER BY confidence DESC",
                                    -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, ctx_pattern, -1, SQLITE_STATIC);
            while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
                const char *o = (const char *)sqlite3_column_text(stmt, 0);
                double c = sqlite3_column_double(stmt, 1);
                if (o && c > best_confidence) {
                    best_outcome = o;
                    best_outcome_len = (size_t)sqlite3_column_bytes(stmt, 0);
                    best_confidence = c;
                    context_matched = 1;
                }
            }
            sqlite3_finalize(stmt);
        }
    }

    /* Fallback: action-only query if no context match */
    int obs_count = 0;
    if (!best_outcome || best_confidence <= 0.0) {
        sqlite3_stmt *stmt = NULL;
        int rc = sqlite3_prepare_v2(model->db,
                                    "SELECT outcome, confidence, context FROM causal_observations "
                                    "WHERE action LIKE ? ORDER BY confidence DESC",
                                    -1, &stmt, NULL);
        if (rc != SQLITE_OK)
            return HU_ERR_MEMORY_STORE;
        sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_STATIC);
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            obs_count++;
            const char *o = (const char *)sqlite3_column_text(stmt, 0);
            double c = sqlite3_column_double(stmt, 1);
            const char *row_ctx = (const char *)sqlite3_column_text(stmt, 2);
            if (o && c > best_confidence) {
                best_outcome = o;
                best_outcome_len = (size_t)sqlite3_column_bytes(stmt, 0);
                best_confidence = c;
                context_matched = (context_filter_used && row_ctx != NULL &&
                                   strstr(row_ctx, ctx_keyword) != NULL);
            }
        }
        sqlite3_finalize(stmt);
    }

    /* Count observations when we got match from context-filtered query */
    if (context_filter_used && best_outcome && obs_count == 0) {
        sqlite3_stmt *cnt_stmt = NULL;
        if (sqlite3_prepare_v2(model->db,
                               "SELECT COUNT(*) FROM causal_observations WHERE action LIKE ?1 AND "
                               "(context IS NULL OR context LIKE ?2)",
                               -1, &cnt_stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(cnt_stmt, 1, pattern, -1, SQLITE_STATIC);
            sqlite3_bind_text(cnt_stmt, 2, ctx_pattern, -1, SQLITE_STATIC);
            if (sqlite3_step(cnt_stmt) == SQLITE_ROW)
                obs_count = (int)sqlite3_column_int64(cnt_stmt, 0);
            sqlite3_finalize(cnt_stmt);
        }
    }

    if (best_outcome && best_confidence > 0.0) {
        if (context_matched)
            best_confidence *= 1.2;
        /* Aggregate: slight boost for multiple observations */
        if (obs_count > 1 && best_confidence < 0.95)
            best_confidence += 0.05;
        if (best_confidence > 1.0)
            best_confidence = 1.0;
        char reasoning_buf[128];
        int rn = snprintf(reasoning_buf, sizeof(reasoning_buf),
                          "Based on %d matching observation(s) for action", obs_count);
        copy_to_prediction(out, best_outcome, best_outcome_len, best_confidence,
                           rn > 0 ? reasoning_buf : "", (size_t)(rn > 0 ? rn : 0));

        /* Store in simulation cache */
        sqlite3_stmt *ins_stmt = NULL;
        if (sqlite3_prepare_v2(model->db,
                               "INSERT INTO simulation_cache(action_hash, context_hash, "
                               "predicted_outcome, confidence, created_at) VALUES(?1,?2,?3,?4,?5)",
                               -1, &ins_stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(ins_stmt, 1, (int64_t)action_hash);
            sqlite3_bind_int64(ins_stmt, 2, (int64_t)context_hash);
            sqlite3_bind_text(ins_stmt, 3, best_outcome, (int)best_outcome_len, SQLITE_STATIC);
            sqlite3_bind_double(ins_stmt, 4, best_confidence);
            sqlite3_bind_int64(ins_stmt, 5, now_ts);
            (void)sqlite3_step(ins_stmt);
            sqlite3_finalize(ins_stmt);
        }
        return HU_OK;
    }

    copy_to_prediction(out, "Unknown outcome (no matching observations)", 42, 0.1, "", 0);
    return HU_OK;
}
hu_error_t hu_world_counterfactual(hu_causal_world_model_t *model, const char *original_action,
                                   size_t orig_len, const char *alternative, size_t alt_len,
                                   const char *context, size_t ctx_len, hu_wm_prediction_t *out) {
    if (!model || !model->db || !out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_wm_prediction_t orig_pred = {0};
    hu_wm_prediction_t alt_pred = {0};

    hu_error_t err =
        hu_world_simulate(model, original_action, orig_len, context, ctx_len, &orig_pred);
    if (err != HU_OK)
        return err;

    err = hu_world_simulate(model, alternative, alt_len, context, ctx_len, &alt_pred);
    if (err != HU_OK)
        return err;

    /* Build reasoning: compare original vs alternative */
    char reasoning[2048];
    int rn = snprintf(reasoning, sizeof(reasoning),
                      "Original (%.*s): %.2f -> %.*s. Alternative (%.*s): %.2f -> %.*s.",
                      (int)orig_len, original_action, orig_pred.confidence,
                      (int)orig_pred.outcome_len, orig_pred.outcome, (int)alt_len, alternative,
                      alt_pred.confidence, (int)alt_pred.outcome_len, alt_pred.outcome);
    if (rn < 0 || (size_t)rn >= sizeof(reasoning))
        rn = 0;

    copy_to_prediction(out, alt_pred.outcome, alt_pred.outcome_len, alt_pred.confidence, reasoning,
                       (size_t)rn);
    return HU_OK;
}

static int compare_options_by_score(const void *a, const void *b) {
    const hu_action_option_t *oa = (const hu_action_option_t *)a;
    const hu_action_option_t *ob = (const hu_action_option_t *)b;
    if (ob->score > oa->score)
        return 1;
    if (ob->score < oa->score)
        return -1;
    return 0;
}

hu_error_t hu_world_evaluate_options(hu_causal_world_model_t *model, const char **actions,
                                     const size_t *action_lens, size_t count, const char *context,
                                     size_t ctx_len, hu_action_option_t *out) {
    if (!model || !model->db || !actions || !out)
        return HU_ERR_INVALID_ARGUMENT;

    const int64_t seven_days_sec = 7 * 24 * 60 * 60;
    int64_t now_cutoff = 0;
    sqlite3_stmt *ts_stmt = NULL;
    if (sqlite3_prepare_v2(model->db, "SELECT MAX(observed_at) FROM causal_observations", -1,
                           &ts_stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(ts_stmt) == SQLITE_ROW) {
            int64_t max_ts = sqlite3_column_int64(ts_stmt, 0);
            now_cutoff = max_ts - seven_days_sec;
        }
        sqlite3_finalize(ts_stmt);
    }

    for (size_t i = 0; i < count; i++) {
        hu_action_option_t *opt = &out[i];
        memset(opt, 0, sizeof(*opt));

        const char *act = actions[i];
        size_t act_len = action_lens ? action_lens[i] : (act ? strlen(act) : 0);
        if (!act)
            act_len = 0;

        if (act && act_len > 0) {
            size_t cp = act_len > sizeof(opt->action) - 1 ? sizeof(opt->action) - 1 : act_len;
            memcpy(opt->action, act, cp);
            opt->action[cp] = '\0';
            opt->action_len = cp;
        }

        hu_error_t err = hu_world_simulate(model, act, act_len, context, ctx_len, &opt->prediction);
        if (err != HU_OK)
            return err;

        double recency_bonus = 0.0;
        char keyword[256];
        size_t kw_len = 0;
        extract_keyword(act, act_len, keyword, sizeof(keyword), &kw_len);
        if (kw_len > 0) {
            char pattern[512];
            snprintf(pattern, sizeof(pattern), "%%%s%%", keyword);
            sqlite3_stmt *rec_stmt = NULL;
            if (sqlite3_prepare_v2(model->db,
                                   "SELECT observed_at FROM causal_observations "
                                   "WHERE action LIKE ? ORDER BY observed_at DESC LIMIT 1",
                                   -1, &rec_stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_text(rec_stmt, 1, pattern, -1, SQLITE_STATIC);
                if (sqlite3_step(rec_stmt) == SQLITE_ROW) {
                    int64_t obs_at = sqlite3_column_int64(rec_stmt, 0);
                    if (obs_at >= now_cutoff)
                        recency_bonus = 0.1;
                }
                sqlite3_finalize(rec_stmt);
            }
        }

        opt->score = opt->prediction.confidence * (1.0 + recency_bonus);
    }

    qsort(out, count, sizeof(hu_action_option_t), compare_options_by_score);
    return HU_OK;
}

hu_error_t hu_world_record_outcome(hu_causal_world_model_t *model, const char *action,
                                   size_t action_len, const char *outcome, size_t outcome_len,
                                   double confidence, int64_t now_ts) {
    if (!model || !model->db)
        return HU_ERR_INVALID_ARGUMENT;
    if (!action || action_len == 0 || !outcome || outcome_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(model->db,
                                "INSERT INTO causal_observations(action, outcome, context, "
                                "confidence, observed_at) VALUES(?, ?, NULL, ?, ?)",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;

    sqlite3_bind_text(stmt, 1, action, (int)action_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, outcome, (int)outcome_len, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 3, confidence);
    sqlite3_bind_int64(stmt, 4, now_ts);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_STORE;
}
#endif /* HU_ENABLE_SQLITE */
