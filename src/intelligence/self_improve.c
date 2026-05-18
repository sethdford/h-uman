/*
 * Self-Improvement Engine — closes the reflection → behavior feedback loop.
 * Reads recent reflections, derives prompt patches and tool preferences.
 */

#ifdef HU_ENABLE_SQLITE

#include "human/intelligence/self_improve.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/core/slice.h"
#include "human/core/string.h"
#include <errno.h>
#include <sqlite3.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "human/eval.h"

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
#include "human/config.h"
#include "human/providers/factory.h"
#include <stdint.h>
#endif

#define PATCH_TEXT_MAX 2048
#define TOOL_NAME_MAX  128

/* Average judge score delta threshold: rollback structured patch if worse by more than this. */
#define HU_SELF_IMPROVE_EVAL_REGRESSION_THRESHOLD 0.05

#define HU_SELF_IMPROVE_FAST_EVAL_TASKS 3

hu_error_t hu_self_improve_create(hu_allocator_t *alloc, sqlite3 *db, hu_self_improve_t *out) {
    if (!alloc || !db || !out)
        return HU_ERR_INVALID_ARGUMENT;
    out->alloc = alloc;
    out->db = db;
    return HU_OK;
}

void hu_self_improve_deinit(hu_self_improve_t *engine) {
    (void)engine;
    /* Caller owns db; no-op */
}

hu_error_t hu_self_improve_init_tables(hu_self_improve_t *engine) {
    if (!engine || !engine->db)
        return HU_ERR_INVALID_ARGUMENT;

    const char *sql_patches = "CREATE TABLE IF NOT EXISTS prompt_patches("
                              "id INTEGER PRIMARY KEY, source TEXT, patch_text TEXT, "
                              "active INTEGER DEFAULT 1, applied_at INTEGER)";
    int rc = sqlite3_exec(engine->db, sql_patches, NULL, NULL, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;

    const char *sql_prefs =
        "CREATE TABLE IF NOT EXISTS tool_prefs("
        "tool_name TEXT PRIMARY KEY, weight REAL DEFAULT 1.0, "
        "successes INTEGER DEFAULT 0, failures INTEGER DEFAULT 0, updated_at INTEGER)";
    rc = sqlite3_exec(engine->db, sql_prefs, NULL, NULL, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;

    const char *sql_deltas = "CREATE TABLE IF NOT EXISTS self_improve_deltas("
                             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                             "patch_id TEXT NOT NULL,"
                             "score_before REAL,"
                             "score_after REAL,"
                             "delta REAL,"
                             "rolled_back INTEGER DEFAULT 0,"
                             "created_at TEXT DEFAULT (datetime('now')))";
    rc = sqlite3_exec(engine->db, sql_deltas, NULL, NULL, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;

    return HU_OK;
}

/* Extract patch text from recommendations (simple string, no LLM). */
static void extract_patch_text(const char *rec, size_t rec_len, char *out, size_t out_cap) {
    if (!rec || rec_len == 0 || !out || out_cap == 0) {
        if (out && out_cap > 0)
            out[0] = '\0';
        return;
    }
    size_t copy_len = rec_len < out_cap - 1 ? rec_len : out_cap - 1;
    memcpy(out, rec, copy_len);
    out[copy_len] = '\0';
    /* Trim trailing whitespace */
    while (copy_len > 0 && (out[copy_len - 1] == ' ' || out[copy_len - 1] == '\t' ||
                            out[copy_len - 1] == '\n' || out[copy_len - 1] == '\r')) {
        out[--copy_len] = '\0';
    }
}

hu_error_t hu_self_improve_apply_reflections(hu_self_improve_t *engine, int64_t now_ts) {
    if (!engine || !engine->db)
        return HU_ERR_INVALID_ARGUMENT;

    /* Get max applied_at from prompt_patches to find reflections since last patch */
    const char *max_sql = "SELECT COALESCE(MAX(applied_at), 0) FROM prompt_patches";
    sqlite3_stmt *max_stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db, max_sql, -1, &max_stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    int64_t since = 0;
    if (sqlite3_step(max_stmt) == SQLITE_ROW)
        since = sqlite3_column_int64(max_stmt, 0);
    sqlite3_finalize(max_stmt);

    /* Gather signals: negative feedback count, failing strategy weights, top lessons */
    int neg_count = 0, pos_count = 0;
    {
        sqlite3_stmt *fb = NULL;
        if (sqlite3_prepare_v2(engine->db,
                               "SELECT signal, COUNT(*) FROM behavioral_feedback "
                               "WHERE timestamp > ?1 GROUP BY signal",
                               -1, &fb, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(fb, 1, since);
            while (sqlite3_step(fb) == SQLITE_ROW) {
                const char *sig = (const char *)sqlite3_column_text(fb, 0);
                int cnt = sqlite3_column_int(fb, 1);
                if (sig && strcmp(sig, "negative") == 0)
                    neg_count = cnt;
                else if (sig && strcmp(sig, "positive") == 0)
                    pos_count = cnt;
            }
            sqlite3_finalize(fb);
        }
    }

    char lowest_strategy[64] = {0};
    double lowest_weight = 2.0;
    {
        sqlite3_stmt *sw = NULL;
        if (sqlite3_prepare_v2(
                engine->db,
                "SELECT strategy, weight FROM strategy_weights ORDER BY weight ASC LIMIT 1", -1,
                &sw, NULL) == SQLITE_OK) {
            if (sqlite3_step(sw) == SQLITE_ROW) {
                const char *s = (const char *)sqlite3_column_text(sw, 0);
                if (s)
                    snprintf(lowest_strategy, sizeof(lowest_strategy), "%s", s);
                lowest_weight = sqlite3_column_double(sw, 1);
            }
            sqlite3_finalize(sw);
        }
    }

    /* Pick one lesson we haven't recently patched about, rotating through them.
       Use the cycle timestamp modulo the lesson count to rotate. */
    char top_lesson[256] = {0};
    {
        int64_t lesson_count = 0;
        sqlite3_stmt *cnt = NULL;
        if (sqlite3_prepare_v2(engine->db,
                               "SELECT COUNT(*) FROM general_lessons WHERE source_count >= 2", -1,
                               &cnt, NULL) == SQLITE_OK) {
            if (sqlite3_step(cnt) == SQLITE_ROW)
                lesson_count = sqlite3_column_int64(cnt, 0);
            sqlite3_finalize(cnt);
        }
        int64_t offset = (lesson_count > 0) ? (now_ts % lesson_count) : 0;
        sqlite3_stmt *ls = NULL;
        if (sqlite3_prepare_v2(engine->db,
                               "SELECT lesson FROM general_lessons WHERE source_count >= 2 "
                               "ORDER BY source_count DESC, last_confirmed DESC "
                               "LIMIT 1 OFFSET ?1",
                               -1, &ls, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(ls, 1, offset);
            if (sqlite3_step(ls) == SQLITE_ROW) {
                const char *l = (const char *)sqlite3_column_text(ls, 0);
                if (l)
                    snprintf(top_lesson, sizeof(top_lesson), "%.*s", 200, l);
            }
            sqlite3_finalize(ls);
        }
    }

    /* Generate ALL applicable patches (not just one) for diverse coverage.
       Dedup by type prefix — only insert if no active patch with same prefix exists. */
    struct {
        const char *prefix;
        char text[PATCH_TEXT_MAX];
        int len;
    } candidates[4];
    size_t n_cand = 0;

    if (neg_count > 0 && neg_count * 3 > pos_count) {
        candidates[n_cand].prefix = "RELIABILITY ISSUE";
        candidates[n_cand].len =
            snprintf(candidates[n_cand].text, PATCH_TEXT_MAX,
                     "RELIABILITY ISSUE: %d failures vs %d successes recently. "
                     "Prefer shorter, more focused prompts. "
                     "If a tool or provider fails, retry with simpler input before escalating.",
                     neg_count, pos_count);
        n_cand++;
    }
    if (lowest_strategy[0] && lowest_weight < 0.8) {
        candidates[n_cand].prefix = "STRATEGY ADJUSTMENT";
        candidates[n_cand].len =
            snprintf(candidates[n_cand].text, PATCH_TEXT_MAX,
                     "STRATEGY ADJUSTMENT: '%s' has low effectiveness (%.0f%%). "
                     "Reduce reliance on this approach. Focus on higher-performing strategies.",
                     lowest_strategy, lowest_weight * 100);
        n_cand++;
    }
    if (top_lesson[0]) {
        candidates[n_cand].prefix = "LEARNED PATTERN";
        candidates[n_cand].len = snprintf(
            candidates[n_cand].text, PATCH_TEXT_MAX,
            "LEARNED PATTERN: %s. Apply this insight when analyzing new information.", top_lesson);
        n_cand++;
    }
    if (pos_count > 5) {
        candidates[n_cand].prefix = "PERFORMING WELL";
        candidates[n_cand].len =
            snprintf(candidates[n_cand].text, PATCH_TEXT_MAX,
                     "PERFORMING WELL: %d successful cycles. "
                     "Expand scope: look for cross-domain connections between findings. "
                     "Prioritize security and architecture implications.",
                     pos_count);
        n_cand++;
    }

    /* For each candidate, check if an active patch with the same prefix already exists.
       If it does, UPDATE it (keeps the slot fresh). If not, INSERT a new one. */
    for (size_t ci = 0; ci < n_cand; ci++) {
        if (candidates[ci].len <= 0 || (size_t)candidates[ci].len >= PATCH_TEXT_MAX)
            continue;

        size_t prefix_len = strlen(candidates[ci].prefix);
        sqlite3_stmt *dup = NULL;
        bool exists = false;
        if (sqlite3_prepare_v2(engine->db,
                               "SELECT id, patch_text FROM prompt_patches WHERE active = 1 "
                               "AND patch_text LIKE ?1 ORDER BY applied_at DESC LIMIT 1",
                               -1, &dup, NULL) == SQLITE_OK) {
            char like_pat[64];
            snprintf(like_pat, sizeof(like_pat), "%s%%", candidates[ci].prefix);
            sqlite3_bind_text(dup, 1, like_pat, -1, SQLITE_STATIC);
            if (sqlite3_step(dup) == SQLITE_ROW) {
                int64_t eid = sqlite3_column_int64(dup, 0);
                const char *old_text = (const char *)sqlite3_column_text(dup, 1);
                exists = true;
                if (!old_text ||
                    strncmp(old_text, candidates[ci].text, (size_t)candidates[ci].len) != 0) {
                    sqlite3_stmt *upd = NULL;
                    if (sqlite3_prepare_v2(
                            engine->db,
                            "UPDATE prompt_patches SET patch_text = ?, applied_at = ? WHERE id = ?",
                            -1, &upd, NULL) == SQLITE_OK) {
                        sqlite3_bind_text(upd, 1, candidates[ci].text, candidates[ci].len,
                                          SQLITE_STATIC);
                        sqlite3_bind_int64(upd, 2, now_ts);
                        sqlite3_bind_int64(upd, 3, eid);
                        sqlite3_step(upd);
                        sqlite3_finalize(upd);
                    }
                }
            }
            sqlite3_finalize(dup);
        }

        if (!exists) {
            sqlite3_stmt *ins = NULL;
            if (sqlite3_prepare_v2(
                    engine->db,
                    "INSERT INTO prompt_patches (source, patch_text, active, applied_at) "
                    "VALUES ('intelligence_cycle', ?, 1, ?)",
                    -1, &ins, NULL) == SQLITE_OK) {
                sqlite3_bind_text(ins, 1, candidates[ci].text, candidates[ci].len, SQLITE_STATIC);
                sqlite3_bind_int64(ins, 2, now_ts);
                sqlite3_step(ins);
                sqlite3_finalize(ins);
            }
        }
        (void)prefix_len;
    }

    /* Also process self_evaluation recommendations that aren't "maintain" */
    {
        const char *sel = "SELECT recommendations, created_at FROM self_evaluations "
                          "WHERE created_at > ?1 AND recommendations IS NOT NULL "
                          "AND recommendations != '' AND recommendations != 'maintain' "
                          "ORDER BY created_at ASC LIMIT 5";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(engine->db, sel, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, since);
            const char *ins = "INSERT INTO prompt_patches (source, patch_text, active, applied_at) "
                              "VALUES ('reflection', ?, 1, ?)";
            sqlite3_stmt *ins_stmt = NULL;
            if (sqlite3_prepare_v2(engine->db, ins, -1, &ins_stmt, NULL) == SQLITE_OK) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char *rec = (const char *)sqlite3_column_text(stmt, 0);
                    int64_t created_at = sqlite3_column_int64(stmt, 1);
                    if (!rec)
                        continue;
                    char rec_buf[PATCH_TEXT_MAX];
                    extract_patch_text(rec, strlen(rec), rec_buf, sizeof(rec_buf));
                    if (rec_buf[0] && strcmp(rec_buf, "maintain") != 0) {
                        sqlite3_bind_text(ins_stmt, 1, rec_buf, -1, SQLITE_STATIC);
                        sqlite3_bind_int64(ins_stmt, 2, created_at);
                        sqlite3_step(ins_stmt);
                        sqlite3_reset(ins_stmt);
                        sqlite3_clear_bindings(ins_stmt);
                    }
                }
                sqlite3_finalize(ins_stmt);
            }
            sqlite3_finalize(stmt);
        }
    }

    /* Deactivate stale patches (keep only the 10 most recent active) */
    {
        sqlite3_exec(
            engine->db,
            "UPDATE prompt_patches SET active = 0 WHERE id NOT IN "
            "(SELECT id FROM prompt_patches WHERE active = 1 ORDER BY applied_at DESC LIMIT 10)",
            NULL, NULL, NULL);
    }

    return HU_OK;
}

hu_error_t hu_self_improve_record_tool_outcome(hu_self_improve_t *engine, const char *tool_name,
                                               size_t name_len, bool succeeded, int64_t now_ts) {
    if (!engine || !engine->db || !tool_name)
        return HU_ERR_INVALID_ARGUMENT;
    if (name_len == 0)
        name_len = strlen(tool_name);
    if (name_len > TOOL_NAME_MAX - 1)
        name_len = TOOL_NAME_MAX - 1;

    /* Fetch current row if exists */
    const char *sel = "SELECT successes, failures FROM tool_prefs WHERE tool_name = ?1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db, sel, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;

    char name_buf[TOOL_NAME_MAX];
    memcpy(name_buf, tool_name, name_len);
    name_buf[name_len] = '\0';
    sqlite3_bind_text(stmt, 1, name_buf, -1, SQLITE_STATIC);

    int32_t successes = 0;
    int32_t failures = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        successes = (int32_t)sqlite3_column_int(stmt, 0);
        failures = (int32_t)sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);

    if (succeeded)
        successes++;
    else
        failures++;

    int total = successes + failures;
    double weight = (total > 0) ? (double)successes / (double)total : 1.0;
    if (weight < 0.1)
        weight = 0.1;
    if (weight > 2.0)
        weight = 2.0;

    const char *upsert =
        "INSERT OR REPLACE INTO tool_prefs (tool_name, weight, successes, failures, updated_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5)";
    rc = sqlite3_prepare_v2(engine->db, upsert, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_text(stmt, 1, name_buf, -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 2, weight);
    sqlite3_bind_int(stmt, 3, successes);
    sqlite3_bind_int(stmt, 4, failures);
    sqlite3_bind_int64(stmt, 5, now_ts);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_STORE;
}

double hu_self_improve_get_tool_weight(hu_self_improve_t *engine, const char *tool_name,
                                       size_t name_len) {
    if (!engine || !engine->db || !tool_name)
        return 1.0;
    if (name_len == 0)
        name_len = strlen(tool_name);
    if (name_len > TOOL_NAME_MAX - 1)
        name_len = TOOL_NAME_MAX - 1;

    char name_buf[TOOL_NAME_MAX];
    memcpy(name_buf, tool_name, name_len);
    name_buf[name_len] = '\0';

    const char *sql = "SELECT weight FROM tool_prefs WHERE tool_name = ?1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return 1.0;
    sqlite3_bind_text(stmt, 1, name_buf, -1, SQLITE_STATIC);
    double weight = 1.0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        weight = sqlite3_column_double(stmt, 0);
    sqlite3_finalize(stmt);
    return weight;
}

hu_error_t hu_self_improve_get_prompt_patches(hu_self_improve_t *engine, char **out,
                                              size_t *out_len) {
    if (!engine || !engine->db || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    const char *sql = "SELECT patch_text FROM prompt_patches WHERE active = 1 "
                      "ORDER BY applied_at DESC LIMIT 10";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;

    char *copies[10];
    hu_str_t parts[10];
    size_t count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < 10) {
        const char *text = (const char *)sqlite3_column_text(stmt, 0);
        int bytes = sqlite3_column_bytes(stmt, 0);
        if (text && text[0] && bytes > 0) {
            size_t len = (size_t)bytes;
            char *copy = (char *)engine->alloc->alloc(engine->alloc->ctx, len + 1);
            if (!copy) {
                for (size_t j = 0; j < count; j++)
                    engine->alloc->free(engine->alloc->ctx, copies[j], parts[j].len + 1);
                sqlite3_finalize(stmt);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(copy, text, len);
            copy[len] = '\0';
            copies[count] = copy;
            parts[count].ptr = copy;
            parts[count].len = len;
            count++;
        }
    }
    sqlite3_finalize(stmt);

    if (count == 0) {
        /* Empty result: return NULL/0 rather than a 1-byte sentinel.
         * Three of four call sites guard `if (patches && patches_len > 0)`
         * before freeing (agent_turn.c, agent_stream.c) — the sentinel
         * leaked from the empty branch because the guard prevents the
         * free. NULL output is unambiguous and matches what the test
         * callers in test_prove_e2e.c / test_agi_frontiers.c already
         * expect (`if (patches) free(patches)`). */
        *out = NULL;
        *out_len = 0;
        return HU_OK;
    }

    hu_str_t sep = HU_STR_LIT("\n");
    char *joined = hu_str_join(engine->alloc, parts, count, sep);
    for (size_t i = 0; i < count; i++)
        engine->alloc->free(engine->alloc->ctx, copies[i], parts[i].len + 1);
    if (!joined)
        return HU_ERR_OUT_OF_MEMORY;
    *out = joined;
    *out_len = strlen(joined);
    return HU_OK;
}

hu_error_t hu_self_improve_get_tool_prefs_prompt(hu_self_improve_t *engine, char **out,
                                                 size_t *out_len) {
    if (!engine || !engine->db || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    const char *sql = "SELECT tool_name, weight, successes, failures FROM tool_prefs "
                      "WHERE successes + failures >= 3 ORDER BY weight DESC LIMIT 10";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;

    char buf[2048];
    size_t pos = 0;
    pos = hu_buf_appendf(buf, sizeof(buf), pos, "Tool reliability:");
    bool first = true;

    while (sqlite3_step(stmt) == SQLITE_ROW && pos < sizeof(buf) - 64) {
        const char *tn = (const char *)sqlite3_column_text(stmt, 0);
        int32_t successes = (int32_t)sqlite3_column_int(stmt, 2);
        int32_t failures = (int32_t)sqlite3_column_int(stmt, 3);
        if (!tn)
            continue;
        int total = successes + failures;
        int pct = (total > 0) ? (int)((successes * 100.0) / (double)total + 0.5) : 0;
        int n = snprintf(buf + pos, sizeof(buf) - pos, "%s %s (%d%%)", first ? " " : ", ", tn, pct);
        if (n > 0 && pos + (size_t)n < sizeof(buf)) {
            pos += (size_t)n;
            first = false;
        }
    }
    sqlite3_finalize(stmt);

    if (first) {
        /* No qualifying tool_prefs rows. Return NULL/0 instead of a
         * 1-byte sentinel — see hu_self_improve_get_prompt_patches above
         * for the same rationale. Caller (agent_turn.c) already guards
         * `if (tool_prefs && tool_prefs_len > 0)` before freeing. */
        *out = NULL;
        *out_len = 0;
        return HU_OK;
    }

    char *result = (char *)engine->alloc->alloc(engine->alloc->ctx, pos + 1);
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(result, buf, pos);
    result[pos] = '\0';
    *out = result;
    *out_len = pos;
    return HU_OK;
}

hu_error_t hu_self_improve_active_patch_count(hu_self_improve_t *engine, size_t *out) {
    if (!engine || !engine->db || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = 0;

    const char *sql = "SELECT COUNT(*) FROM prompt_patches WHERE active = 1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        *out = (size_t)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return HU_OK;
}

/* --- Assessment-driven closed-loop self-improvement --- */

#include "human/intelligence/weakness.h"

hu_error_t hu_self_improve_from_assessment(hu_self_improve_t *engine, const hu_eval_run_t *run,
                                           const hu_eval_suite_t *suite, int64_t now_ts) {
    if (!engine || !engine->db || !run)
        return HU_ERR_INVALID_ARGUMENT;

    /* Create eval_patches table if needed */
    const char *ddl = "CREATE TABLE IF NOT EXISTS eval_patches("
                      "id INTEGER PRIMARY KEY, weakness_type TEXT, task_id TEXT, "
                      "patch_text TEXT, applied INTEGER DEFAULT 0, "
                      "pass_rate_before REAL, pass_rate_after REAL, "
                      "kept INTEGER DEFAULT 0, created_at INTEGER)";
    if (sqlite3_exec(engine->db, ddl, NULL, NULL, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;

    hu_weakness_report_t report = {0};
    hu_error_t err = hu_weakness_analyze(engine->alloc, run, suite, &report);
    if (err != HU_OK)
        return err;
    if (report.count == 0)
        return HU_OK;

    const char *ins_eval = "INSERT INTO eval_patches (weakness_type, task_id, patch_text, "
                           "applied, pass_rate_before, kept, created_at) "
                           "VALUES (?1, ?2, ?3, 0, ?4, 0, ?5)";
    const char *ins_prompt = "INSERT INTO prompt_patches (source, patch_text, active, applied_at) "
                             "VALUES (?1, ?2, 1, ?3)";
    sqlite3_stmt *s_eval = NULL, *s_prompt = NULL;
    int rc = sqlite3_prepare_v2(engine->db, ins_eval, -1, &s_eval, NULL);
    if (rc != SQLITE_OK) {
        hu_weakness_report_free(engine->alloc, &report);
        return HU_ERR_MEMORY_STORE;
    }
    rc = sqlite3_prepare_v2(engine->db, ins_prompt, -1, &s_prompt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(s_eval);
        hu_weakness_report_free(engine->alloc, &report);
        return HU_ERR_MEMORY_STORE;
    }

    for (size_t i = 0; i < report.count; i++) {
        hu_weakness_t *w = &report.items[i];

        /* Insert into eval_patches */
        sqlite3_bind_text(s_eval, 1, hu_weakness_type_str(w->type), -1, SQLITE_STATIC);
        sqlite3_bind_text(s_eval, 2, w->task_id, -1, SQLITE_STATIC);
        sqlite3_bind_text(s_eval, 3, w->suggested_fix, (int)w->suggested_fix_len, SQLITE_STATIC);
        sqlite3_bind_double(s_eval, 4, run->pass_rate);
        sqlite3_bind_int64(s_eval, 5, now_ts);
        sqlite3_step(s_eval);
        sqlite3_reset(s_eval);
        sqlite3_clear_bindings(s_eval);

        /* Also insert into prompt_patches so it takes effect */
        char source[256];
        snprintf(source, sizeof(source), "assessment:%s", w->task_id);
        sqlite3_bind_text(s_prompt, 1, source, -1, SQLITE_STATIC);
        sqlite3_bind_text(s_prompt, 2, w->suggested_fix, (int)w->suggested_fix_len, SQLITE_STATIC);
        sqlite3_bind_int64(s_prompt, 3, now_ts);
        sqlite3_step(s_prompt);
        sqlite3_reset(s_prompt);
        sqlite3_clear_bindings(s_prompt);
    }

    sqlite3_finalize(s_eval);
    sqlite3_finalize(s_prompt);
    hu_weakness_report_free(engine->alloc, &report);
    return HU_OK;
}

hu_error_t hu_self_improve_verify_patch(hu_self_improve_t *engine, int64_t patch_id,
                                        double new_pass_rate) {
    if (!engine || !engine->db)
        return HU_ERR_INVALID_ARGUMENT;

    /* Get the before rate */
    const char *sel = "SELECT pass_rate_before FROM eval_patches WHERE id = ?1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db, sel, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_int64(stmt, 1, patch_id);
    double before = 0.0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        before = sqlite3_column_double(stmt, 0);
    else {
        sqlite3_finalize(stmt);
        return HU_ERR_NOT_FOUND;
    }
    sqlite3_finalize(stmt);

    int kept = (new_pass_rate >= before) ? 1 : 0;

    const char *upd = "UPDATE eval_patches SET pass_rate_after = ?1, kept = ?2 WHERE id = ?3";
    rc = sqlite3_prepare_v2(engine->db, upd, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_double(stmt, 1, new_pass_rate);
    sqlite3_bind_int(stmt, 2, kept);
    sqlite3_bind_int64(stmt, 3, patch_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_STORE;

    /* If not kept, deactivate the prompt patch too */
    if (!kept) {
        const char *deactivate = "UPDATE prompt_patches SET active = 0 "
                                 "WHERE source LIKE 'assessment:%' AND applied_at = "
                                 "(SELECT created_at FROM eval_patches WHERE id = ?1)";
        rc = sqlite3_prepare_v2(engine->db, deactivate, -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, patch_id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    return HU_OK;
}

hu_error_t hu_self_improve_rollback_patch(hu_self_improve_t *engine, int64_t patch_id) {
    if (!engine || !engine->db)
        return HU_ERR_INVALID_ARGUMENT;

    /* Mark eval_patches entry as not kept */
    const char *upd = "UPDATE eval_patches SET kept = 0 WHERE id = ?1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db, upd, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_int64(stmt, 1, patch_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_STORE;

    /* Deactivate corresponding prompt patch */
    const char *deact = "UPDATE prompt_patches SET active = 0 "
                        "WHERE source LIKE 'assessment:%' AND applied_at = "
                        "(SELECT created_at FROM eval_patches WHERE id = ?1)";
    rc = sqlite3_prepare_v2(engine->db, deact, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, patch_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    return HU_OK;
}

hu_error_t hu_self_improve_kept_patch_count(hu_self_improve_t *engine, size_t *out) {
    if (!engine || !engine->db || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = 0;

    const char *sql = "SELECT COUNT(*) FROM eval_patches WHERE kept = 1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        *out = (size_t)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return HU_OK;
}

/* --- Closed-loop eval → weakness → patch → re-eval → keep/rollback --- */

static hu_error_t ensure_eval_patches_table(sqlite3 *db) {
    const char *ddl = "CREATE TABLE IF NOT EXISTS eval_patches("
                      "id INTEGER PRIMARY KEY, weakness_type TEXT, task_id TEXT, "
                      "patch_text TEXT, applied INTEGER DEFAULT 0, "
                      "pass_rate_before REAL, pass_rate_after REAL, "
                      "kept INTEGER DEFAULT 0, created_at INTEGER)";
    if (sqlite3_exec(db, ddl, NULL, NULL, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    return HU_OK;
}

static const hu_weakness_t *pick_weakest_weakness(hu_allocator_t *alloc,
                                                  const hu_weakness_report_t *report,
                                                  const hu_eval_run_t *run,
                                                  const hu_eval_suite_t *suite) {
    if (!report || report->count == 0)
        return NULL;

    hu_weakness_summary_t summaries[8];
    size_t nsum = 0;
    if (hu_weakness_analyze_summary(alloc, run, suite, summaries,
                                    sizeof(summaries) / sizeof(summaries[0]), &nsum) != HU_OK ||
        nsum == 0)
        return &report->items[0];

    int max_fail = -1;
    hu_weakness_type_t best_ty = HU_WEAKNESS_UNKNOWN;
    for (size_t i = 0; i < nsum; i++) {
        if (summaries[i].failed_count > max_fail) {
            max_fail = summaries[i].failed_count;
            best_ty = summaries[i].type;
        }
    }
    for (size_t i = 0; i < report->count; i++) {
        if (report->items[i].type == best_ty)
            return &report->items[i];
    }
    return &report->items[0];
}

static hu_error_t insert_single_eval_patch(hu_self_improve_t *engine, const hu_weakness_t *w,
                                           double pass_rate_before, int64_t now_ts,
                                           int64_t *patch_id_out) {
    if (!engine || !engine->db || !w || !patch_id_out)
        return HU_ERR_INVALID_ARGUMENT;
    *patch_id_out = 0;

    hu_error_t err = ensure_eval_patches_table(engine->db);
    if (err != HU_OK)
        return err;

    const char *ins_eval = "INSERT INTO eval_patches (weakness_type, task_id, patch_text, "
                           "applied, pass_rate_before, kept, created_at) "
                           "VALUES (?1, ?2, ?3, 0, ?4, 0, ?5)";
    const char *ins_prompt = "INSERT INTO prompt_patches (source, patch_text, active, applied_at) "
                             "VALUES (?1, ?2, 1, ?3)";
    sqlite3_stmt *s_eval = NULL, *s_prompt = NULL;
    int rc = sqlite3_prepare_v2(engine->db, ins_eval, -1, &s_eval, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    rc = sqlite3_prepare_v2(engine->db, ins_prompt, -1, &s_prompt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(s_eval);
        return HU_ERR_MEMORY_STORE;
    }

    sqlite3_bind_text(s_eval, 1, hu_weakness_type_str(w->type), -1, SQLITE_STATIC);
    sqlite3_bind_text(s_eval, 2, w->task_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(s_eval, 3, w->suggested_fix, (int)w->suggested_fix_len, SQLITE_STATIC);
    sqlite3_bind_double(s_eval, 4, pass_rate_before);
    sqlite3_bind_int64(s_eval, 5, now_ts);
    rc = sqlite3_step(s_eval);
    sqlite3_finalize(s_eval);
    if (rc != SQLITE_DONE) {
        sqlite3_finalize(s_prompt);
        return HU_ERR_MEMORY_STORE;
    }

    *patch_id_out = sqlite3_last_insert_rowid(engine->db);

    char source[256];
    snprintf(source, sizeof(source), "assessment:%s", w->task_id);
    sqlite3_bind_text(s_prompt, 1, source, -1, SQLITE_STATIC);
    sqlite3_bind_text(s_prompt, 2, w->suggested_fix, (int)w->suggested_fix_len, SQLITE_STATIC);
    sqlite3_bind_int64(s_prompt, 3, now_ts);
    rc = sqlite3_step(s_prompt);
    sqlite3_finalize(s_prompt);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_STORE;

    return HU_OK;
}

#if defined(HU_IS_TEST) && HU_IS_TEST
static hu_error_t self_improve_closed_loop_simulate(hu_allocator_t *alloc, sqlite3 *db) {
    hu_self_improve_t engine = {0};
    hu_error_t err = hu_self_improve_create(alloc, db, &engine);
    if (err != HU_OK)
        return err;
    err = hu_self_improve_init_tables(&engine);
    if (err != HU_OK)
        return err;

    hu_eval_task_t tasks[5] = {
        {.id = "t1",
         .prompt = "2+2",
         .prompt_len = 3,
         .expected = "4",
         .expected_len = 1,
         .category = "math"},
        {.id = "t2",
         .prompt = "capital of France",
         .prompt_len = 17,
         .expected = "Paris",
         .expected_len = 5,
         .category = "geography"},
        {.id = "t3",
         .prompt = "1+1",
         .prompt_len = 3,
         .expected = "2",
         .expected_len = 1,
         .category = "math"},
        {.id = "t4",
         .prompt = "3*3",
         .prompt_len = 3,
         .expected = "9",
         .expected_len = 1,
         .category = "math"},
        {.id = "t5",
         .prompt = "tone",
         .prompt_len = 4,
         .expected = "brief",
         .expected_len = 5,
         .category = "format"},
    };
    hu_eval_result_t results[5] = {
        {.task_id = "t1", .passed = false, .actual_output = "5", .actual_output_len = 1},
        {.task_id = "t2", .passed = false, .actual_output = "London", .actual_output_len = 6},
        {.task_id = "t3", .passed = true, .actual_output = "2", .actual_output_len = 1},
        {.task_id = "t4", .passed = true, .actual_output = "9", .actual_output_len = 1},
        {.task_id = "t5", .passed = true, .actual_output = "ok", .actual_output_len = 2},
    };
    hu_eval_run_t run = {
        .results = results, .results_count = 5, .passed = 3, .failed = 2, .pass_rate = 0.6};
    hu_eval_suite_t suite = {.name = "closed_loop_sim", .tasks = tasks, .tasks_count = 5};

    hu_weakness_report_t report = {0};
    err = hu_weakness_analyze(alloc, &run, &suite, &report);
    if (err != HU_OK)
        return err;
    if (report.count == 0) {
        hu_weakness_report_free(alloc, &report);
        return HU_ERR_INTERNAL;
    }

    const hu_weakness_t *w = pick_weakest_weakness(alloc, &report, &run, &suite);
    if (!w) {
        hu_weakness_report_free(alloc, &report);
        return HU_ERR_INTERNAL;
    }

    int64_t patch_id = 0;
    err = insert_single_eval_patch(&engine, w, run.pass_rate, 424242, &patch_id);
    hu_weakness_report_free(alloc, &report);
    if (err != HU_OK)
        return err;

    err = hu_self_improve_verify_patch(&engine, patch_id, 0.75);
    if (err != HU_OK)
        return err;

    hu_log_info("self_improve", NULL,
                "closed_loop: suite=sim baseline=60.0%% after=75.0%% action=kept patch_id=%lld",
                (long long)patch_id);
    return HU_OK;
}
#endif /* HU_IS_TEST */

hu_error_t hu_self_improve_closed_loop(hu_allocator_t *alloc, void *db_void,
                                       hu_provider_t *provider, const char *model, size_t model_len,
                                       const char *eval_suite_path) {
    if (!alloc || !db_void)
        return HU_ERR_INVALID_ARGUMENT;
#if !(defined(HU_IS_TEST) && HU_IS_TEST)
    if (!provider || !eval_suite_path || !eval_suite_path[0])
        return HU_ERR_INVALID_ARGUMENT;
#else
    (void)eval_suite_path;
#endif

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)provider;
    (void)model;
    (void)model_len;
    return self_improve_closed_loop_simulate(alloc, (sqlite3 *)db_void);
#else
    sqlite3 *db = (sqlite3 *)db_void;
    hu_self_improve_t engine = {0};
    hu_error_t err = hu_self_improve_create(alloc, db, &engine);
    if (err != HU_OK)
        return err;
    err = hu_self_improve_init_tables(&engine);
    if (err != HU_OK)
        return err;

    FILE *f = fopen(eval_suite_path, "rb");
    if (!f) {
        hu_log_info("self_improve", NULL, "closed_loop: cannot open %s: %s", eval_suite_path,
                    strerror(errno));
        return HU_ERR_IO;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    long sz = ftell(f);
    if (sz <= 0 || sz > 1024 * 1024) {
        fclose(f);
        hu_log_info("self_improve", NULL, "closed_loop: invalid suite file size");
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    size_t json_len = (size_t)sz;
    char *json = (char *)alloc->alloc(alloc->ctx, json_len + 1);
    if (!json) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t nread = fread(json, 1, json_len, f);
    fclose(f);
    if (nread != json_len) {
        alloc->free(alloc->ctx, json, json_len + 1);
        return HU_ERR_IO;
    }
    json[json_len] = '\0';

    hu_eval_suite_t suite = {0};
    err = hu_eval_suite_load_json(alloc, json, json_len, &suite);
    alloc->free(alloc->ctx, json, json_len + 1);
    if (err != HU_OK) {
        hu_log_error("self_improve", NULL, "closed_loop: load suite failed (%s)",
                     hu_error_string(err));
        return err;
    }

    hu_eval_run_t baseline = {0};
    err = hu_eval_run_suite(alloc, provider, model, model_len, &suite, HU_EVAL_CONTAINS, &baseline);
    if (err != HU_OK) {
        hu_eval_suite_free(alloc, &suite);
        hu_log_error("self_improve", NULL, "closed_loop: baseline eval failed (%s)",
                     hu_error_string(err));
        return err;
    }

    double baseline_rate = baseline.pass_rate;

    hu_weakness_report_t report = {0};
    err = hu_weakness_analyze(alloc, &baseline, &suite, &report);
    if (err != HU_OK) {
        hu_eval_run_free(alloc, &baseline);
        hu_eval_suite_free(alloc, &suite);
        return err;
    }
    if (report.count == 0) {
        hu_weakness_report_free(alloc, &report);
        hu_eval_run_free(alloc, &baseline);
        hu_eval_suite_free(alloc, &suite);
        hu_log_info("self_improve", NULL,
                    "closed_loop: suite=%s baseline=%.1f%% no weaknesses; skip patch",
                    suite.name ? suite.name : "", baseline_rate * 100.0);
        return HU_OK;
    }

    const hu_weakness_t *w = pick_weakest_weakness(alloc, &report, &baseline, &suite);
    if (!w) {
        hu_weakness_report_free(alloc, &report);
        hu_eval_run_free(alloc, &baseline);
        hu_eval_suite_free(alloc, &suite);
        return HU_ERR_INTERNAL;
    }

    int64_t now_ts = (int64_t)time(NULL);
    if (now_ts <= 0)
        now_ts = 1;
    int64_t patch_id = 0;
    err = insert_single_eval_patch(&engine, w, baseline_rate, now_ts, &patch_id);
    hu_weakness_report_free(alloc, &report);
    if (err != HU_OK) {
        hu_eval_run_free(alloc, &baseline);
        hu_eval_suite_free(alloc, &suite);
        return err;
    }

    hu_eval_run_t after = {0};
    err = hu_eval_run_suite(alloc, provider, model, model_len, &suite, HU_EVAL_CONTAINS, &after);
    if (err != HU_OK) {
        hu_self_improve_rollback_patch(&engine, patch_id);
        hu_eval_run_free(alloc, &baseline);
        hu_eval_suite_free(alloc, &suite);
        hu_log_error("self_improve", NULL, "closed_loop: re-eval failed (%s); rolled back",
                     hu_error_string(err));
        return err;
    }

    double after_rate = after.pass_rate;
    hu_eval_run_free(alloc, &after);

    const char *action = "rolled_back";
    if (after_rate > baseline_rate) {
        err = hu_self_improve_verify_patch(&engine, patch_id, after_rate);
        if (err != HU_OK) {
            hu_self_improve_rollback_patch(&engine, patch_id);
            hu_eval_run_free(alloc, &baseline);
            hu_eval_suite_free(alloc, &suite);
            return err;
        }
        action = "kept";
    } else {
        err = hu_self_improve_rollback_patch(&engine, patch_id);
        if (err != HU_OK) {
            hu_eval_run_free(alloc, &baseline);
            hu_eval_suite_free(alloc, &suite);
            return err;
        }
    }

    hu_log_info("self_improve", NULL,
                "closed_loop: suite=%s baseline=%.1f%% after=%.1f%% action=%s patch_id=%lld",
                suite.name ? suite.name : "", baseline_rate * 100.0, after_rate * 100.0, action,
                (long long)patch_id);

    hu_eval_run_free(alloc, &baseline);
    hu_eval_suite_free(alloc, &suite);
    return HU_OK;
#endif /* !HU_IS_TEST */
}

/* --- Structured patches (config/persona-oriented commands) --- */

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
static const char *self_improve_structured_type_label(hu_patch_type_t ty) {
    static const char *labels[] = {
        "TEMPERATURE", "MAX_TOKENS", "PERSONA_TRAIT_ADD", "PERSONA_TRAIT_REMOVE",
        "STYLE_RULE",  "TOOL_PREF",  "TEXT_HINT",
    };
    if ((int)ty < 0 || (size_t)ty >= sizeof(labels) / sizeof(labels[0]))
        return "UNKNOWN";
    return labels[ty];
}
#endif

bool hu_self_improve_parse_patch(const char *patch_text, size_t patch_text_len,
                                 hu_structured_patch_t *out) {
    if (!patch_text || patch_text_len == 0 || !out)
        return false;
    memset(out, 0, sizeof(*out));

    size_t colon = 0;
    bool found = false;
    for (size_t i = 0; i < patch_text_len; i++) {
        if (patch_text[i] == ':') {
            colon = i;
            found = true;
            break;
        }
    }
    if (!found || colon == 0 || colon >= patch_text_len - 1)
        return false;

    size_t klen = colon < sizeof(out->key) - 1 ? colon : sizeof(out->key) - 1;
    memcpy(out->key, patch_text, klen);
    out->key[klen] = '\0';

    const char *val = patch_text + colon + 1;
    size_t vlen = patch_text_len - colon - 1;
    if (vlen >= sizeof(out->value))
        vlen = sizeof(out->value) - 1;
    memcpy(out->value, val, vlen);
    out->value[vlen] = '\0';

    if (strcmp(out->key, "TEMPERATURE") == 0) {
        out->type = HU_PATCH_TEMPERATURE;
        out->numeric_value = strtod(out->value, NULL);
    } else if (strcmp(out->key, "MAX_TOKENS") == 0) {
        out->type = HU_PATCH_MAX_TOKENS;
        out->numeric_value = strtod(out->value, NULL);
    } else if (strcmp(out->key, "PERSONA_TRAIT") == 0) {
        out->type =
            (out->value[0] == '-') ? HU_PATCH_PERSONA_TRAIT_REMOVE : HU_PATCH_PERSONA_TRAIT_ADD;
    } else if (strcmp(out->key, "STYLE_RULE") == 0) {
        out->type = HU_PATCH_STYLE_RULE;
    } else if (strcmp(out->key, "TOOL_PREF") == 0) {
        out->type = HU_PATCH_TOOL_PREF;
        const char *sep = strchr(out->value, ':');
        if (sep)
            out->numeric_value = strtod(sep + 1, NULL);
    } else {
        out->type = HU_PATCH_TEXT_HINT;
    }
    out->parsed = true;
    return true;
}

hu_error_t hu_self_improve_apply_structured_patch(hu_self_improve_t *engine,
                                                  const hu_structured_patch_t *patch) {
    if (!engine || !patch)
        return HU_ERR_INVALID_ARGUMENT;
    if (!patch->parsed)
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)engine;
    return HU_OK;
#else
    if (!engine->db)
        return HU_ERR_NOT_SUPPORTED;

    const char *tname = self_improve_structured_type_label(patch->type);

    char patch_text[2048];
    int n = snprintf(patch_text, sizeof(patch_text), "[STRUCTURED:%s] %s", tname, patch->value);
    if (n < 0 || (size_t)n >= sizeof(patch_text))
        return HU_ERR_INTERNAL;

    sqlite3_stmt *deact = NULL;
    if (sqlite3_prepare_v2(
            engine->db,
            "UPDATE prompt_patches SET active = 0 WHERE patch_text LIKE ?1 AND active = 1", -1,
            &deact, NULL) == SQLITE_OK) {
        char like_pat[256];
        int ln = snprintf(like_pat, sizeof(like_pat), "[STRUCTURED:%s]%%", tname);
        if (ln > 0 && (size_t)ln < sizeof(like_pat)) {
            sqlite3_bind_text(deact, 1, like_pat, ln, SQLITE_STATIC);
            (void)sqlite3_step(deact);
        }
        sqlite3_finalize(deact);
    }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(engine->db,
                           "INSERT INTO prompt_patches(source, patch_text, active, applied_at) "
                           "VALUES('structured', ?1, 1, strftime('%s','now'))",
                           -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_text(stmt, 1, patch_text, n, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_STORE;
#endif
}

hu_error_t hu_self_improve_get_structured_patches(hu_self_improve_t *engine, hu_allocator_t *alloc,
                                                  hu_patch_type_t type, hu_structured_patch_t **out,
                                                  size_t *out_count) {
    if (!engine || !alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)engine;
    (void)type;
    return HU_OK;
#else
    if (!engine->db)
        return HU_ERR_NOT_SUPPORTED;

    const char *tname = self_improve_structured_type_label(type);
    char like_pat[256];
    int ln = snprintf(like_pat, sizeof(like_pat), "[STRUCTURED:%s]%%", tname);
    if (ln <= 0 || (size_t)ln >= sizeof(like_pat))
        return HU_ERR_INTERNAL;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(
            engine->db,
            "SELECT patch_text FROM prompt_patches WHERE patch_text LIKE ?1 AND active = 1 "
            "ORDER BY applied_at DESC LIMIT 20",
            -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_text(stmt, 1, like_pat, ln, SQLITE_STATIC);

    hu_structured_patch_t results[20];
    size_t count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < 20) {
        const char *text = (const char *)sqlite3_column_text(stmt, 0);
        if (!text)
            continue;
        const char *val_start = strchr(text, ']');
        if (val_start && val_start[1] == ' ')
            val_start += 2;
        else
            val_start = text;
        hu_structured_patch_t *p = &results[count];
        memset(p, 0, sizeof(*p));
        p->type = type;
        size_t vlen = strlen(val_start);
        if (vlen >= sizeof(p->value))
            vlen = sizeof(p->value) - 1;
        memcpy(p->value, val_start, vlen);
        p->value[vlen] = '\0';
        p->parsed = true;
        if (type == HU_PATCH_TEMPERATURE || type == HU_PATCH_MAX_TOKENS)
            p->numeric_value = strtod(p->value, NULL);
        else if (type == HU_PATCH_TOOL_PREF) {
            const char *sep = strchr(p->value, ':');
            if (sep)
                p->numeric_value = strtod(sep + 1, NULL);
        }
        count++;
    }
    sqlite3_finalize(stmt);

    if (count == 0)
        return HU_OK;

    hu_structured_patch_t *arr =
        (hu_structured_patch_t *)alloc->alloc(alloc->ctx, count * sizeof(hu_structured_patch_t));
    if (!arr)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(arr, results, count * sizeof(hu_structured_patch_t));
    *out = arr;
    *out_count = count;
    return HU_OK;
#endif
}

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
static void self_improve_pick_task_indices(size_t n_tasks, unsigned seed,
                                           size_t out_pick[HU_SELF_IMPROVE_FAST_EVAL_TASKS],
                                           size_t *out_n) {
    *out_n = 0;
    if (n_tasks == 0)
        return;
    size_t want = HU_SELF_IMPROVE_FAST_EVAL_TASKS;
    if (n_tasks < want)
        want = n_tasks;
    for (size_t k = 0; k < want; k++) {
        size_t idx = ((size_t)seed + k * 7919u) % n_tasks;
        size_t offset = 0;
        bool placed = false;
        while (offset < n_tasks) {
            size_t cand = (idx + offset) % n_tasks;
            bool dup = false;
            for (size_t j = 0; j < k; j++) {
                if (out_pick[j] == cand) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                out_pick[k] = cand;
                placed = true;
                break;
            }
            offset++;
        }
        if (!placed)
            out_pick[k] = k % n_tasks;
    }
    *out_n = want;
}

static double self_improve_avg_judge_score(const hu_eval_run_t *run) {
    if (!run || run->results_count == 0)
        return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < run->results_count; i++)
        sum += run->results[i].score;
    return sum / (double)run->results_count;
}

static hu_error_t self_improve_insert_delta(sqlite3 *db, const char *patch_id_str, double sb,
                                            double sa, double d) {
    const char *ins =
        "INSERT INTO self_improve_deltas (patch_id, score_before, score_after, delta, rolled_back) "
        "VALUES (?1, ?2, ?3, ?4, 0)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, ins, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_text(stmt, 1, patch_id_str, -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 2, sb);
    sqlite3_bind_double(stmt, 3, sa);
    sqlite3_bind_double(stmt, 4, d);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_MEMORY_STORE;
}

static hu_error_t self_improve_deactivate_structured_row(sqlite3 *db, sqlite3_int64 rowid) {
    const char *sql =
        "UPDATE prompt_patches SET active = 0 WHERE id = ?1 AND source = 'structured'";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_int64(stmt, 1, rowid);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_MEMORY_STORE;
}

static hu_error_t self_improve_open_fidelity_suite(hu_allocator_t *alloc, hu_eval_suite_t *suite) {
    static const char *paths[] = {
#ifdef HU_EVAL_SUITES_DIR
        HU_EVAL_SUITES_DIR "/fidelity.json",
#endif
        "eval_suites/fidelity.json",
        NULL,
    };
    for (size_t pi = 0; paths[pi]; pi++) {
        hu_error_t e = hu_eval_suite_load_json_path(alloc, paths[pi], suite);
        if (e == HU_OK)
            return HU_OK;
    }
    return HU_ERR_IO;
}

static hu_error_t self_improve_run_fast_eval(hu_allocator_t *alloc, hu_eval_suite_t *full,
                                             const size_t pick[HU_SELF_IMPROVE_FAST_EVAL_TASKS],
                                             size_t n_pick, hu_provider_t *provider,
                                             const char *model, size_t model_len,
                                             double *out_score) {
    if (!alloc || !full || !pick || !provider || !out_score)
        return HU_ERR_INVALID_ARGUMENT;
    hu_eval_task_t stack_tasks[HU_SELF_IMPROVE_FAST_EVAL_TASKS];
    for (size_t i = 0; i < n_pick; i++) {
        if (pick[i] >= full->tasks_count)
            return HU_ERR_INVALID_ARGUMENT;
        stack_tasks[i] = full->tasks[pick[i]];
    }
    hu_eval_suite_t sub = *full;
    sub.tasks = stack_tasks;
    sub.tasks_count = n_pick;
    hu_eval_run_t run = {0};
    hu_error_t err =
        hu_eval_run_suite(alloc, provider, model, model_len, &sub, full->default_match_mode, &run);
    if (err != HU_OK) {
        hu_eval_run_free(alloc, &run);
        return err;
    }
    *out_score = self_improve_avg_judge_score(&run);
    hu_eval_run_free(alloc, &run);
    return HU_OK;
}
#endif /* !HU_IS_TEST */

hu_error_t hu_self_improve_eval_and_apply(hu_allocator_t *alloc, sqlite3 *db,
                                          const hu_structured_patch_t *patch,
                                          hu_self_improve_delta_t *out_delta) {
    if (!alloc || !db || !patch || !out_delta)
        return HU_ERR_INVALID_ARGUMENT;
    if (!patch->parsed)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out_delta, 0, sizeof(*out_delta));

#if defined(HU_IS_TEST) && HU_IS_TEST
    out_delta->score_before = 0.7;
    out_delta->score_after = 0.72;
    out_delta->delta = out_delta->score_after - out_delta->score_before;
    out_delta->should_rollback = false;
    snprintf(out_delta->patch_id, sizeof(out_delta->patch_id), "test");
    return HU_OK;
#else
    hu_self_improve_t engine = {0};
    hu_error_t err = hu_self_improve_create(alloc, db, &engine);
    if (err != HU_OK)
        return err;
    err = hu_self_improve_init_tables(&engine);
    if (err != HU_OK)
        return err;

    hu_eval_suite_t suite = {0};
    err = self_improve_open_fidelity_suite(alloc, &suite);
    if (err != HU_OK) {
        hu_eval_suite_free(alloc, &suite);
        return err;
    }

    size_t work_n = suite.tasks_count;
    if (work_n > 128)
        work_n = 128;

    size_t picks[HU_SELF_IMPROVE_FAST_EVAL_TASKS];
    size_t n_pick = 0;
    unsigned seed =
        (unsigned)time(NULL) ^ (unsigned)patch->type ^ (unsigned)(unsigned char)patch->value[0];
    self_improve_pick_task_indices(work_n, seed, picks, &n_pick);
    if (n_pick == 0) {
        hu_eval_suite_free(alloc, &suite);
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_config_t cfg = {0};
    err = hu_config_load(alloc, &cfg);
    if (err != HU_OK) {
        hu_eval_suite_free(alloc, &suite);
        return err;
    }
    const char *prov = cfg.default_provider ? cfg.default_provider : "openai";
    size_t prov_len = strlen(prov);
    const char *model = cfg.default_model ? cfg.default_model : "";
    size_t model_len = model ? strlen(model) : 0;
    hu_provider_t provider = {0};
    err = hu_provider_create_from_config(alloc, &cfg, prov, prov_len, &provider);
    hu_config_deinit(&cfg);
    if (err != HU_OK) {
        hu_eval_suite_free(alloc, &suite);
        return err;
    }

    double before = 0.0;
    err = self_improve_run_fast_eval(alloc, &suite, picks, n_pick, &provider, model, model_len,
                                     &before);
    if (err != HU_OK) {
        if (provider.vtable && provider.vtable->deinit)
            provider.vtable->deinit(provider.ctx, alloc);
        hu_eval_suite_free(alloc, &suite);
        return err;
    }

    err = hu_self_improve_apply_structured_patch(&engine, patch);
    if (err != HU_OK) {
        if (provider.vtable && provider.vtable->deinit)
            provider.vtable->deinit(provider.ctx, alloc);
        hu_eval_suite_free(alloc, &suite);
        return err;
    }

    sqlite3_int64 rowid = sqlite3_last_insert_rowid(db);
    if (rowid <= 0) {
        if (provider.vtable && provider.vtable->deinit)
            provider.vtable->deinit(provider.ctx, alloc);
        hu_eval_suite_free(alloc, &suite);
        return HU_ERR_INTERNAL;
    }
    snprintf(out_delta->patch_id, sizeof(out_delta->patch_id), "%lld", (long long)rowid);

    double after = 0.0;
    err = self_improve_run_fast_eval(alloc, &suite, picks, n_pick, &provider, model, model_len,
                                     &after);
    if (provider.vtable && provider.vtable->deinit)
        provider.vtable->deinit(provider.ctx, alloc);
    hu_eval_suite_free(alloc, &suite);
    if (err != HU_OK) {
        (void)self_improve_deactivate_structured_row(db, rowid);
        return err;
    }

    out_delta->score_before = before;
    out_delta->score_after = after;
    out_delta->delta = after - before;
    out_delta->should_rollback = (out_delta->delta < -HU_SELF_IMPROVE_EVAL_REGRESSION_THRESHOLD);

    err = self_improve_insert_delta(db, out_delta->patch_id, before, after, out_delta->delta);
    if (err != HU_OK)
        return err;

    return HU_OK;
#endif /* !HU_IS_TEST */
}

hu_error_t hu_self_improve_rollback_if_negative(hu_allocator_t *alloc, sqlite3 *db,
                                                const hu_self_improve_delta_t *delta) {
    (void)alloc;
    if (!db || !delta)
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)delta;
    return HU_OK;
#else
    if (!delta->should_rollback)
        return HU_OK;

    char *end = NULL;
    sqlite3_int64 rowid = (sqlite3_int64)strtoll(delta->patch_id, &end, 10);
    if (!end || end == delta->patch_id || rowid <= 0)
        return HU_ERR_INVALID_ARGUMENT;

    hu_error_t err = self_improve_deactivate_structured_row(db, rowid);
    if (err != HU_OK)
        return err;

    const char *upd =
        "UPDATE self_improve_deltas SET rolled_back = 1 WHERE id = ("
        "SELECT id FROM self_improve_deltas WHERE patch_id = ?1 ORDER BY id DESC LIMIT 1)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, upd, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_text(stmt, 1, delta->patch_id, -1, SQLITE_STATIC);
    (void)sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return HU_OK;
#endif /* !HU_IS_TEST */
}

#endif /* HU_ENABLE_SQLITE */
