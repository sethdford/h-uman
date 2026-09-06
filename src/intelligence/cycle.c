#ifdef HU_ENABLE_SQLITE

#include "human/intelligence/cycle.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/intelligence/distiller.h"
#ifdef HU_ENABLE_FEEDS
#include "human/feeds/research_executor.h"
#endif
#include "human/intelligence/meta_learning.h"
#include "human/intelligence/online_learning.h"
#include "human/intelligence/reflection.h"
#include "human/intelligence/self_improve.h"
#include "human/intelligence/skills.h"
#include "human/intelligence/value_learning.h"
#include "human/intelligence/world_model.h"
#include <ctype.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define WORD_BUF_SIZE 64
#define MAX_WORDS     128

static int is_cycle_stop_word(const char *w, size_t len);

typedef struct word_count {
    char word[WORD_BUF_SIZE];
    int count;
} word_count_t;

static void extract_first_significant_word(const char *text, size_t text_len, char *out,
                                           size_t out_cap, size_t *out_len) {
    *out_len = 0;
    if (!text || out_cap == 0)
        return;
    size_t i = 0;
    while (i < text_len) {
        while (i < text_len &&
               (text[i] == ' ' || text[i] == '\t' || text[i] == ',' || text[i] == '.' ||
                text[i] == '*' || text[i] == '#' || text[i] == '-' || text[i] == '[' ||
                text[i] == ']' || text[i] == '(' || text[i] == ')' || text[i] == ':'))
            i++;
        if (i >= text_len)
            break;
        size_t start = i;
        while (i < text_len && text[i] != ' ' && text[i] != '\t' && text[i] != ',' &&
               text[i] != '.' && text[i] != '*' && text[i] != ':' && text[i] != '\0')
            i++;
        size_t word_len = i - start;
        if (word_len < 5)
            continue;
        if (is_cycle_stop_word(text + start, word_len))
            continue;

        if (word_len >= out_cap)
            word_len = out_cap - 1;
        memcpy(out, text + start, word_len);
        out[word_len] = '\0';
        *out_len = word_len;
        return;
    }
}

static int is_cycle_stop_word(const char *w, size_t len) {
    static const char *stops[] = {
        "h-uman",    "h-uman's",       "human",     "human's",     "should",       "could",
        "would",     "about",          "their",     "these",       "those",        "which",
        "where",     "there",          "being",     "other",       "after",        "before",
        "between",   "through",        "during",    "against",     "above",        "below",
        "under",     "using",          "based",     "focus",       "consider",     "ensure",
        "implement", "implementation", "integrate", "integration", "monitor",      "investigate",
        "evaluate",  "explore",        "relevant",  "development", "developments", "approach",
        "system",    "systems",        "findings",  "finding",     "research",     "suggests",
        "recurring", "theme",          "across",    "pattern",     "potential",    "current",
        NULL};
    for (int i = 0; stops[i]; i++) {
        if (strlen(stops[i]) == len && strncasecmp(w, stops[i], len) == 0)
            return 1;
    }
    if (len >= 6 && strncasecmp(w, "h-uman", 6) == 0)
        return 1;
    if (len >= 5 && strncasecmp(w, "human", 5) == 0)
        return 1;
    return 0;
}

static void add_word_to_counts(word_count_t *counts, int *n, const char *word, size_t word_len) {
    if (word_len < 5 || *n >= MAX_WORDS)
        return;
    if (word_len >= WORD_BUF_SIZE)
        word_len = WORD_BUF_SIZE - 1;
    char w[WORD_BUF_SIZE];
    for (size_t i = 0; i < word_len; i++)
        w[i] = (char)tolower((unsigned char)word[i]);
    w[word_len] = '\0';

    /* Skip project names, generic verbs, and meta-words */
    if (is_cycle_stop_word(w, word_len))
        return;
    /* Skip words starting with markdown artifacts */
    if (w[0] == '*' || w[0] == '#' || w[0] == '-' || w[0] == '[')
        return;

    for (int i = 0; i < *n; i++) {
        if (strncmp(counts[i].word, w, word_len) == 0 && counts[i].word[word_len] == '\0') {
            counts[i].count++;
            return;
        }
    }
    memcpy(counts[*n].word, w, word_len + 1);
    counts[*n].count = 1;
    (*n)++;
}

static void extract_words_from_text(const char *text, size_t text_len, word_count_t *counts,
                                    int *n) {
    size_t i = 0;
    while (i < text_len && *n < MAX_WORDS) {
        while (i < text_len && (text[i] == ' ' || text[i] == '\t' || text[i] == ',' ||
                                text[i] == '.' || text[i] == ';' || text[i] == ':'))
            i++;
        if (i >= text_len)
            break;
        size_t start = i;
        while (i < text_len && text[i] != ' ' && text[i] != '\t' && text[i] != ',' &&
               text[i] != '.' && text[i] != ';' && text[i] != ':' && text[i] != '\0')
            i++;
        size_t word_len = i - start;
        if (word_len >= 5)
            add_word_to_counts(counts, n, text + start, word_len);
    }
}

hu_error_t hu_intelligence_run_cycle(hu_allocator_t *alloc, sqlite3 *db,
                                     hu_intelligence_cycle_result_t *result) {
    if (!alloc || !db || !result)
        return HU_ERR_INVALID_ARGUMENT;

    memset(result, 0, sizeof(*result));
    int64_t now_ts = (int64_t)time(NULL);
    int steps_succeeded = 0;

    /* Step 0: Ensure self-improve tables exist and apply reflections */
    {
        hu_self_improve_t si = {0};
        if (hu_self_improve_create(alloc, db, &si) == HU_OK) {
            hu_error_t tbl_err = hu_self_improve_init_tables(&si);
            if (tbl_err != HU_OK)
                hu_log_error("cycle", NULL, "self_improve table init failed: %s",
                             hu_error_string(tbl_err));

            /* MAX(id) over all rows — a new insert always gets a new rowid even if older active
             * rows have larger ids than some inactive rows (uncommon); monotonic ids detect
             * inserts. */
            int64_t max_patch_id_before = 0;
            sqlite3_stmt *mx = NULL;
            if (sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(id),0) FROM prompt_patches", -1, &mx,
                                   NULL) == SQLITE_OK) {
                if (sqlite3_step(mx) == SQLITE_ROW)
                    max_patch_id_before = sqlite3_column_int64(mx, 0);
                sqlite3_finalize(mx);
            }

            (void)hu_self_improve_apply_reflections(&si, now_ts);

            int64_t max_patch_id_after = 0;
            mx = NULL;
            if (sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(id),0) FROM prompt_patches", -1, &mx,
                                   NULL) == SQLITE_OK) {
                if (sqlite3_step(mx) == SQLITE_ROW)
                    max_patch_id_after = sqlite3_column_int64(mx, 0);
                sqlite3_finalize(mx);
            }

            if (max_patch_id_after > max_patch_id_before) {
                sqlite3_stmt *ps = NULL;
                char patch_copy[2048];
                patch_copy[0] = '\0';
                if (sqlite3_prepare_v2(db, "SELECT patch_text FROM prompt_patches WHERE id = ?1",
                                       -1, &ps, NULL) == SQLITE_OK) {
                    sqlite3_bind_int64(ps, 1, max_patch_id_after);
                    if (sqlite3_step(ps) == SQLITE_ROW) {
                        const char *ptxt = (const char *)sqlite3_column_text(ps, 0);
                        int nbytes = sqlite3_column_bytes(ps, 0);
                        if (ptxt && nbytes > 0) {
                            size_t copy_len = (size_t)nbytes < sizeof(patch_copy) - 1
                                                  ? (size_t)nbytes
                                                  : sizeof(patch_copy) - 1;
                            memcpy(patch_copy, ptxt, copy_len);
                            patch_copy[copy_len] = '\0';
                        }
                    }
                    sqlite3_finalize(ps);
                }

                if (patch_copy[0]) {
                    hu_structured_patch_t sp = {0};
                    if (hu_self_improve_parse_patch(patch_copy, strlen(patch_copy), &sp) &&
                        sp.type != HU_PATCH_TEXT_HINT) {
                        sqlite3_stmt *deact = NULL;
                        if (sqlite3_prepare_v2(db,
                                               "UPDATE prompt_patches SET active = 0 WHERE id = ?1",
                                               -1, &deact, NULL) == SQLITE_OK) {
                            sqlite3_bind_int64(deact, 1, max_patch_id_after);
                            (void)sqlite3_step(deact);
                            sqlite3_finalize(deact);
                        }

                        hu_self_improve_delta_t delta = {0};
                        hu_error_t eval_err =
                            hu_self_improve_eval_and_apply(alloc, db, &sp, &delta);
                        if (eval_err == HU_OK) {
                            const char *rb = delta.should_rollback ? " [rolled back]" : "";
                            hu_log_info("self-improve", NULL,
                                        "patch %s: %.2f → %.2f (delta: %+.2f)%s", delta.patch_id,
                                        delta.score_before, delta.score_after, delta.delta, rb);
                            (void)hu_self_improve_rollback_if_negative(alloc, db, &delta);
                        }
                    }
                }
            }

            (void)hu_self_improve_record_tool_outcome(&si, "feed_digest", 11, true, now_ts);
            (void)hu_self_improve_record_tool_outcome(&si, "trend_detect", 12, true, now_ts);
            (void)hu_self_improve_record_tool_outcome(&si, "research_agent", 14, true, now_ts);

            hu_self_improve_deinit(&si);
            steps_succeeded++;
        }
    }

    /* Step 1: Action pending findings (HIGH/MEDIUM only; LOW stays pending) */
    {
        const char *sql = "SELECT id, finding, suggested_action, priority FROM research_findings "
                          "WHERE status = 'pending' AND priority != 'LOW'";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            hu_causal_world_model_t wm = {0};
            hu_online_learning_t ol = {0};
            hu_error_t wm_err = hu_causal_world_model_create(alloc, db, &wm);
            hu_error_t ol_err = hu_online_learning_create(alloc, db, 0.1, &ol);
            if (wm_err == HU_OK) {
                hu_error_t tbl_err = hu_causal_world_model_init_tables(&wm);
                if (tbl_err != HU_OK)
                    hu_log_error("cycle", NULL, "world_model table init failed: %s",
                                 hu_error_string(tbl_err));
            }
            if (ol_err == HU_OK) {
                hu_error_t tbl_err = hu_online_learning_init_tables(&ol);
                if (tbl_err != HU_OK)
                    hu_log_error("cycle", NULL, "online_learning table init failed: %s",
                                 hu_error_string(tbl_err));
            }

            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int64_t id = sqlite3_column_int64(stmt, 0);
                const char *finding = (const char *)sqlite3_column_text(stmt, 1);
                const char *suggested = (const char *)sqlite3_column_text(stmt, 2);
                const char *priority = (const char *)sqlite3_column_text(stmt, 3);
                size_t finding_len = finding ? strlen(finding) : 0;
                size_t suggested_len = suggested ? strlen(suggested) : 0;
                if (!finding)
                    finding = "";
                if (!suggested)
                    suggested = "";
                double confidence = 0.3;
                if (priority && strcmp(priority, "HIGH") == 0)
                    confidence = 0.8;
                else if (priority && strcmp(priority, "MEDIUM") == 0)
                    confidence = 0.5;

                if (wm_err == HU_OK) {
                    hu_error_t ro = hu_world_record_outcome(&wm, finding, finding_len, suggested,
                                                            suggested_len, confidence, now_ts);
                    if (ro == HU_OK)
                        result->causal_recorded++;
                }

                if (ol_err == HU_OK) {
                    hu_learning_signal_t sig = {0};
                    sig.type = HU_SIGNAL_TOOL_SUCCESS;
                    sig.magnitude = confidence;
                    sig.timestamp = now_ts;
                    size_t ctx_len = finding_len < 511 ? finding_len : 511;
                    if (ctx_len > 0) {
                        memcpy(sig.context, finding, ctx_len);
                        sig.context[ctx_len] = '\0';
                        sig.context_len = ctx_len;
                    }
                    (void)hu_online_learning_record(&ol, &sig);
                }

                sqlite3_stmt *upd = NULL;
                if (sqlite3_prepare_v2(
                        db, "UPDATE research_findings SET status = ?, acted_at = ? WHERE id = ?",
                        -1, &upd, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(upd, 1, "actioned", 8, SQLITE_STATIC);
                    sqlite3_bind_int64(upd, 2, now_ts);
                    sqlite3_bind_int64(upd, 3, id);
                    if (sqlite3_step(upd) == SQLITE_DONE)
                        result->findings_actioned++;
                    sqlite3_finalize(upd);
                }

                /* Classify and execute safe research actions */
#ifdef HU_ENABLE_FEEDS
                if (suggested_len > 0) {
                    hu_research_action_t research_action = {0};
                    if (hu_research_classify_action(suggested, suggested_len, &research_action) ==
                            HU_OK &&
                        research_action.is_safe) {
                        (void)hu_research_execute_safe(alloc, db, &research_action);
                    }
                }
#endif
            }
            sqlite3_finalize(stmt);
            if (wm_err == HU_OK)
                hu_causal_world_model_deinit(&wm);
            if (ol_err == HU_OK)
                hu_online_learning_deinit(&ol);
            if (result->findings_actioned > 0)
                steps_succeeded++;
        } else {
            hu_log_error("cycle", NULL, "intelligence/cycle: step 1 prepare failed");
        }
    }

    /* Step 1b: Counterfactual analysis — evaluate alternatives for actioned findings */
    {
        const char *cf_sql = "SELECT suggested_action FROM research_findings "
                             "WHERE status = 'actioned' AND priority = 'HIGH' LIMIT 5";
        sqlite3_stmt *cf_stmt = NULL;
        if (sqlite3_prepare_v2(db, cf_sql, -1, &cf_stmt, NULL) == SQLITE_OK) {
            hu_causal_world_model_t cf_wm = {0};
            if (hu_causal_world_model_create(alloc, db, &cf_wm) == HU_OK) {
                while (sqlite3_step(cf_stmt) == SQLITE_ROW) {
                    const char *action = (const char *)sqlite3_column_text(cf_stmt, 0);
                    if (!action)
                        continue;
                    size_t action_len = strlen(action);
                    static const char alt[] = "defer for more data";
                    hu_wm_prediction_t cf_pred = {0};
                    hu_error_t cf_err =
                        hu_world_counterfactual(&cf_wm, action, action_len, alt, sizeof(alt) - 1,
                                                "intelligence_cycle", 18, &cf_pred);
                    if (cf_err == HU_OK && cf_pred.confidence > 0.0) {
                        hu_world_record_outcome(&cf_wm, "counterfactual_analysis", 23, action,
                                                action_len, cf_pred.confidence, now_ts);
                    }
                }
                hu_causal_world_model_deinit(&cf_wm);
            }
            sqlite3_finalize(cf_stmt);
        }
    }

    /* Step 2: Populate current_events from recent feed items */
    {
        int64_t cutoff = now_ts - (24 * 3600);
        const char *sql = "SELECT DISTINCT source, content_type, substr(content, 1, 200), "
                          "url, ingested_at FROM feed_items WHERE ingested_at >= ? "
                          "ORDER BY ingested_at DESC LIMIT 20";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, cutoff);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *source = (const char *)sqlite3_column_text(stmt, 0);
                const char *content_type = (const char *)sqlite3_column_text(stmt, 1);
                const char *content = (const char *)sqlite3_column_text(stmt, 2);
                int64_t ingested_at = sqlite3_column_int64(stmt, 4);
                if (!source)
                    source = "";
                if (!content_type)
                    content_type = "feed";
                if (!content)
                    content = "";

                sqlite3_stmt *ins = NULL;
                const char *ins_sql = "INSERT OR IGNORE INTO current_events "
                                      "(topic, summary, source, published_at, relevance) "
                                      "VALUES (?, ?, ?, ?, 0.5)";
                if (sqlite3_prepare_v2(db, ins_sql, -1, &ins, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(ins, 1, content_type, -1, SQLITE_STATIC);
                    sqlite3_bind_text(ins, 2, content, -1, SQLITE_STATIC);
                    sqlite3_bind_text(ins, 3, source, -1, SQLITE_STATIC);
                    sqlite3_bind_int64(ins, 4, ingested_at);
                    if (sqlite3_step(ins) == SQLITE_DONE)
                        result->events_recorded++;
                    sqlite3_finalize(ins);
                }
            }
            sqlite3_finalize(stmt);
            if (result->events_recorded > 0)
                steps_succeeded++;
        } else {
            hu_log_error("cycle", NULL,
                         "intelligence/cycle: step 2 prepare failed (feed_items may not exist)");
        }
    }

    /* Step 3: Extract general lessons from actioned findings */
    {
        const char *sql =
            "SELECT suggested_action FROM research_findings WHERE status = 'actioned'";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            word_count_t counts[MAX_WORDS];
            int n = 0;
            memset(counts, 0, sizeof(counts));
            while (sqlite3_step(stmt) == SQLITE_ROW && n < MAX_WORDS) {
                const char *action = (const char *)sqlite3_column_text(stmt, 0);
                if (action)
                    extract_words_from_text(action, strlen(action), counts, &n);
            }
            sqlite3_finalize(stmt);

            sqlite3_stmt *ins = NULL;
            const char *ins_sql =
                "INSERT OR IGNORE INTO general_lessons "
                "(lesson, confidence, source_count, first_learned, last_confirmed) "
                "VALUES (?, 0.5, ?, ?, ?)";
            if (sqlite3_prepare_v2(db, ins_sql, -1, &ins, NULL) == SQLITE_OK) {
                for (int i = 0; i < n; i++) {
                    if (counts[i].count >= 3) {
                        char lesson[256];
                        int ln = snprintf(lesson, sizeof(lesson),
                                          "Recurring topic: '%s' appears in %d actioned findings",
                                          counts[i].word, counts[i].count);
                        if (ln > 0 && (size_t)ln < sizeof(lesson)) {
                            sqlite3_bind_text(ins, 1, lesson, ln, SQLITE_STATIC);
                            sqlite3_bind_int(ins, 2, counts[i].count);
                            sqlite3_bind_int64(ins, 3, now_ts);
                            sqlite3_bind_int64(ins, 4, now_ts);
                            if (sqlite3_step(ins) == SQLITE_DONE)
                                result->lessons_extracted++;
                            sqlite3_reset(ins);
                        }
                    }
                }
                sqlite3_finalize(ins);
            }
            if (result->lessons_extracted > 0)
                steps_succeeded++;
        } else {
            hu_log_error("cycle", NULL, "intelligence/cycle: step 3 prepare failed");
        }
    }

    /* Step 3b: Distill recurring experience patterns into lessons */
    {
        hu_error_t tbl_err = hu_distiller_init_tables(db);
        if (tbl_err != HU_OK)
            hu_log_error("cycle", NULL, "distiller table init failed: %s",
                         hu_error_string(tbl_err));
        size_t distilled = 0;
        if (hu_experience_distill(alloc, db, 2, now_ts, &distilled) == HU_OK)
            result->lessons_extracted += distilled;
    }

    /* Step 4: Learn values from HIGH findings */
    {
        hu_value_engine_t ve = {0};
        if (hu_value_engine_create(alloc, db, &ve) == HU_OK) {
            hu_error_t tbl_err = hu_value_init_tables(&ve);
            if (tbl_err != HU_OK)
                hu_log_error("cycle", NULL, "value table init failed: %s",
                             hu_error_string(tbl_err));
            const char *sql = "SELECT finding, suggested_action FROM research_findings "
                              "WHERE priority = 'HIGH' AND status = 'actioned'";
            sqlite3_stmt *stmt = NULL;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char *finding = (const char *)sqlite3_column_text(stmt, 0);
                    const char *action = (const char *)sqlite3_column_text(stmt, 1);
                    if (!action)
                        continue;
                    size_t action_len = strlen(action);
                    char word[WORD_BUF_SIZE];
                    size_t word_len = 0;
                    extract_first_significant_word(action, action_len, word, sizeof(word),
                                                   &word_len);
                    if (word_len == 0)
                        continue;
                    size_t finding_len = finding ? strlen(finding) : 0;
                    if (!finding)
                        finding = "";
                    hu_error_t err = hu_value_learn_from_correction(&ve, word, word_len, finding,
                                                                    finding_len, 1.0, now_ts);
                    if (err == HU_OK)
                        result->values_learned++;
                }
                sqlite3_finalize(stmt);
            }
            hu_value_engine_deinit(&ve);
            if (result->values_learned > 0)
                steps_succeeded++;
        } else {
            hu_log_error("cycle", NULL, "intelligence/cycle: step 4 value engine create failed");
        }
    }

    /* Step 5: Run reflection (weekly, extract lessons, daily) */
    {
        hu_reflection_engine_t re = {0};
        if (hu_reflection_engine_create(alloc, db, &re) == HU_OK) {
            hu_error_t rw = hu_reflection_weekly(&re, now_ts);
            hu_error_t rel = hu_reflection_extract_general_lessons(&re, now_ts);
            hu_error_t rd = hu_reflection_daily(&re, now_ts);
            if (rw == HU_OK || rel == HU_OK || rd == HU_OK) {
                result->skills_updated = 1;
                steps_succeeded++;
            }
            hu_reflection_engine_deinit(&re);
        } else {
            hu_log_error("cycle", NULL,
                         "intelligence/cycle: step 5 reflection engine create failed");
        }
    }

    /* Step 6: Run meta-learning optimization */
    {
        hu_meta_params_t params = {0};
        if (hu_meta_learning_optimize(db, &params) == HU_OK)
            steps_succeeded++;
        /* Meta-params now available for future refinement.
         * params.default_confidence_threshold could adjust step 1's confidence mapping.
         * params.refinement_frequency_weeks and params.discovery_min_feedback_count
         * are used by the reflection engine's scheduling in daemon.c. */
    }

    /* Step 7: Seed behavioral feedback from successful cycle run */
    if (result->findings_actioned > 0 || result->events_recorded > 0) {
        sqlite3_stmt *fb = NULL;
        const char *fb_sql = "INSERT INTO behavioral_feedback "
                             "(behavior_type, contact_id, signal, context, timestamp) "
                             "VALUES (?, 'system', ?, ?, ?)";
        if (sqlite3_prepare_v2(db, fb_sql, -1, &fb, NULL) == SQLITE_OK) {
            sqlite3_bind_text(fb, 1, "research_cycle", -1, SQLITE_STATIC);
            sqlite3_bind_text(fb, 2, "positive", -1, SQLITE_STATIC);
            char ctx[128];
            int cl = snprintf(ctx, sizeof(ctx), "Cycle processed %zu findings, %zu events",
                              result->findings_actioned, result->events_recorded);
            sqlite3_bind_text(fb, 3, ctx, cl, SQLITE_STATIC);
            sqlite3_bind_int64(fb, 4, now_ts);
            if (sqlite3_step(fb) == SQLITE_DONE)
                steps_succeeded++;
            sqlite3_finalize(fb);
        }
    }

    /* Step 8: Populate opinions from HIGH-priority findings.
     * Scoped to findings actioned THIS cycle, exactly as Step 9 below already is.
     * Without the acted_at bound this re-inserted every historical HIGH finding on
     * every cycle: research_findings keeps status='actioned' forever, and the
     * `INSERT OR IGNORE` cannot dedup because `opinions` has NO unique constraint
     * (only `id AUTOINCREMENT` and a non-unique idx_opinions_topic), so OR IGNORE
     * never fires. Measured 2026-08-02: 9,533,051 rows over 2,962 distinct
     * (topic, position) pairs -- 3,218x duplication, 4.3 GB, growing ~62k rows per
     * cycle. The persona reads this table via the live [evolved_opinion] directive,
     * so the duplicates were also injected into real replies. */
    {
        const char *sql = "SELECT finding, suggested_action FROM research_findings "
                          "WHERE priority = 'HIGH' AND status = 'actioned' "
                          "AND acted_at >= ?";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, now_ts);
            sqlite3_stmt *ins = NULL;
            const char *ins_sql = "INSERT OR IGNORE INTO opinions "
                                  "(topic, position, confidence, first_expressed, last_expressed) "
                                  "VALUES (?, ?, 0.7, ?, ?)";
            int opinions_stored = 0;
            if (sqlite3_prepare_v2(db, ins_sql, -1, &ins, NULL) == SQLITE_OK) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char *finding = (const char *)sqlite3_column_text(stmt, 0);
                    const char *action = (const char *)sqlite3_column_text(stmt, 1);
                    if (!finding || !action)
                        continue;
                    sqlite3_bind_text(ins, 1, finding, -1, SQLITE_STATIC);
                    sqlite3_bind_text(ins, 2, action, -1, SQLITE_STATIC);
                    sqlite3_bind_int64(ins, 3, now_ts);
                    sqlite3_bind_int64(ins, 4, now_ts);
                    if (sqlite3_step(ins) == SQLITE_DONE)
                        opinions_stored++;
                    sqlite3_reset(ins);
                }
                sqlite3_finalize(ins);
            }
            sqlite3_finalize(stmt);
            if (opinions_stored > 0)
                steps_succeeded++;
        }
    }

    /* Step 9: Create skills from recurring findings topics.
     * Scoped to findings actioned THIS cycle (Step 1 stamps acted_at=now_ts):
     * research_findings rows keep status='actioned' forever, so an unscoped
     * scan re-records a "cycle run" positive for every historical source on
     * every cycle — 5,362 sources x ~280 cycles/day of contentless rows. */
    {
        const char *sql = "SELECT source, suggested_action FROM research_findings "
                          "WHERE status = 'actioned' AND acted_at >= ? GROUP BY source";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, now_ts);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *source = (const char *)sqlite3_column_text(stmt, 0);
                const char *action = (const char *)sqlite3_column_text(stmt, 1);
                if (!source || !action)
                    continue;
                size_t act_len = strlen(action);

                char skill_name[128];
                int sn = snprintf(skill_name, sizeof(skill_name), "monitor_%s", source);
                if (sn < 0 || (size_t)sn >= sizeof(skill_name))
                    continue;

                hu_skill_t existing = {0};
                if (hu_skill_get_by_name(alloc, db, skill_name, (size_t)sn, &existing) == HU_OK &&
                    existing.id != 0) {
                    hu_skill_record_attempt(db, existing.id, "system", 6, now_ts, "positive", 8,
                                            "cycle run", 9, action, act_len < 256 ? act_len : 256,
                                            NULL);
                    hu_skill_update_success_rate(db, existing.id, existing.attempts + 1,
                                                 existing.successes + 1);
                    result->skills_updated++;
                    continue;
                }

                char strategy[512];
                int stl =
                    snprintf(strategy, sizeof(strategy), "Monitor %s for AI developments: %.*s",
                             source, (int)(act_len < 300 ? act_len : 300), action);
                if (stl < 0 || (size_t)stl >= sizeof(strategy))
                    continue;

                int64_t skill_id = 0;
                hu_error_t err = hu_skill_insert(alloc, db, skill_name, (size_t)sn, "research", 8,
                                                 "system", 6, NULL, 0, strategy, (size_t)stl,
                                                 "intelligence_cycle", 18, 0, now_ts, &skill_id);
                if (err == HU_OK)
                    result->skills_updated++;
            }
            sqlite3_finalize(stmt);
        }
    }

    /* Step 10: Log cognitive load (how much was processed) */
    {
        size_t total_processed = result->findings_actioned + result->events_recorded +
                                 result->lessons_extracted + result->values_learned;
        sqlite3_stmt *cl = NULL;
        const char *cl_sql = "INSERT INTO cognitive_load_log "
                             "(capacity, conversation_depth, hour_of_day, day_of_week, "
                             "physical_state, recorded_at) "
                             "VALUES (?, ?, ?, ?, ?, ?)";
        if (sqlite3_prepare_v2(db, cl_sql, -1, &cl, NULL) == SQLITE_OK) {
            sqlite3_bind_int(cl, 1, (int)total_processed);
            sqlite3_bind_int(cl, 2, (int)result->findings_actioned);
            struct tm *t = localtime(&(time_t){now_ts});
            sqlite3_bind_int(cl, 3, t ? t->tm_hour : 0);
            sqlite3_bind_int(cl, 4, t ? t->tm_wday : 0);
            sqlite3_bind_text(cl, 5, "research_cycle", -1, SQLITE_STATIC);
            sqlite3_bind_int64(cl, 6, now_ts);
            if (sqlite3_step(cl) == SQLITE_DONE)
                steps_succeeded++;
            sqlite3_finalize(cl);
        }
    }

    /* Step 11: Record growth milestone if findings were actioned */
    if (result->findings_actioned > 0) {
        char after_buf[128];
        int n =
            snprintf(after_buf, sizeof(after_buf), "actioned %zu findings, extracted %zu lessons",
                     result->findings_actioned, result->lessons_extracted);
        if (n > 0 && (size_t)n < sizeof(after_buf)) {
            sqlite3_stmt *stmt = NULL;
            const char *sql = "INSERT INTO growth_milestones "
                              "(contact_id, topic, before_state, after_state, created_at) "
                              "VALUES ('system', 'research_cycle', 'pending findings', ?, ?)";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, after_buf, n, SQLITE_STATIC);
                sqlite3_bind_int64(stmt, 2, now_ts);
                if (sqlite3_step(stmt) == SQLITE_DONE)
                    steps_succeeded++;
                sqlite3_finalize(stmt);
            }
        }
    }

    /* Step 12: Update strategy weights from cycle outcomes */
    {
        hu_online_learning_t ol = {0};
        if (hu_online_learning_create(alloc, db, 0.1, &ol) == HU_OK) {
            hu_error_t tbl_err = hu_online_learning_init_tables(&ol);
            if (tbl_err != HU_OK)
                hu_log_error("cycle", NULL, "online_learning table init failed: %s",
                             hu_error_string(tbl_err));
            if (result->findings_actioned > 0)
                (void)hu_online_learning_update_weight(&ol, "research_findings", 18, 1.0, now_ts);
            if (result->events_recorded > 0)
                (void)hu_online_learning_update_weight(&ol, "feed_monitoring", 15, 0.8, now_ts);
            if (result->lessons_extracted > 0)
                (void)hu_online_learning_update_weight(&ol, "lesson_extraction", 17, 0.7, now_ts);
            if (result->skills_updated > 0)
                (void)hu_online_learning_update_weight(&ol, "skill_creation", 14, 0.6, now_ts);
            hu_online_learning_deinit(&ol);
            steps_succeeded++;
        }
    }

    /* Step 13: Apply matching skills to findings; record negative for unmatched */
    {
        hu_skill_t *active = NULL;
        size_t active_count = 0;
        if (hu_skill_load_active(alloc, db, "system", 6, &active, &active_count) == HU_OK &&
            active && active_count > 0) {
            bool *matched = (bool *)alloc->alloc(alloc->ctx, active_count * sizeof(bool));
            if (matched)
                memset(matched, 0, active_count * sizeof(bool));

            const char *sql = "SELECT id, finding FROM research_findings WHERE status = 'actioned' "
                              "ORDER BY created_at DESC LIMIT 10";
            sqlite3_stmt *stmt = NULL;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char *finding = (const char *)sqlite3_column_text(stmt, 1);
                    if (!finding)
                        continue;
                    size_t flen = strlen(finding);
                    for (size_t s = 0; s < active_count; s++) {
                        if (active[s].strategy_len == 0)
                            continue;
                        char *mon = strstr(active[s].strategy, "Monitor ");
                        if (!mon)
                            continue;
                        char source_prefix[64] = {0};
                        size_t si = 8;
                        size_t sp = 0;
                        while (si < active[s].strategy_len && active[s].strategy[si] != ' ' &&
                               sp < sizeof(source_prefix) - 1) {
                            source_prefix[sp++] = active[s].strategy[si++];
                        }
                        if (sp > 0 && strstr(finding, source_prefix)) {
                            int64_t attempt_id = 0;
                            hu_skill_record_attempt(db, active[s].id, "system", 6, now_ts,
                                                    "positive", 8, "finding matched", 15, finding,
                                                    flen < 256 ? flen : 256, &attempt_id);
                            hu_skill_update_success_rate(db, active[s].id, active[s].attempts + 1,
                                                         active[s].successes + 1);
                            if (matched)
                                matched[s] = true;
                        }
                    }
                }
                sqlite3_finalize(stmt);
            }

            /* Record negative signals for skills that had no matching findings.
             * Aggregate-only: no skill_attempts row per negative — one row per
             * skill per cycle is O(skills x cycles) unbounded growth (73.6M rows
             * / 9.9GB by 2026-07), and the skills.attempts counter already
             * carries the signal. */
            if (matched) {
                for (size_t s = 0; s < active_count; s++) {
                    if (!matched[s] && active[s].strategy_len > 0) {
                        hu_skill_update_success_rate(db, active[s].id, active[s].attempts + 1,
                                                     active[s].successes);
                    }
                }
                alloc->free(alloc->ctx, matched, active_count * sizeof(bool));
            }

            hu_skill_free(alloc, active, active_count);
            steps_succeeded++;
        }
    }

    return steps_succeeded > 0 ? HU_OK : HU_ERR_IO;
}

#endif /* HU_ENABLE_SQLITE */
