#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/intelligence/world_model.h"
#include <sqlite3.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Extract first word (keyword) from action for LIKE query */
static void extract_keyword(const char *action, size_t action_len,
                            char *keyword_buf, size_t buf_cap, size_t *out_len) {
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

hu_error_t hu_world_model_create(hu_allocator_t *alloc, sqlite3 *db,
                                 hu_world_model_t *out) {
    if (!alloc || !db || !out)
        return HU_ERR_INVALID_ARGUMENT;
    out->alloc = alloc;
    out->db = db;
    return HU_OK;
}

void hu_world_model_deinit(hu_world_model_t *model) {
    (void)model;
}

hu_error_t hu_world_model_init_tables(hu_world_model_t *model) {
    if (!model || !model->db)
        return HU_ERR_INVALID_ARGUMENT;

    const char *sql1 =
        "CREATE TABLE IF NOT EXISTS causal_observations("
        "id INTEGER PRIMARY KEY, action TEXT, outcome TEXT, context TEXT, "
        "confidence REAL, observed_at INTEGER)";
    const char *sql2 =
        "CREATE TABLE IF NOT EXISTS simulation_cache("
        "id INTEGER PRIMARY KEY, action_hash INTEGER, context_hash INTEGER, "
        "predicted_outcome TEXT, confidence REAL, created_at INTEGER)";

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

hu_error_t hu_world_simulate(hu_world_model_t *model,
                             const char *action, size_t action_len,
                             const char *context, size_t context_len,
                             hu_wm_prediction_t *out) {
    if (!model || !model->db || !out)
        return HU_ERR_INVALID_ARGUMENT;
    (void)context;
    (void)context_len;

    char keyword[256];
    size_t kw_len = 0;
    extract_keyword(action, action_len, keyword, sizeof(keyword), &kw_len);

    if (kw_len == 0) {
        copy_to_prediction(out, "Unknown outcome (no action keyword)", 34, 0.1, "", 0);
        return HU_OK;
    }

    /* Build LIKE pattern: %keyword% */
    char pattern[512];
    int n = snprintf(pattern, sizeof(pattern), "%%%s%%", keyword);
    if (n < 0 || (size_t)n >= sizeof(pattern)) {
        copy_to_prediction(out, "Unknown outcome", 14, 0.1, "", 0);
        return HU_OK;
    }

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(model->db,
                                "SELECT outcome, confidence FROM causal_observations "
                                "WHERE action LIKE ? ORDER BY confidence DESC",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;

    sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_STATIC);

    const char *best_outcome = NULL;
    size_t best_outcome_len = 0;
    double best_confidence = 0.0;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *o = (const char *)sqlite3_column_text(stmt, 0);
        double c = sqlite3_column_double(stmt, 1);
        if (o && c > best_confidence) {
            best_outcome = o;
            best_outcome_len = (size_t)sqlite3_column_bytes(stmt, 0);
            best_confidence = c;
        }
    }
    sqlite3_finalize(stmt);

    if (best_outcome && best_confidence > 0.0) {
        copy_to_prediction(out, best_outcome, best_outcome_len, best_confidence, "", 0);
        return HU_OK;
    }

    copy_to_prediction(out, "Unknown outcome (no matching observations)", 42, 0.1, "", 0);
    return HU_OK;
}

hu_error_t hu_world_counterfactual(hu_world_model_t *model,
                                   const char *original_action, size_t orig_len,
                                   const char *alternative, size_t alt_len,
                                   const char *context, size_t ctx_len,
                                   hu_wm_prediction_t *out) {
    if (!model || !model->db || !out)
        return HU_ERR_INVALID_ARGUMENT;
    (void)context;
    (void)ctx_len;

    hu_wm_prediction_t orig_pred = {0};
    hu_wm_prediction_t alt_pred = {0};

    hu_error_t err = hu_world_simulate(model, original_action, orig_len, context, ctx_len, &orig_pred);
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
                     (int)orig_pred.outcome_len, orig_pred.outcome,
                     (int)alt_len, alternative, alt_pred.confidence,
                     (int)alt_pred.outcome_len, alt_pred.outcome);
    if (rn < 0 || (size_t)rn >= sizeof(reasoning))
        rn = 0;

    copy_to_prediction(out, alt_pred.outcome, alt_pred.outcome_len,
                      alt_pred.confidence, reasoning, (size_t)rn);
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

hu_error_t hu_world_evaluate_options(hu_world_model_t *model,
                                      const char **actions, const size_t *action_lens,
                                      size_t count,
                                      const char *context, size_t ctx_len,
                                      hu_action_option_t *out) {
    if (!model || !model->db || !actions || !out)
        return HU_ERR_INVALID_ARGUMENT;

    const int64_t seven_days_sec = 7 * 24 * 60 * 60;
    int64_t now_cutoff = 0;
    sqlite3_stmt *ts_stmt = NULL;
    if (sqlite3_prepare_v2(model->db, "SELECT MAX(observed_at) FROM causal_observations",
                           -1, &ts_stmt, NULL) == SQLITE_OK) {
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

hu_error_t hu_world_record_outcome(hu_world_model_t *model,
                                   const char *action, size_t action_len,
                                   const char *outcome, size_t outcome_len,
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

hu_error_t hu_world_causal_depth(hu_world_model_t *model,
                                 const char *action, size_t action_len,
                                 size_t *out_depth) {
    if (!model || !model->db || !out_depth)
        return HU_ERR_INVALID_ARGUMENT;

    *out_depth = 0;

    char keyword[256];
    size_t kw_len = 0;
    extract_keyword(action, action_len, keyword, sizeof(keyword), &kw_len);
    if (kw_len == 0)
        return HU_OK;

    char pattern[512];
    int n = snprintf(pattern, sizeof(pattern), "%%%s%%", keyword);
    if (n < 0 || (size_t)n >= sizeof(pattern))
        return HU_OK;

    /* Count distinct outcomes that chain: o1.action matches, o1.outcome contains start of o2.action */
    const char *sql =
        "SELECT COUNT(DISTINCT o2.outcome) FROM causal_observations o1 "
        "JOIN causal_observations o2 ON o1.outcome LIKE '%' || substr(o2.action, 1, 20) || '%' "
        "WHERE o1.action LIKE ?";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(model->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;

    sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_ROW)
        *out_depth = (size_t)sqlite3_column_int64(stmt, 0);

    sqlite3_finalize(stmt);
    return HU_OK;
}

#endif /* HU_ENABLE_SQLITE */
