#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/intelligence/reflection.h"
#include <stdio.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

#ifdef HU_ENABLE_SQLITE
#include "human/intelligence/skills.h"
#include <sqlite3.h>
#endif

#define HU_REFLECTION_ESCAPE_BUF 1024
#define HU_REFLECTION_SQL_BUF 4096

static void escape_sql_string(const char *s, size_t len, char *buf, size_t cap, size_t *out_len) {
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 2 < cap; i++) {
        if (s[i] == '\'') {
            buf[pos++] = '\'';
            buf[pos++] = '\'';
        } else {
            buf[pos++] = s[i];
        }
    }
    buf[pos] = '\0';
    *out_len = pos;
}

hu_error_t hu_reflection_create_tables_sql(char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 1024)
        return HU_ERR_INVALID_ARGUMENT;

    static const char sql[] =
        "CREATE TABLE IF NOT EXISTS feedback_signals (\n"
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
        "    contact_id TEXT NOT NULL,\n"
        "    type INTEGER NOT NULL,\n"
        "    context TEXT,\n"
        "    our_action TEXT,\n"
        "    timestamp INTEGER NOT NULL\n"
        ");\n"
        "CREATE INDEX IF NOT EXISTS idx_feedback_contact ON feedback_signals(contact_id);\n"
        "CREATE INDEX IF NOT EXISTS idx_feedback_timestamp ON feedback_signals(timestamp);\n"
        "CREATE TABLE IF NOT EXISTS reflections (\n"
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
        "    period TEXT NOT NULL,\n"
        "    insights TEXT,\n"
        "    improvements TEXT,\n"
        "    created_at INTEGER NOT NULL\n"
        ");\n"
        "CREATE INDEX IF NOT EXISTS idx_reflections_period ON reflections(period);\n"
        "CREATE INDEX IF NOT EXISTS idx_reflections_created ON reflections(created_at);\n";

    size_t len = sizeof(sql) - 1;
    if (len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, sql, len + 1);
    *out_len = len;
    return HU_OK;
}

hu_error_t hu_feedback_insert_sql(const hu_feedback_signal_t *fb, char *buf, size_t cap,
                                 size_t *out_len) {
    if (!fb || !buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;
    if (!fb->contact_id)
        return HU_ERR_INVALID_ARGUMENT;

    char contact_esc[HU_REFLECTION_ESCAPE_BUF];
    char context_esc[HU_REFLECTION_ESCAPE_BUF];
    char action_esc[HU_REFLECTION_ESCAPE_BUF];

    size_t ce_len, cxe_len, ae_len;
    escape_sql_string(fb->contact_id, fb->contact_id_len, contact_esc, sizeof(contact_esc),
                      &ce_len);
    escape_sql_string(fb->context ? fb->context : "", fb->context_len, context_esc,
                      sizeof(context_esc), &cxe_len);
    escape_sql_string(fb->our_action ? fb->our_action : "", fb->our_action_len, action_esc,
                      sizeof(action_esc), &ae_len);

    int n = snprintf(buf, cap,
                    "INSERT INTO feedback_signals (contact_id, type, context, our_action, timestamp) "
                    "VALUES ('%s', %d, '%s', '%s', %llu)",
                    contact_esc, (int)fb->type, context_esc, action_esc,
                    (unsigned long long)fb->timestamp);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_feedback_query_recent_sql(const char *contact_id, size_t len, uint32_t limit,
                                        char *buf, size_t cap, size_t *out_len) {
    if (!contact_id || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;

    char contact_esc[HU_REFLECTION_ESCAPE_BUF];
    size_t ce_len;
    escape_sql_string(contact_id, len, contact_esc, sizeof(contact_esc), &ce_len);

    int n = snprintf(buf, cap,
                    "SELECT id, contact_id, type, context, our_action, timestamp FROM "
                    "feedback_signals WHERE contact_id = '%s' ORDER BY timestamp DESC LIMIT %u",
                    contact_esc, limit);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_reflection_insert_sql(const hu_reflection_entry_t *entry, char *buf, size_t cap,
                                   size_t *out_len) {
    if (!entry || !buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;
    if (!entry->period)
        return HU_ERR_INVALID_ARGUMENT;

    char period_esc[HU_REFLECTION_ESCAPE_BUF];
    char insights_esc[HU_REFLECTION_ESCAPE_BUF];
    char improvements_esc[HU_REFLECTION_ESCAPE_BUF];

    size_t pe_len, ie_len, ime_len;
    escape_sql_string(entry->period, entry->period_len, period_esc, sizeof(period_esc), &pe_len);
    escape_sql_string(entry->insights ? entry->insights : "", entry->insights_len, insights_esc,
                      sizeof(insights_esc), &ie_len);
    escape_sql_string(entry->improvements ? entry->improvements : "", entry->improvements_len,
                      improvements_esc, sizeof(improvements_esc), &ime_len);

    int n = snprintf(buf, cap,
                    "INSERT INTO reflections (period, insights, improvements, created_at) "
                    "VALUES ('%s', '%s', '%s', %llu)",
                    period_esc, insights_esc, improvements_esc,
                    (unsigned long long)entry->created_at);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_reflection_query_latest_sql(const char *period, size_t period_len, char *buf,
                                          size_t cap, size_t *out_len) {
    if (!period || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;

    char period_esc[HU_REFLECTION_ESCAPE_BUF];
    size_t pe_len;
    escape_sql_string(period, period_len, period_esc, sizeof(period_esc), &pe_len);

    int n = snprintf(buf, cap,
                    "SELECT id, period, insights, improvements, created_at FROM reflections "
                    "WHERE period = '%s' ORDER BY created_at DESC LIMIT 1",
                    period_esc);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_feedback_signal_type_t hu_reflection_feedback_classify(uint32_t response_time_seconds,
                                                          size_t response_length,
                                                          bool contains_question,
                                                          bool left_on_read) {
    (void)response_time_seconds;
    if (left_on_read)
        return HU_FEEDBACK_NEGATIVE;
    if (response_length < 5)
        return HU_FEEDBACK_NEGATIVE;
    if (contains_question)
        return HU_FEEDBACK_POSITIVE;
    return HU_FEEDBACK_NEUTRAL;
}

double hu_skill_proficiency_score(uint32_t positive_count, uint32_t total_count,
                                 uint32_t practice_count) {
    if (total_count == 0)
        return 0.0;
    double ratio = (double)positive_count / (double)total_count;
    double practice_factor = 1.0;
    if (practice_count < 10)
        practice_factor = (double)practice_count / 10.0;
    double score = ratio * practice_factor;
    if (score > 1.0)
        return 1.0;
    if (score < 0.0)
        return 0.0;
    return score;
}

double hu_cross_contact_learning_weight(double source_proficiency, double relevance) {
    return source_proficiency * relevance * 0.5;
}

#ifdef HU_ENABLE_SQLITE
hu_error_t hu_reflection_engine_create(hu_allocator_t *alloc, sqlite3 *db,
                                      hu_reflection_engine_t *out) {
    if (!alloc || !db || !out)
        return HU_ERR_INVALID_ARGUMENT;
    out->alloc = alloc;
    out->db = db;
    return HU_OK;
}

void hu_reflection_engine_deinit(hu_reflection_engine_t *engine) {
    (void)engine;
}

hu_error_t hu_reflection_weekly(hu_reflection_engine_t *engine, int64_t now_ts) {
    if (!engine || !engine->db || !engine->alloc)
        return HU_ERR_INVALID_ARGUMENT;

    int64_t week = now_ts / 604800;
    int64_t cutoff = now_ts - (7 * 24 * 3600);

    const char *sel_contacts =
        "SELECT DISTINCT contact_id FROM behavioral_feedback WHERE timestamp >= ?1 AND timestamp <= ?2";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db, sel_contacts, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;

    sqlite3_bind_int64(stmt, 1, cutoff);
    sqlite3_bind_int64(stmt, 2, now_ts);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *contact_id = (const char *)sqlite3_column_text(stmt, 0);
        if (!contact_id)
            continue;
        size_t cid_len = (size_t)sqlite3_column_bytes(stmt, 0);

        const char *sel_counts =
            "SELECT signal, COUNT(*) FROM behavioral_feedback "
            "WHERE contact_id = ?1 AND timestamp >= ?2 AND timestamp <= ?3 GROUP BY signal";
        sqlite3_stmt *count_stmt = NULL;
        rc = sqlite3_prepare_v2(engine->db, sel_counts, -1, &count_stmt, NULL);
        if (rc != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return HU_ERR_MEMORY_STORE;
        }
        sqlite3_bind_text(count_stmt, 1, contact_id, (int)cid_len, SQLITE_STATIC);
        sqlite3_bind_int64(count_stmt, 2, cutoff);
        sqlite3_bind_int64(count_stmt, 3, now_ts);

        uint32_t pos = 0, neg = 0, neu = 0;
        while (sqlite3_step(count_stmt) == SQLITE_ROW) {
            const char *sig = (const char *)sqlite3_column_text(count_stmt, 0);
            int cnt = sqlite3_column_int(count_stmt, 1);
            if (sig) {
                if (strcmp(sig, "positive") == 0)
                    pos = (uint32_t)cnt;
                else if (strcmp(sig, "negative") == 0)
                    neg = (uint32_t)cnt;
                else
                    neu = (uint32_t)cnt;
            }
        }
        sqlite3_finalize(count_stmt);

        double health = (double)pos / ((double)pos + (double)neg + 1.0);
        const char *rec = (health >= 0.5) ? "maintain" : "improve";

        char metrics[256];
        int n = snprintf(metrics, sizeof(metrics),
                         "{\"positive\":%u,\"negative\":%u,\"neutral\":%u,\"relationship_health\":%.4f}",
                         pos, neg, neu, health);
        if (n < 0 || (size_t)n >= sizeof(metrics)) {
            sqlite3_finalize(stmt);
            return HU_ERR_INVALID_ARGUMENT;
        }

        const char *ins =
            "INSERT INTO self_evaluations (contact_id, week, metrics, recommendations, created_at) "
            "VALUES (?1, ?2, ?3, ?4, ?5)";
        sqlite3_stmt *ins_stmt = NULL;
        rc = sqlite3_prepare_v2(engine->db, ins, -1, &ins_stmt, NULL);
        if (rc != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return HU_ERR_MEMORY_STORE;
        }
        sqlite3_bind_text(ins_stmt, 1, contact_id, (int)cid_len, SQLITE_STATIC);
        sqlite3_bind_int64(ins_stmt, 2, week);
        sqlite3_bind_text(ins_stmt, 3, metrics, n, SQLITE_STATIC);
        sqlite3_bind_text(ins_stmt, 4, rec, (int)strlen(rec), SQLITE_STATIC);
        sqlite3_bind_int64(ins_stmt, 5, now_ts);

        rc = sqlite3_step(ins_stmt);
        sqlite3_finalize(ins_stmt);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            return HU_ERR_MEMORY_STORE;
        }
    }
    sqlite3_finalize(stmt);
    return HU_OK;
}

hu_error_t hu_reflection_extract_general_lessons(hu_reflection_engine_t *engine, int64_t now_ts) {
    if (!engine || !engine->db || !engine->alloc)
        return HU_ERR_INVALID_ARGUMENT;

    int64_t cutoff = now_ts - (30 * 24 * 3600);

    const char *sel =
        "SELECT recommendations, COUNT(DISTINCT contact_id) as cnt FROM self_evaluations "
        "WHERE created_at >= ?1 AND recommendations IS NOT NULL AND recommendations != '' "
        "GROUP BY recommendations HAVING cnt >= 2";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db, sel, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_int64(stmt, 1, cutoff);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *rec = (const char *)sqlite3_column_text(stmt, 0);
        int contact_count = sqlite3_column_int(stmt, 1);
        if (!rec || contact_count < 2)
            continue;

        const char *sel_lesson = "SELECT id FROM general_lessons WHERE lesson = ?1";
        sqlite3_stmt *chk = NULL;
        rc = sqlite3_prepare_v2(engine->db, sel_lesson, -1, &chk, NULL);
        if (rc != SQLITE_OK)
            continue;
        sqlite3_bind_text(chk, 1, rec, -1, SQLITE_STATIC);
        int exists = (sqlite3_step(chk) == SQLITE_ROW);
        sqlite3_finalize(chk);

        if (exists) {
            const char *upd =
                "UPDATE general_lessons SET source_count = source_count + ?1, last_confirmed = ?2 "
                "WHERE lesson = ?3";
            sqlite3_stmt *upd_stmt = NULL;
            rc = sqlite3_prepare_v2(engine->db, upd, -1, &upd_stmt, NULL);
            if (rc != SQLITE_OK)
                continue;
            sqlite3_bind_int(upd_stmt, 1, contact_count);
            sqlite3_bind_int64(upd_stmt, 2, now_ts);
            sqlite3_bind_text(upd_stmt, 3, rec, -1, SQLITE_STATIC);
            sqlite3_step(upd_stmt);
            sqlite3_finalize(upd_stmt);
        } else {
            const char *ins =
                "INSERT INTO general_lessons (lesson, confidence, source_count, first_learned, last_confirmed) "
                "VALUES (?1, 0.5, ?2, ?3, ?4)";
            sqlite3_stmt *ins_stmt = NULL;
            rc = sqlite3_prepare_v2(engine->db, ins, -1, &ins_stmt, NULL);
            if (rc != SQLITE_OK)
                continue;
            sqlite3_bind_text(ins_stmt, 1, rec, -1, SQLITE_STATIC);
            sqlite3_bind_int(ins_stmt, 2, contact_count);
            sqlite3_bind_int64(ins_stmt, 3, now_ts);
            sqlite3_bind_int64(ins_stmt, 4, now_ts);
            sqlite3_step(ins_stmt);
            sqlite3_finalize(ins_stmt);
        }
    }
    sqlite3_finalize(stmt);
    return HU_OK;
}
#endif /* HU_ENABLE_SQLITE */

hu_error_t hu_reflection_build_prompt(hu_allocator_t *alloc,
                                     const hu_reflection_entry_t *latest,
                                     const hu_feedback_signal_t *recent_feedback, size_t fb_count,
                                     char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    uint32_t pos_count = 0;
    uint32_t neg_count = 0;
    uint32_t neu_count = 0;
    uint32_t corr_count = 0;
    for (size_t i = 0; i < fb_count && recent_feedback; i++) {
        switch (recent_feedback[i].type) {
        case HU_FEEDBACK_POSITIVE:
            pos_count++;
            break;
        case HU_FEEDBACK_NEGATIVE:
            neg_count++;
            break;
        case HU_FEEDBACK_NEUTRAL:
            neu_count++;
            break;
        case HU_FEEDBACK_CORRECTION:
            corr_count++;
            break;
        }
    }

    const char *insights_str = (latest && latest->insights && latest->insights_len > 0)
                                  ? latest->insights
                                  : "(none yet)";
    size_t insights_len =
        (latest && latest->insights && latest->insights_len > 0) ? latest->insights_len : 10;

    const char *focus = "ask more questions.";
    if (neg_count > pos_count)
        focus = "ask more questions.";
    else if (corr_count > 0)
        focus = "clarify intent before acting.";
    else if (pos_count > neg_count)
        focus = "continue current approach.";

    size_t cap = 512 + insights_len + 128;
    char *p = (char *)alloc->alloc(alloc->ctx, cap);
    if (!p)
        return HU_ERR_OUT_OF_MEMORY;

    int n = snprintf(p, cap,
                    "[RECENT LEARNING]: Last reflection: [%.*s]. Recent feedback: %u positive, "
                    "%u negative, %u neutral. Focus: %s",
                    (int)insights_len, insights_str, pos_count, neg_count, neu_count, focus);
    if (n < 0 || (size_t)n >= cap) {
        alloc->free(alloc->ctx, p, cap);
        return HU_ERR_INVALID_ARGUMENT;
    }

    *out = p;
    *out_len = (size_t)n;
    return HU_OK;
}

const char *hu_feedback_type_str(hu_feedback_signal_type_t type) {
    switch (type) {
    case HU_FEEDBACK_POSITIVE:
        return "positive";
    case HU_FEEDBACK_NEGATIVE:
        return "negative";
    case HU_FEEDBACK_NEUTRAL:
        return "neutral";
    case HU_FEEDBACK_CORRECTION:
        return "correction";
    default:
        return "unknown";
    }
}

void hu_feedback_signal_deinit(hu_allocator_t *alloc, hu_feedback_signal_t *fb) {
    if (!alloc || !fb)
        return;
    if (fb->contact_id) {
        hu_str_free(alloc, fb->contact_id);
        fb->contact_id = NULL;
        fb->contact_id_len = 0;
    }
    if (fb->context) {
        hu_str_free(alloc, fb->context);
        fb->context = NULL;
        fb->context_len = 0;
    }
    if (fb->our_action) {
        hu_str_free(alloc, fb->our_action);
        fb->our_action = NULL;
        fb->our_action_len = 0;
    }
}

void hu_reflection_entry_deinit(hu_allocator_t *alloc, hu_reflection_entry_t *entry) {
    if (!alloc || !entry)
        return;
    if (entry->period) {
        hu_str_free(alloc, entry->period);
        entry->period = NULL;
        entry->period_len = 0;
    }
    if (entry->insights) {
        hu_str_free(alloc, entry->insights);
        entry->insights = NULL;
        entry->insights_len = 0;
    }
    if (entry->improvements) {
        hu_str_free(alloc, entry->improvements);
        entry->improvements = NULL;
        entry->improvements_len = 0;
    }
}

void hu_skill_observation_deinit(hu_allocator_t *alloc, hu_skill_observation_t *obs) {
    if (!alloc || !obs)
        return;
    if (obs->skill_name) {
        hu_str_free(alloc, obs->skill_name);
        obs->skill_name = NULL;
        obs->skill_name_len = 0;
    }
    if (obs->contact_id) {
        hu_str_free(alloc, obs->contact_id);
        obs->contact_id = NULL;
        obs->contact_id_len = 0;
    }
}

#ifdef HU_ENABLE_SQLITE
hu_error_t hu_reflection_daily(hu_reflection_engine_t *engine, int64_t now_ts) {
    if (!engine || !engine->alloc || !engine->db)
        return HU_ERR_INVALID_ARGUMENT;

    int64_t cutoff = now_ts - 86400;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db,
                                "SELECT contact_id, behavior_type FROM behavioral_feedback "
                                "WHERE timestamp > ?1 AND signal = 'positive' "
                                "GROUP BY contact_id, behavior_type HAVING COUNT(*) >= 3",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;

    sqlite3_bind_int64(stmt, 1, cutoff);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *contact = (const char *)sqlite3_column_text(stmt, 0);
        const char *bt = (const char *)sqlite3_column_text(stmt, 1);
        if (!contact || !bt)
            continue;

        size_t cid_len = (size_t)sqlite3_column_bytes(stmt, 0);
        size_t bt_len = (size_t)sqlite3_column_bytes(stmt, 1);
        if (cid_len == 0 || bt_len == 0)
            continue;

        char name_buf[128];
        int n = snprintf(name_buf, sizeof(name_buf), "reflection_%.*s_%.*s",
                         (int)bt_len, bt, (int)cid_len, contact);
        if (n < 0 || (size_t)n >= sizeof(name_buf))
            continue;

        hu_skill_t existing = {0};
        hu_error_t err = hu_skill_get_by_name(engine->alloc, engine->db, name_buf,
                                              (size_t)n, &existing);
        if (err == HU_OK && existing.id != 0)
            continue;

        const char *strategy = "Continue approach that elicited positive feedback.";
        const char *origin = "reflection";
        int64_t out_id = 0;
        err = hu_skill_insert(engine->alloc, engine->db,
                             name_buf, (size_t)n,
                             bt, bt_len,
                             contact, cid_len,
                             NULL, 0,
                             strategy, strlen(strategy),
                             origin, strlen(origin),
                             0, now_ts, &out_id);
        if (err != HU_OK)
            continue;
    }

    sqlite3_finalize(stmt);

    char val_buf[32];
    int val_n = snprintf(val_buf, sizeof(val_buf), "%lld", (long long)now_ts);
    if (val_n < 0 || (size_t)val_n >= sizeof(val_buf))
        return HU_OK;

    sqlite3_stmt *kv_stmt = NULL;
    rc = sqlite3_prepare_v2(engine->db,
                            "INSERT OR REPLACE INTO kv (key, value) VALUES ('reflection_daily_last', ?1)",
                            -1, &kv_stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_OK;

    char *val_copy = (char *)engine->alloc->alloc(engine->alloc->ctx, (size_t)val_n + 1);
    if (val_copy) {
        memcpy(val_copy, val_buf, (size_t)val_n + 1);
        sqlite3_bind_text(kv_stmt, 1, val_copy, val_n, SQLITE_STATIC);
        sqlite3_step(kv_stmt);
        engine->alloc->free(engine->alloc->ctx, val_copy, (size_t)val_n + 1);
    }
    sqlite3_finalize(kv_stmt);

    return HU_OK;
}
#endif /* HU_ENABLE_SQLITE */
