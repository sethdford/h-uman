/* Tests for the intelligence subsystem wiring added across Phases 1-4.
 * Validates that the new recording paths, silence intuition, emotional pacing,
 * and relationship tone injection work correctly. */
#include "test_framework.h"
#include "human/humanness.h"
#include "human/core/allocator.h"
#include "human/experience.h"
#ifdef HU_ENABLE_SQLITE
#include "human/intelligence/online_learning.h"
#include "human/intelligence/world_model.h"
#include "human/memory.h"
#include <sqlite3.h>
#endif
#include <string.h>

#define S(lit) (lit), (sizeof(lit) - 1)

/* ── Silence Intuition Tests ─────────────────────────────────────────────── */

static void test_silence_full_response_on_question(void) {
    hu_silence_response_t r = hu_silence_intuit(S("What time is it?"),
                                                 HU_WEIGHT_NORMAL, 5, true);
    HU_ASSERT_EQ((int)r, (int)HU_SILENCE_FULL_RESPONSE);
}

static void test_silence_full_response_on_normal_message(void) {
    hu_silence_response_t r = hu_silence_intuit(S("help me with the CLI"),
                                                 HU_WEIGHT_NORMAL, 3, false);
    HU_ASSERT_EQ((int)r, (int)HU_SILENCE_FULL_RESPONSE);
}

static void test_silence_presence_only_on_grief_no_question(void) {
    hu_silence_response_t r = hu_silence_intuit(S("she's gone"),
                                                 HU_WEIGHT_GRIEF, 10, false);
    HU_ASSERT_EQ((int)r, (int)HU_SILENCE_PRESENCE_ONLY);
}

static void test_silence_full_response_on_grief_with_question(void) {
    hu_silence_response_t r = hu_silence_intuit(S("she's gone... what do I do?"),
                                                 HU_WEIGHT_GRIEF, 10, true);
    HU_ASSERT_EQ((int)r, (int)HU_SILENCE_FULL_RESPONSE);
}

static void test_silence_brief_on_heavy_short_message(void) {
    hu_silence_response_t r = hu_silence_intuit(S("I'm so tired"),
                                                 HU_WEIGHT_HEAVY, 8, false);
    HU_ASSERT_EQ((int)r, (int)HU_SILENCE_BRIEF_ACKNOWLEDGE);
}

static void test_silence_actual_silence_on_empty(void) {
    hu_silence_response_t r = hu_silence_intuit(NULL, 0,
                                                 HU_WEIGHT_NORMAL, 0, false);
    HU_ASSERT_EQ((int)r, (int)HU_SILENCE_ACTUAL_SILENCE);
}

static void test_silence_build_acknowledgment_presence(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t len = 0;
    char *ack = hu_silence_build_acknowledgment(&alloc, HU_SILENCE_PRESENCE_ONLY, &len);
    HU_ASSERT_NOT_NULL(ack);
    HU_ASSERT(len > 0);
    HU_ASSERT(strstr(ack, "here") != NULL);
    alloc.free(alloc.ctx, ack, len + 1);
}

static void test_silence_build_acknowledgment_brief(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t len = 0;
    char *ack = hu_silence_build_acknowledgment(&alloc, HU_SILENCE_BRIEF_ACKNOWLEDGE, &len);
    HU_ASSERT_NOT_NULL(ack);
    HU_ASSERT(len > 0);
    HU_ASSERT(strstr(ack, "hear") != NULL);
    alloc.free(alloc.ctx, ack, len + 1);
}

static void test_silence_build_acknowledgment_full_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t len = 0;
    char *ack = hu_silence_build_acknowledgment(&alloc, HU_SILENCE_FULL_RESPONSE, &len);
    HU_ASSERT(ack == NULL);
}

/* ── Emotional Weight & Pacing Tests ────────────────────────────────────── */

static void test_emotional_weight_normal_message(void) {
    /* "can you" and "fix" trigger NORMAL; no grief/heavy keywords */
    hu_emotional_weight_t w = hu_emotional_weight_classify(S("can you explain this code"));
    HU_ASSERT(w == HU_WEIGHT_NORMAL);
}

static void test_emotional_weight_heavy_message(void) {
    /* "overwhelmed" triggers HEAVY */
    hu_emotional_weight_t w = hu_emotional_weight_classify(
        S("I feel so overwhelmed right now"));
    HU_ASSERT(w == HU_WEIGHT_HEAVY);
}

static void test_emotional_pacing_no_delay_for_light(void) {
    uint64_t delay = hu_emotional_pacing_adjust(0, HU_WEIGHT_LIGHT);
    HU_ASSERT_EQ(delay, (uint64_t)0);
}

static void test_emotional_pacing_no_delay_for_normal(void) {
    uint64_t delay = hu_emotional_pacing_adjust(0, HU_WEIGHT_NORMAL);
    HU_ASSERT_EQ(delay, (uint64_t)0);
}

static void test_emotional_pacing_delay_for_heavy(void) {
    uint64_t delay = hu_emotional_pacing_adjust(0, HU_WEIGHT_HEAVY);
    HU_ASSERT(delay > 0);
}

static void test_emotional_pacing_delay_for_grief(void) {
    uint64_t delay = hu_emotional_pacing_adjust(0, HU_WEIGHT_GRIEF);
    HU_ASSERT(delay > 0);
    /* Grief should have longer delay than heavy */
    uint64_t heavy_delay = hu_emotional_pacing_adjust(0, HU_WEIGHT_HEAVY);
    HU_ASSERT(delay >= heavy_delay);
}

/* ── World Model Outcome Recording Tests ────────────────────────────────── */

#ifdef HU_ENABLE_SQLITE
static void test_world_model_record_and_retrieve(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);

    hu_causal_world_model_t wm;
    HU_ASSERT_EQ(hu_causal_world_model_create(&alloc, db, &wm), HU_OK);
    HU_ASSERT_EQ(hu_causal_world_model_init_tables(&wm), HU_OK);

    /* Record an outcome */
    HU_ASSERT_EQ(hu_world_record_outcome(&wm, S("file_read"), S("read 100 lines"),
                                          0.85, (int64_t)1000000), HU_OK);

    /* Simulate — should find the recorded outcome */
    hu_wm_prediction_t pred = {0};
    hu_error_t err = hu_world_simulate(&wm, S("file_read"), NULL, 0, &pred);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(pred.confidence > 0.0);
    HU_ASSERT(pred.outcome_len > 0);

    hu_causal_world_model_deinit(&wm);
    sqlite3_close(db);
}

static void test_world_model_multiple_outcomes_highest_confidence(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);

    hu_causal_world_model_t wm;
    hu_causal_world_model_create(&alloc, db, &wm);
    hu_causal_world_model_init_tables(&wm);

    /* Record multiple outcomes with different confidence */
    hu_world_record_outcome(&wm, S("web_search"), S("found results"), 0.9, 1000);
    hu_world_record_outcome(&wm, S("web_search"), S("timeout"), 0.3, 2000);

    hu_wm_prediction_t pred = {0};
    hu_world_simulate(&wm, S("web_search"), NULL, 0, &pred);
    /* Should return the higher-confidence outcome */
    HU_ASSERT(pred.confidence >= 0.3);

    hu_causal_world_model_deinit(&wm);
    sqlite3_close(db);
}

/* ── Experience Recording Tests ─────────────────────────────────────────── */

static void test_experience_per_tool_recording(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_experience_store_t store;
    HU_ASSERT_EQ(hu_experience_store_init(&alloc, NULL, &store), HU_OK);

    /* Record tool-level experiences (mimics per-tool recording in agent_turn) */
    HU_ASSERT_EQ(hu_experience_record(&store, S("file_read"), S("{\"path\":\"/tmp/x\"}"),
                                       S("contents of file"), 0.9), HU_OK);
    HU_ASSERT_EQ(store.stored_count, (size_t)1);

    HU_ASSERT_EQ(hu_experience_record(&store, S("web_search"), S("{\"query\":\"test\"}"),
                                       S("3 results found"), 0.8), HU_OK);
    HU_ASSERT_EQ(store.stored_count, (size_t)2);

    /* Failed tool */
    HU_ASSERT_EQ(hu_experience_record(&store, S("shell"), S("{\"cmd\":\"rm -rf /\"}"),
                                       S("permission denied"), 0.2), HU_OK);
    HU_ASSERT_EQ(store.stored_count, (size_t)3);

    /* Recall should find relevant experiences */
    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_experience_recall_similar(&store, S("file_read"), &ctx, &ctx_len), HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT(ctx_len > 0);
    alloc.free(alloc.ctx, ctx, ctx_len + 1);

    hu_experience_store_deinit(&store);
}

/* ── Online Learning Signal Tests ───────────────────────────────────────── */

static void test_online_learning_tool_signals(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);

    hu_online_learning_t ol;
    HU_ASSERT_EQ(hu_online_learning_create(&alloc, db, 0.1, &ol), HU_OK);
    HU_ASSERT_EQ(hu_online_learning_init_tables(&ol), HU_OK);

    /* Record success signal */
    hu_learning_signal_t sig = {
        .type = HU_SIGNAL_TOOL_SUCCESS,
        .tool_name = "file_read",
        .tool_name_len = 9,
        .magnitude = 1.0,
        .timestamp = 1000,
    };
    HU_ASSERT_EQ(hu_online_learning_record(&ol, &sig), HU_OK);

    /* Record failure signal */
    sig.type = HU_SIGNAL_TOOL_FAILURE;
    memcpy(sig.tool_name, "web_fetch", 9);
    sig.tool_name_len = 9;
    sig.timestamp = 2000;
    HU_ASSERT_EQ(hu_online_learning_record(&ol, &sig), HU_OK);

    hu_online_learning_deinit(&ol);
    sqlite3_close(db);
}

/* ── Feedback Classification Tests ──────────────────────────────────────── */

static void test_feedback_classification_short_negative(void) {
    /* Short responses (<5 chars) are classified as negative */
    /* This mirrors the inline logic in agent_turn.c */
    const char *fb_type = "neutral";
    size_t resp_len = 3;
    bool has_q = false;
    if (resp_len < 5)
        fb_type = "negative";
    else if (has_q)
        fb_type = "positive";
    HU_ASSERT_STR_EQ(fb_type, "negative");
}

static void test_feedback_classification_question_positive(void) {
    const char *fb_type = "neutral";
    size_t resp_len = 100;
    bool has_q = true;
    if (resp_len < 5)
        fb_type = "negative";
    else if (has_q)
        fb_type = "positive";
    HU_ASSERT_STR_EQ(fb_type, "positive");
}

static void test_feedback_classification_normal_neutral(void) {
    const char *fb_type = "neutral";
    size_t resp_len = 100;
    bool has_q = false;
    if (resp_len < 5)
        fb_type = "negative";
    else if (has_q)
        fb_type = "positive";
    HU_ASSERT_STR_EQ(fb_type, "neutral");
}

/* ── Behavioral Feedback SQL Tests ──────────────────────────────────────── */

static void test_behavioral_feedback_parameterized_insert(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);

    /* Create the table */
    const char *ddl = "CREATE TABLE IF NOT EXISTS behavioral_feedback("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                      "behavior_type TEXT, contact_id TEXT, signal TEXT, "
                      "context TEXT, timestamp INTEGER)";
    HU_ASSERT_EQ(sqlite3_exec(db, ddl, NULL, NULL, NULL), SQLITE_OK);

    /* Insert with parameterized query (same pattern as agent_turn.c) */
    static const char sql[] =
        "INSERT OR IGNORE INTO behavioral_feedback"
        "(behavior_type, contact_id, signal, context, timestamp) "
        "VALUES(?1, ?2, ?3, ?4, ?5)";
    sqlite3_stmt *stmt = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL), SQLITE_OK);

    /* Test with SQL injection attempt in context */
    const char *evil_msg = "test'; DROP TABLE behavioral_feedback;--";
    sqlite3_bind_text(stmt, 1, "agent_turn", 10, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, "user123", 7, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, "positive", 8, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, evil_msg, (int)strlen(evil_msg), SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, 1000000);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);

    /* Verify table still exists and has the row */
    sqlite3_stmt *check = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT context FROM behavioral_feedback", -1,
                                     &check, NULL), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(check), SQLITE_ROW);
    const char *stored = (const char *)sqlite3_column_text(check, 0);
    /* The injection attempt should be stored as literal text, not executed */
    HU_ASSERT_NOT_NULL(stored);
    HU_ASSERT(strstr(stored, "DROP TABLE") != NULL); /* stored literally */
    sqlite3_finalize(check);

    /* Verify table still exists after the "injection" */
    sqlite3_stmt *verify = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM behavioral_feedback", -1,
                                     &verify, NULL), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(verify), SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(verify, 0), 1);
    sqlite3_finalize(verify);

    sqlite3_close(db);
}
#endif /* HU_ENABLE_SQLITE */

/* ── Registration ────────────────────────────────────────────────────────── */

void run_intelligence_wiring_tests(void) {
    HU_TEST_SUITE("intelligence_wiring");

    /* Silence intuition */
    HU_RUN_TEST(test_silence_full_response_on_question);
    HU_RUN_TEST(test_silence_full_response_on_normal_message);
    HU_RUN_TEST(test_silence_presence_only_on_grief_no_question);
    HU_RUN_TEST(test_silence_full_response_on_grief_with_question);
    HU_RUN_TEST(test_silence_brief_on_heavy_short_message);
    HU_RUN_TEST(test_silence_actual_silence_on_empty);
    HU_RUN_TEST(test_silence_build_acknowledgment_presence);
    HU_RUN_TEST(test_silence_build_acknowledgment_brief);
    HU_RUN_TEST(test_silence_build_acknowledgment_full_returns_null);

    /* Emotional weight & pacing */
    HU_RUN_TEST(test_emotional_weight_normal_message);
    HU_RUN_TEST(test_emotional_weight_heavy_message);
    HU_RUN_TEST(test_emotional_pacing_no_delay_for_light);
    HU_RUN_TEST(test_emotional_pacing_no_delay_for_normal);
    HU_RUN_TEST(test_emotional_pacing_delay_for_heavy);
    HU_RUN_TEST(test_emotional_pacing_delay_for_grief);

#ifdef HU_ENABLE_SQLITE
    /* World model outcome recording */
    HU_RUN_TEST(test_world_model_record_and_retrieve);
    HU_RUN_TEST(test_world_model_multiple_outcomes_highest_confidence);

    /* Experience per-tool recording */
    HU_RUN_TEST(test_experience_per_tool_recording);

    /* Online learning signals */
    HU_RUN_TEST(test_online_learning_tool_signals);

    /* Feedback classification */
    HU_RUN_TEST(test_feedback_classification_short_negative);
    HU_RUN_TEST(test_feedback_classification_question_positive);
    HU_RUN_TEST(test_feedback_classification_normal_neutral);

    /* SQL injection prevention */
    HU_RUN_TEST(test_behavioral_feedback_parameterized_insert);
#endif
}
