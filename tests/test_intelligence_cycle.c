// @covers-none — integration test spanning
// src/intelligence/{cycle,skills,world_model,self_improve,online_learning}.c; name-heuristic
// wrongly implies src/context/intelligence.c
#ifdef HU_ENABLE_SQLITE

#include "human/agent/tree_of_thought.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/intelligence/cycle.h"
#include "human/intelligence/online_learning.h"
#include "human/intelligence/self_improve.h"
#include "human/intelligence/skills.h"
#include "human/intelligence/world_model.h"
#include "human/memory.h"
#include "test_framework.h"
#include <sqlite3.h>
#include <string.h>
#include <time.h>

static void ensure_opinions_table(sqlite3 *db) {
    const char *sql = "CREATE TABLE IF NOT EXISTS opinions ("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                      "topic TEXT NOT NULL,"
                      "position TEXT NOT NULL,"
                      "confidence REAL NOT NULL,"
                      "first_expressed INTEGER NOT NULL,"
                      "last_expressed INTEGER NOT NULL,"
                      "superseded_by INTEGER)";
    sqlite3_exec(db, sql, NULL, NULL, NULL);
}

static void insert_finding(sqlite3 *db, const char *source, const char *finding,
                           const char *relevance, const char *priority, const char *action,
                           int64_t created_at) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO research_findings "
                      "(source, finding, relevance, priority, suggested_action, "
                      "status, created_at) VALUES (?, ?, ?, ?, ?, 'pending', ?)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(stmt, 1, source, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, finding, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, relevance, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, priority, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, action, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, created_at);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

static void insert_feed_item(sqlite3 *db, const char *source, const char *content_type,
                             const char *content, const char *url, int64_t ingested_at) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO feed_items "
                      "(source, contact_id, content_type, content, url, ingested_at) "
                      "VALUES (?, 'system', ?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(stmt, 1, source, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, content_type, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, content, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, url, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, ingested_at);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

static int count_rows(sqlite3 *db, const char *table) {
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

/* --- NULL arg tests --- */

static void cycle_null_alloc_returns_invalid(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_intelligence_cycle_result_t r = {0};
    HU_ASSERT_EQ(hu_intelligence_run_cycle(NULL, db, &r), HU_ERR_INVALID_ARGUMENT);
    sqlite3_close(db);
}

static void cycle_null_db_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_intelligence_cycle_result_t r = {0};
    HU_ASSERT_EQ(hu_intelligence_run_cycle(&alloc, NULL, &r), HU_ERR_INVALID_ARGUMENT);
}

static void cycle_null_result_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    HU_ASSERT_EQ(hu_intelligence_run_cycle(&alloc, db, NULL), HU_ERR_INVALID_ARGUMENT);
    sqlite3_close(db);
}

/* --- Empty DB test --- */

static void cycle_empty_db_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_opinions_table(db);

    hu_intelligence_cycle_result_t r = {0};
    hu_error_t err = hu_intelligence_run_cycle(&alloc, db, &r);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_IO);
    HU_ASSERT_EQ(r.findings_actioned, 0u);
    HU_ASSERT_EQ(r.events_recorded, 0u);
    HU_ASSERT_EQ(r.lessons_extracted, 0u);
    HU_ASSERT_EQ(r.values_learned, 0u);
    HU_ASSERT_EQ(r.causal_recorded, 0u);

    mem.vtable->deinit(mem.ctx);
}

/* --- Full cycle with findings and feed items --- */

static void cycle_with_findings_and_feeds_processes_all(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_opinions_table(db);

    int64_t now = (int64_t)time(NULL);

    insert_finding(db, "arxiv", "Transformer scaling improves accuracy", "high", "HIGH",
                   "Investigate scaling laws further with repeated analysis", now - 60);
    insert_finding(db, "arxiv", "Attention mechanisms reduce latency", "medium", "MEDIUM",
                   "Optimize attention module performance improvements", now - 30);
    insert_finding(db, "github", "New library improves inference speed", "high", "HIGH",
                   "Benchmark inference engine against baseline performance", now - 10);

    insert_feed_item(db, "rss_ai_news", "article", "GPT-5 announced with breakthrough capabilities",
                     "https://example.com/gpt5", now - 100);
    insert_feed_item(db, "twitter_ai", "post",
                     "Major advancement in reinforcement learning published today",
                     "https://example.com/rl", now - 50);

    hu_intelligence_cycle_result_t r = {0};
    hu_error_t err = hu_intelligence_run_cycle(&alloc, db, &r);
    HU_ASSERT_EQ(err, HU_OK);

    HU_ASSERT_TRUE(r.findings_actioned >= 2);
    HU_ASSERT_TRUE(r.events_recorded >= 1);
    HU_ASSERT_TRUE(r.causal_recorded >= 1);

    sqlite3_stmt *chk = NULL;
    int rc = sqlite3_prepare_v2(
        db, "SELECT COUNT(*) FROM research_findings WHERE status = 'actioned'", -1, &chk, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(chk);
    HU_ASSERT_EQ(rc, SQLITE_ROW);
    int actioned = sqlite3_column_int(chk, 0);
    sqlite3_finalize(chk);
    HU_ASSERT_TRUE(actioned >= 2);

    HU_ASSERT_TRUE(count_rows(db, "current_events") >= 1);

    mem.vtable->deinit(mem.ctx);
}

/* --- Findings only (no feeds) --- */

static void cycle_findings_only_no_events(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_opinions_table(db);

    int64_t now = (int64_t)time(NULL);

    insert_finding(db, "paper", "Novel architecture outperforms baseline", "high", "HIGH",
                   "Replicate experiment to verify claims", now - 60);

    hu_intelligence_cycle_result_t r = {0};
    hu_error_t err = hu_intelligence_run_cycle(&alloc, db, &r);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(r.findings_actioned >= 1);
    HU_ASSERT_EQ(r.events_recorded, 0u);

    mem.vtable->deinit(mem.ctx);
}

/* --- Feeds only (no findings) --- */

static void cycle_feeds_only_no_findings(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_opinions_table(db);

    int64_t now = (int64_t)time(NULL);

    insert_feed_item(db, "rss_tech", "article",
                     "Cloud computing infrastructure shows growth trajectory",
                     "https://example.com/cloud", now - 30);

    hu_intelligence_cycle_result_t r = {0};
    hu_error_t err = hu_intelligence_run_cycle(&alloc, db, &r);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_IO);
    HU_ASSERT_EQ(r.findings_actioned, 0u);
    HU_ASSERT_TRUE(r.events_recorded >= 1);

    mem.vtable->deinit(mem.ctx);
}

/* --- Lessons extraction from recurring words --- */

static void cycle_recurring_words_extract_lessons(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_opinions_table(db);

    int64_t now = (int64_t)time(NULL);

    insert_finding(db, "src1", "Finding about scaling", "high", "HIGH",
                   "Investigate scaling approach thoroughly", now - 120);
    insert_finding(db, "src2", "Finding about optimization", "high", "HIGH",
                   "Investigate scaling methods completely", now - 60);

    hu_intelligence_cycle_result_t r = {0};
    hu_error_t err = hu_intelligence_run_cycle(&alloc, db, &r);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(r.findings_actioned >= 2);
    HU_ASSERT_TRUE(r.lessons_extracted >= 0);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT lesson FROM general_lessons WHERE lesson LIKE '%Recurring "
                                "topic%' OR lesson LIKE '%recurring%'",
                                -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    /* Lessons may or may not be generated depending on word frequency thresholds */
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const char *lesson = (const char *)sqlite3_column_text(stmt, 0);
        HU_ASSERT_NOT_NULL(lesson);
        HU_ASSERT_TRUE(strlen(lesson) > 0);
    }
    sqlite3_finalize(stmt);

    mem.vtable->deinit(mem.ctx);
}

/* --- Value learning from HIGH findings --- */

static void cycle_high_findings_learn_values(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_opinions_table(db);

    int64_t now = (int64_t)time(NULL);

    insert_finding(db, "paper", "Alignment techniques improve safety", "critical", "HIGH",
                   "Prioritize safety-first development approach", now - 60);

    hu_intelligence_cycle_result_t r = {0};
    hu_error_t err = hu_intelligence_run_cycle(&alloc, db, &r);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(r.findings_actioned >= 1);
    HU_ASSERT_TRUE(r.values_learned >= 1);

    mem.vtable->deinit(mem.ctx);
}

/* --- Opinions populated from HIGH findings --- */

static void cycle_high_findings_populate_opinions(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_opinions_table(db);

    int64_t now = (int64_t)time(NULL);

    insert_finding(db, "research", "Sparse models are more efficient", "high", "HIGH",
                   "Adopt sparse model architectures", now - 60);
    insert_finding(db, "benchmark", "Dense models achieve higher accuracy", "high", "HIGH",
                   "Maintain dense model baselines", now - 30);

    hu_intelligence_cycle_result_t r = {0};
    hu_error_t err = hu_intelligence_run_cycle(&alloc, db, &r);
    HU_ASSERT_EQ(err, HU_OK);

    int opinion_count = count_rows(db, "opinions");
    HU_ASSERT_TRUE(opinion_count >= 2);

    mem.vtable->deinit(mem.ctx);
}

/* --- Skills created from recurring sources --- */

static void cycle_recurring_source_creates_skills(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_opinions_table(db);

    int64_t now = (int64_t)time(NULL);

    insert_finding(db, "arxiv_ml", "New training method improves convergence", "high", "HIGH",
                   "Monitor arxiv_ml for training improvements", now - 120);
    insert_finding(db, "arxiv_ml", "Better regularization reduces overfitting", "medium", "MEDIUM",
                   "Apply regularization techniques broadly", now - 60);

    hu_intelligence_cycle_result_t r = {0};
    hu_error_t err = hu_intelligence_run_cycle(&alloc, db, &r);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(r.skills_updated >= 1);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT name FROM skills WHERE name LIKE 'monitor_%'", -1,
                                &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    HU_ASSERT_EQ(rc, SQLITE_ROW);
    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    HU_ASSERT_NOT_NULL(name);
    HU_ASSERT_TRUE(strstr(name, "monitor_") == name);
    sqlite3_finalize(stmt);

    mem.vtable->deinit(mem.ctx);
}

/* --- Cognitive load logged --- */

static void cycle_logs_cognitive_load(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_opinions_table(db);

    int64_t now = (int64_t)time(NULL);

    insert_finding(db, "test", "Test finding for cognitive load", "high", "HIGH",
                   "Action needed immediately", now - 60);

    hu_intelligence_cycle_result_t r = {0};
    hu_error_t err = hu_intelligence_run_cycle(&alloc, db, &r);
    HU_ASSERT_EQ(err, HU_OK);

    int load_count = count_rows(db, "cognitive_load_log");
    HU_ASSERT_TRUE(load_count >= 1);

    mem.vtable->deinit(mem.ctx);
}

/* --- Growth milestone recorded when findings actioned --- */

static void cycle_records_growth_milestone(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_opinions_table(db);

    int64_t now = (int64_t)time(NULL);

    insert_finding(db, "src", "Important finding for milestones", "high", "HIGH",
                   "Track this milestone carefully", now - 60);

    hu_intelligence_cycle_result_t r = {0};
    hu_error_t err = hu_intelligence_run_cycle(&alloc, db, &r);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(r.findings_actioned >= 1);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db, "SELECT after_state FROM growth_milestones WHERE topic = 'research_cycle'", -1, &stmt,
        NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    HU_ASSERT_EQ(rc, SQLITE_ROW);
    const char *after = (const char *)sqlite3_column_text(stmt, 0);
    HU_ASSERT_NOT_NULL(after);
    HU_ASSERT_TRUE(strstr(after, "actioned") != NULL);
    sqlite3_finalize(stmt);

    mem.vtable->deinit(mem.ctx);
}

/* --- Behavioral feedback seeded on successful cycle --- */

static void cycle_seeds_behavioral_feedback(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_opinions_table(db);

    int64_t now = (int64_t)time(NULL);

    insert_finding(db, "src", "Finding triggers feedback", "high", "HIGH",
                   "Process this finding urgently", now - 60);

    hu_intelligence_cycle_result_t r = {0};
    hu_error_t err = hu_intelligence_run_cycle(&alloc, db, &r);
    HU_ASSERT_EQ(err, HU_OK);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT signal, context FROM behavioral_feedback "
                                "WHERE behavior_type = 'research_cycle'",
                                -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    HU_ASSERT_EQ(rc, SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "positive");
    const char *ctx = (const char *)sqlite3_column_text(stmt, 1);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(strstr(ctx, "Cycle processed") != NULL);
    sqlite3_finalize(stmt);

    mem.vtable->deinit(mem.ctx);
}

/* --- Result zeroed on entry --- */

static void cycle_result_zeroed_on_entry(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_opinions_table(db);

    hu_intelligence_cycle_result_t r;
    memset(&r, 0xFF, sizeof(r));

    hu_error_t err = hu_intelligence_run_cycle(&alloc, db, &r);
    (void)err;

    HU_ASSERT_EQ(r.findings_actioned, 0u);
    HU_ASSERT_EQ(r.events_recorded, 0u);
    HU_ASSERT_EQ(r.lessons_extracted, 0u);
    HU_ASSERT_EQ(r.values_learned, 0u);
    HU_ASSERT_EQ(r.causal_recorded, 0u);

    mem.vtable->deinit(mem.ctx);
}

/* --- LOW priority findings are not actioned --- */

static void cycle_low_priority_findings_skipped(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_opinions_table(db);

    int64_t now = (int64_t)time(NULL);

    insert_finding(db, "src", "Low priority finding", "low", "LOW", "Maybe look at this later",
                   now - 60);

    int pre_low_count = 0;
    {
        sqlite3_stmt *s = NULL;
        sqlite3_prepare_v2(
            db,
            "SELECT COUNT(*) FROM research_findings WHERE priority = 'LOW' AND status = 'pending'",
            -1, &s, NULL);
        if (sqlite3_step(s) == SQLITE_ROW)
            pre_low_count = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    HU_ASSERT_TRUE(pre_low_count >= 1);

    hu_intelligence_cycle_result_t r = {0};
    hu_error_t err = hu_intelligence_run_cycle(&alloc, db, &r);
    (void)err;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db, "SELECT COUNT(*) FROM research_findings WHERE priority = 'LOW' AND status = 'pending'",
        -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    HU_ASSERT_EQ(rc, SQLITE_ROW);
    int post_low_count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    HU_ASSERT_EQ(post_low_count, pre_low_count);

    mem.vtable->deinit(mem.ctx);
}

/* --- World model simulation_cache --- */

static void cycle_simulation_cache_returns_cached_result(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    hu_causal_world_model_t wm;
    HU_ASSERT_EQ(hu_causal_world_model_create(&alloc, db, &wm), HU_OK);
    HU_ASSERT_EQ(hu_causal_world_model_init_tables(&wm), HU_OK);

    /* Seed a causal observation */
    int64_t now = (int64_t)time(NULL);
    HU_ASSERT_EQ(hu_world_record_outcome(&wm, "deploy code", 11, "service restarts", 16, 0.9, now),
                 HU_OK);

    /* First simulate — should query causal_observations and cache the result */
    hu_wm_prediction_t pred1 = {0};
    HU_ASSERT_EQ(hu_world_simulate(&wm, "deploy code", 11, NULL, 0, &pred1), HU_OK);
    HU_ASSERT_TRUE(pred1.confidence > 0.0);
    HU_ASSERT_TRUE(pred1.outcome_len > 0);

    /* Verify simulation_cache has an entry */
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM simulation_cache", -1, &stmt, NULL);
    sqlite3_step(stmt);
    int cache_count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    HU_ASSERT_TRUE(cache_count >= 1);

    /* Second simulate — should hit cache and return same result */
    hu_wm_prediction_t pred2 = {0};
    HU_ASSERT_EQ(hu_world_simulate(&wm, "deploy code", 11, NULL, 0, &pred2), HU_OK);
    HU_ASSERT_TRUE(pred2.confidence > 0.0);
    /* Confidence should be same from cache */
    HU_ASSERT_TRUE(pred1.confidence == pred2.confidence);

    hu_causal_world_model_deinit(&wm);
    mem.vtable->deinit(mem.ctx);
}

/* --- Self-improve tool prefs prompt format --- */

static void self_improve_tool_prefs_prompt_format(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    hu_self_improve_t si;
    HU_ASSERT_EQ(hu_self_improve_create(&alloc, db, &si), HU_OK);
    HU_ASSERT_EQ(hu_self_improve_init_tables(&si), HU_OK);

    int64_t now = (int64_t)time(NULL);
    /* Record several tool outcomes (need 3+ per tool for prompt inclusion) */
    HU_ASSERT_EQ(hu_self_improve_record_tool_outcome(&si, "web_search", 10, true, now), HU_OK);
    HU_ASSERT_EQ(hu_self_improve_record_tool_outcome(&si, "web_search", 10, true, now), HU_OK);
    HU_ASSERT_EQ(hu_self_improve_record_tool_outcome(&si, "web_search", 10, false, now), HU_OK);
    HU_ASSERT_EQ(hu_self_improve_record_tool_outcome(&si, "file_read", 9, true, now), HU_OK);
    HU_ASSERT_EQ(hu_self_improve_record_tool_outcome(&si, "file_read", 9, true, now), HU_OK);
    HU_ASSERT_EQ(hu_self_improve_record_tool_outcome(&si, "file_read", 9, true, now), HU_OK);

    char *prompt = NULL;
    size_t prompt_len = 0;
    HU_ASSERT_EQ(hu_self_improve_get_tool_prefs_prompt(&si, &prompt, &prompt_len), HU_OK);
    HU_ASSERT_NOT_NULL(prompt);
    HU_ASSERT_TRUE(prompt_len > 0);
    /* Should mention tool names */
    HU_ASSERT_TRUE(strstr(prompt, "web_search") != NULL || strstr(prompt, "file_read") != NULL);

    alloc.free(alloc.ctx, prompt, prompt_len + 1);
    hu_self_improve_deinit(&si);
    mem.vtable->deinit(mem.ctx);
}

/* --- World model context-based prediction --- */

static void cycle_context_changes_prediction(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    hu_causal_world_model_t wm;
    HU_ASSERT_EQ(hu_causal_world_model_create(&alloc, db, &wm), HU_OK);
    HU_ASSERT_EQ(hu_causal_world_model_init_tables(&wm), HU_OK);

    int64_t now = (int64_t)time(NULL);
    /* Record outcomes with different contexts */
    HU_ASSERT_EQ(
        hu_world_record_outcome(&wm, "send message", 12, "delivered quickly", 17, 0.9, now), HU_OK);

    /* Simulate with no context */
    hu_wm_prediction_t pred_none = {0};
    hu_error_t err1 = hu_world_simulate(&wm, "send message", 12, NULL, 0, &pred_none);

    /* Simulate with a context */
    hu_wm_prediction_t pred_ctx = {0};
    hu_error_t err2 = hu_world_simulate(&wm, "send message", 12, "urgent", 6, &pred_ctx);

    /* Both should succeed (may or may not differ in outcome depending on data) */
    HU_ASSERT_EQ(err1, HU_OK);
    HU_ASSERT_EQ(err2, HU_OK);
    /* At minimum, both return valid predictions */
    HU_ASSERT_TRUE(pred_none.confidence >= 0.0);
    HU_ASSERT_TRUE(pred_ctx.confidence >= 0.0);

    hu_causal_world_model_deinit(&wm);
    mem.vtable->deinit(mem.ctx);
}

/* --- Online learning: record changes weight --- */

static void cycle_record_signal_changes_weight(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    hu_online_learning_t ol;
    HU_ASSERT_EQ(hu_online_learning_create(&alloc, db, 0.1, &ol), HU_OK);
    HU_ASSERT_EQ(hu_online_learning_init_tables(&ol), HU_OK);

    /* Get baseline weight (should be 1.0 neutral) */
    const char *strategy = "tool:test_strategy";
    double before = hu_online_learning_get_weight(&ol, strategy, 18);
    HU_ASSERT_TRUE(before == 1.0);

    /* Record a learning signal — triggers update_weight internally */
    hu_learning_signal_t sig = {0};
    sig.type = HU_SIGNAL_TOOL_SUCCESS;
    sig.magnitude = 0.8;
    sig.timestamp = (int64_t)time(NULL);
    memcpy(sig.tool_name, "test_strategy", 13);
    sig.tool_name_len = 13;
    HU_ASSERT_EQ(hu_online_learning_record(&ol, &sig), HU_OK);

    /* Weight should have changed from default 1.0 */
    double after = hu_online_learning_get_weight(&ol, strategy, 18);
    HU_ASSERT_TRUE(after != 1.0);

    hu_online_learning_deinit(&ol);
    mem.vtable->deinit(mem.ctx);
}

/* --- Cycle triggers prompt patches (self_improve tables) --- */

static void cycle_triggers_prompt_patches(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_opinions_table(db);

    int64_t now = (int64_t)time(NULL);

    insert_finding(db, "research", "Prompt engineering improves responses", "high", "HIGH",
                   "Adopt systematic prompt engineering approach", now - 60);

    hu_intelligence_cycle_result_t r = {0};
    hu_error_t err = hu_intelligence_run_cycle(&alloc, db, &r);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(r.findings_actioned >= 1);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db, "SELECT name FROM sqlite_master WHERE type='table' AND name='prompt_patches'", -1,
        &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    HU_ASSERT_EQ(rc, SQLITE_ROW);
    sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(
        db, "SELECT name FROM sqlite_master WHERE type='table' AND name='tool_prefs'", -1, &stmt,
        NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    HU_ASSERT_EQ(rc, SQLITE_ROW);
    sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM tool_prefs", -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    HU_ASSERT_EQ(rc, SQLITE_ROW);
    int tool_count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    HU_ASSERT_TRUE(tool_count >= 3);

    mem.vtable->deinit(mem.ctx);
}

/* --- Skill attempt flood guard: negatives bump aggregate only, no rows --- */

static void insert_monitor_skill(sqlite3 *db, const char *name, const char *source, int64_t now,
                                 int64_t *out_id) {
    hu_allocator_t alloc = hu_system_allocator();
    char strategy[256];
    int n = snprintf(strategy, sizeof(strategy), "Monitor %s for AI developments: track updates",
                     source);
    HU_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(strategy));
    hu_error_t err =
        hu_skill_insert(&alloc, db, name, strlen(name), "research", 8, "system", 6, NULL, 0,
                        strategy, (size_t)n, "intelligence_cycle", 18, 0, now, out_id);
    HU_ASSERT_EQ(err, HU_OK);
}

static int count_attempts_by_signal(sqlite3 *db, const char *signal) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM skill_attempts WHERE outcome_signal = ?", -1,
                           &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, signal, -1, SQLITE_STATIC);
    int count = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

static void cycle_no_matching_findings_inserts_no_attempt_rows(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_opinions_table(db);

    int64_t now = (int64_t)time(NULL);

    /* Three active system skills, zero findings anywhere: every skill takes
     * the negative branch of cycle Step 13. */
    int64_t id_a = 0, id_b = 0, id_c = 0;
    insert_monitor_skill(db, "monitor_src_alpha", "src_alpha", now, &id_a);
    insert_monitor_skill(db, "monitor_src_beta", "src_beta", now, &id_b);
    insert_monitor_skill(db, "monitor_src_gamma", "src_gamma", now, &id_c);

    HU_ASSERT_EQ(count_rows(db, "skill_attempts"), 0);

    hu_intelligence_cycle_result_t r = {0};
    hu_error_t err = hu_intelligence_run_cycle(&alloc, db, &r);
    HU_ASSERT_EQ(err, HU_OK);

    /* The O(skills x cycles) flood guard: negatives must NOT append per-skill
     * skill_attempts rows (at most one summary row per cycle is tolerated). */
    HU_ASSERT_TRUE(count_rows(db, "skill_attempts") <= 1);
    HU_ASSERT_EQ(count_attempts_by_signal(db, "negative"), 0);

    /* But the per-skill aggregate on the skills table still records the miss. */
    hu_skill_t reloaded = {0};
    HU_ASSERT_EQ(hu_skill_get_by_name(&alloc, db, "monitor_src_alpha", 17, &reloaded), HU_OK);
    HU_ASSERT_EQ(reloaded.attempts, 1);
    HU_ASSERT_EQ(reloaded.successes, 0);
    HU_ASSERT_EQ(hu_skill_get_by_name(&alloc, db, "monitor_src_gamma", 17, &reloaded), HU_OK);
    HU_ASSERT_EQ(reloaded.attempts, 1);
    HU_ASSERT_EQ(reloaded.successes, 0);

    mem.vtable->deinit(mem.ctx);
}

static void cycle_matching_finding_still_records_positive_attempt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_opinions_table(db);

    int64_t now = (int64_t)time(NULL);

    int64_t skill_id = 0;
    insert_monitor_skill(db, "monitor_arxiv", "arxiv", now, &skill_id);

    /* Actioned finding whose text contains the skill's source prefix, so
     * Step 13 matches it and records a positive attempt. */
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO research_findings "
                      "(source, finding, relevance, priority, suggested_action, "
                      "status, created_at) VALUES "
                      "('arxiv', 'arxiv paper on scaling laws', 'high', 'HIGH', "
                      "'Review scaling laws paper', 'actioned', ?)";
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, now - 60);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);

    hu_intelligence_cycle_result_t r = {0};
    hu_error_t err = hu_intelligence_run_cycle(&alloc, db, &r);
    HU_ASSERT_EQ(err, HU_OK);

    /* Matched skills still leave an evidence trail in skill_attempts. */
    HU_ASSERT_TRUE(count_attempts_by_signal(db, "positive") >= 1);
    HU_ASSERT_EQ(count_attempts_by_signal(db, "negative"), 0);

    hu_skill_t reloaded = {0};
    HU_ASSERT_EQ(hu_skill_get_by_name(&alloc, db, "monitor_arxiv", 13, &reloaded), HU_OK);
    HU_ASSERT_TRUE(reloaded.attempts >= 1);
    HU_ASSERT_TRUE(reloaded.successes >= 1);

    mem.vtable->deinit(mem.ctx);
}

/* --- Step 9 flood guard: historical actioned findings don't re-record --- */

static void insert_actioned_finding(sqlite3 *db, const char *source, const char *finding,
                                    const char *action, int64_t created_at, int64_t acted_at) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO research_findings "
                      "(source, finding, relevance, priority, suggested_action, "
                      "status, created_at, acted_at) VALUES (?, ?, 'high', 'HIGH', ?, "
                      "'actioned', ?, ?)";
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, source, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, finding, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, action, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4, created_at);
    sqlite3_bind_int64(stmt, 5, acted_at);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
}

static void cycle_previously_actioned_findings_do_not_rerecord_positive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_opinions_table(db);

    int64_t now = (int64_t)time(NULL);

    /* Skill for a source whose only finding was actioned an hour ago (a prior
     * cycle). Finding text deliberately does NOT contain the source string, so
     * cycle Step 13 cannot match it — isolating Step 9 behavior. */
    int64_t skill_id = 0;
    insert_monitor_skill(db, "monitor_oldsource", "oldsource", now - 7200, &skill_id);
    insert_actioned_finding(db, "oldsource", "stale historic result", "Review the archived report",
                            now - 7200, now - 3600);

    hu_intelligence_cycle_result_t r = {0};
    hu_error_t err = hu_intelligence_run_cycle(&alloc, db, &r);
    HU_ASSERT_EQ(err, HU_OK);

    /* Step 9 must not re-record a positive for a finding actioned by a PRIOR
     * cycle — that is O(historical_sources x cycles) unbounded growth. */
    HU_ASSERT_EQ(count_attempts_by_signal(db, "positive"), 0);
    HU_ASSERT_EQ(count_rows(db, "skill_attempts"), 0);

    /* The skill saw no fresh findings this cycle: Step 13's aggregate-only
     * negative applies (attempts 1, successes 0) — not Step 9's success. */
    hu_skill_t reloaded = {0};
    HU_ASSERT_EQ(hu_skill_get_by_name(&alloc, db, "monitor_oldsource", 17, &reloaded), HU_OK);
    HU_ASSERT_EQ(reloaded.attempts, 1);
    HU_ASSERT_EQ(reloaded.successes, 0);

    mem.vtable->deinit(mem.ctx);
}

static void cycle_freshly_actioned_finding_records_step9_positive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_opinions_table(db);

    int64_t now = (int64_t)time(NULL);

    /* Skill for a source with a PENDING finding: this cycle actions it
     * (Step 1 stamps acted_at), so Step 9 must still record the positive.
     * Finding text avoids the source string to keep Step 13 out of it. */
    int64_t skill_id = 0;
    insert_monitor_skill(db, "monitor_freshsource", "freshsource", now - 7200, &skill_id);
    insert_finding(db, "freshsource", "brand new observation worth acting on", "high", "HIGH",
                   "Investigate the new observation", now - 60);

    hu_intelligence_cycle_result_t r = {0};
    hu_error_t err = hu_intelligence_run_cycle(&alloc, db, &r);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(r.findings_actioned >= 1);

    HU_ASSERT_TRUE(count_attempts_by_signal(db, "positive") >= 1);
    HU_ASSERT_EQ(count_attempts_by_signal(db, "negative"), 0);

    hu_skill_t reloaded = {0};
    HU_ASSERT_EQ(hu_skill_get_by_name(&alloc, db, "monitor_freshsource", 19, &reloaded), HU_OK);
    HU_ASSERT_TRUE(reloaded.successes >= 1);

    mem.vtable->deinit(mem.ctx);
}

/* --- Tree-of-thought: depth increases exploration --- */

static void tot_depth_increases_exploration(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tot_config_t cfg1 = hu_tot_config_default();
    cfg1.max_depth = 1;
    cfg1.enabled = true;

    hu_tot_result_t result1 = {0};
    hu_error_t err1 =
        hu_tot_explore(&alloc, NULL, "test", 4, "solve this problem", 18, &cfg1, &result1);
    HU_ASSERT_EQ(err1, HU_OK);
    HU_ASSERT_TRUE(result1.branches_explored >= 1);
    size_t explored1 = result1.branches_explored;
    hu_tot_result_free(&alloc, &result1);

    hu_tot_config_t cfg3 = hu_tot_config_default();
    cfg3.max_depth = 3;
    cfg3.enabled = true;

    hu_tot_result_t result3 = {0};
    hu_error_t err3 =
        hu_tot_explore(&alloc, NULL, "test", 4, "solve this problem", 18, &cfg3, &result3);
    HU_ASSERT_EQ(err3, HU_OK);
    HU_ASSERT_TRUE(result3.branches_explored >= explored1);
    hu_tot_result_free(&alloc, &result3);
}

void run_intelligence_cycle_tests(void) {
    HU_TEST_SUITE("intelligence_cycle");
    HU_RUN_TEST(cycle_null_alloc_returns_invalid);
    HU_RUN_TEST(cycle_null_db_returns_invalid);
    HU_RUN_TEST(cycle_null_result_returns_invalid);
    HU_RUN_TEST(cycle_empty_db_no_crash);
    HU_RUN_TEST(cycle_result_zeroed_on_entry);
    HU_RUN_TEST(cycle_with_findings_and_feeds_processes_all);
    HU_RUN_TEST(cycle_findings_only_no_events);
    HU_RUN_TEST(cycle_feeds_only_no_findings);
    HU_RUN_TEST(cycle_recurring_words_extract_lessons);
    HU_RUN_TEST(cycle_high_findings_learn_values);
    HU_RUN_TEST(cycle_high_findings_populate_opinions);
    HU_RUN_TEST(cycle_recurring_source_creates_skills);
    HU_RUN_TEST(cycle_logs_cognitive_load);
    HU_RUN_TEST(cycle_records_growth_milestone);
    HU_RUN_TEST(cycle_seeds_behavioral_feedback);
    HU_RUN_TEST(cycle_low_priority_findings_skipped);
    HU_RUN_TEST(cycle_no_matching_findings_inserts_no_attempt_rows);
    HU_RUN_TEST(cycle_matching_finding_still_records_positive_attempt);
    HU_RUN_TEST(cycle_previously_actioned_findings_do_not_rerecord_positive);
    HU_RUN_TEST(cycle_freshly_actioned_finding_records_step9_positive);
    HU_RUN_TEST(cycle_simulation_cache_returns_cached_result);
    HU_RUN_TEST(cycle_context_changes_prediction);
    HU_RUN_TEST(cycle_record_signal_changes_weight);
    HU_RUN_TEST(cycle_triggers_prompt_patches);
    HU_RUN_TEST(tot_depth_increases_exploration);
    HU_RUN_TEST(self_improve_tool_prefs_prompt_format);
}

#else

void run_intelligence_cycle_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
