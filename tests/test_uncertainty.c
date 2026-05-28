#include "human/agent/uncertainty.h"
#include "human/core/allocator.h"
#include "human/persona.h"
#include "test_framework.h"
#include <string.h>

/* AC-4: lock pre-change behavior on the no-real-signals path. This test
 * MUST pass against the unmodified hu_uncertainty_evaluate. After Tasks
 * 2-5 modify the score function, this test still passes — that's the
 * regression contract. */
static void test_score_unchanged_with_no_real_signals(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_uncertainty_signals_t signals = {0};
    signals.retrieval_coverage = 0.5; /* contributes 0.15 */
    signals.tool_results_count = 1;   /* contributes 0.2 */
    signals.has_citations = false;
    signals.has_hedging_language = false; /* confident language → 0.15 */
    signals.memory_results_count = 2;     /* 2 * 0.033 = 0.066 */
    signals.is_factual_query = true;      /* no opinion bonus */
    /* NEW fields explicitly zero — exercises the no-real-signals path */
    signals.grounded_confidence = 0.0;
    signals.fact_count = 0;
    signals.verbalized_confidence = 0.0;
    signals.has_verbalized = false;
    signals.contradiction_present = false;
    signals.has_temporal_decay = false;

    hu_uncertainty_result_t result = {0};
    HU_ASSERT_EQ(hu_uncertainty_evaluate(&alloc, &signals, &result), HU_OK);

    /* Pre-change expected: 0.15 + 0.2 + 0.15 + 0.066 = 0.566 */
    HU_ASSERT_TRUE(result.confidence > 0.565 && result.confidence < 0.567);
    HU_ASSERT_EQ(result.level, HU_CONFIDENCE_MEDIUM);

    hu_uncertainty_result_free(&alloc, &result);
}

static void test_score_blend_at_one_fact(void) {
    /* fact_count=1, grounded_confidence=0.9 → 33% real + 67% heuristic
     * Set only retrieval_coverage=0.5 to get heuristic=0.15, avoiding
     * default-signal contributions (e.g. !has_hedging_language → +0.15) */
    hu_uncertainty_signals_t signals = {0};
    signals.retrieval_coverage = 0.5;
    signals.fact_count = 1;
    signals.grounded_confidence = 0.9;
    /* Explicitly set these to prevent default-false signals from boosting score */
    signals.has_citations = false;
    signals.has_hedging_language = true; /* suppress the !hedging bonus */
    signals.is_factual_query = true;     /* suppress the !factual bonus */
    hu_allocator_t alloc = hu_system_allocator();
    hu_uncertainty_result_t result = {0};
    HU_ASSERT_EQ(hu_uncertainty_evaluate(&alloc, &signals, &result), HU_OK);
    /* Expect: (1-1/3)*0.15 + (1/3)*0.9 = 0.1 + 0.3 = 0.4 */
    HU_ASSERT_TRUE(result.confidence > 0.37 && result.confidence < 0.43);
    hu_uncertainty_result_free(&alloc, &result);
}

static void test_score_blend_at_three_facts(void) {
    /* fact_count=3 → 100% real signal. heuristics contribute 0 */
    hu_uncertainty_signals_t signals = {0};
    signals.retrieval_coverage = 0.5;
    signals.fact_count = 3;
    signals.grounded_confidence = 0.85;
    hu_allocator_t alloc = hu_system_allocator();
    hu_uncertainty_result_t result = {0};
    HU_ASSERT_EQ(hu_uncertainty_evaluate(&alloc, &signals, &result), HU_OK);
    HU_ASSERT_TRUE(result.confidence > 0.84 && result.confidence < 0.86);
    hu_uncertainty_result_free(&alloc, &result);
}

static void test_grounded_confidence_uses_effective_decay(void) {
    /* 60-day-old 0.9 fact arrives here as 0.57 (decay already applied
       at agent_turn integration). Pure consumption test. */
    hu_uncertainty_signals_t signals = {0};
    signals.fact_count = 3;
    signals.grounded_confidence = 0.57;
    hu_allocator_t alloc = hu_system_allocator();
    hu_uncertainty_result_t result = {0};
    HU_ASSERT_EQ(hu_uncertainty_evaluate(&alloc, &signals, &result), HU_OK);
    HU_ASSERT_EQ(result.level, HU_CONFIDENCE_MEDIUM);
    hu_uncertainty_result_free(&alloc, &result);
}

static void test_contradiction_penalty_applies(void) {
    hu_uncertainty_signals_t signals = {0};
    signals.fact_count = 3;
    signals.grounded_confidence = 0.85;
    signals.contradiction_present = true;
    hu_allocator_t alloc = hu_system_allocator();
    hu_uncertainty_result_t result = {0};
    HU_ASSERT_EQ(hu_uncertainty_evaluate(&alloc, &signals, &result), HU_OK);
    /* fact_count=3 → evidence_weight=1.0 → blended=0.85 → 0.85-0.15=0.70 */
    HU_ASSERT_TRUE(result.confidence > 0.69 && result.confidence < 0.71);
    hu_uncertainty_result_free(&alloc, &result);
}

static void test_contradiction_penalty_with_low_evidence_weight(void) {
    /* fact_count=1, grounded_confidence=0.9, contradiction_present=true
     * evidence_weight = 1/3 ≈ 0.333
     * heuristic_score = 0 (all defaults suppressed)
     * blended before penalty: (1-1/3)*0 + (1/3)*0.9 = 0.3
     * after penalty: 0.3 - 0.15 = 0.15
     * Expected: 0.15 ± 0.01
     */
    hu_uncertainty_signals_t signals = {0};
    signals.fact_count = 1;
    signals.grounded_confidence = 0.9;
    signals.contradiction_present = true;
    signals.has_hedging_language = true; /* suppress !hedging bonus */
    signals.is_factual_query = true;     /* suppress !factual bonus */
    hu_allocator_t alloc = hu_system_allocator();
    hu_uncertainty_result_t result = {0};
    HU_ASSERT_EQ(hu_uncertainty_evaluate(&alloc, &signals, &result), HU_OK);
    HU_ASSERT_TRUE(result.confidence > 0.14 && result.confidence < 0.16);
    hu_uncertainty_result_free(&alloc, &result);
}

static void test_verbalized_low_pulls_score_down(void) {
    /* Model self-reports 0.3 vs blended 0.7 → result = 0.6*0.7 + 0.4*0.3 = 0.54 */
    hu_uncertainty_signals_t signals = {0};
    signals.fact_count = 3;
    signals.grounded_confidence = 0.7;
    signals.has_verbalized = true;
    signals.verbalized_confidence = 0.3;
    hu_allocator_t alloc = hu_system_allocator();
    hu_uncertainty_result_t result = {0};
    HU_ASSERT_EQ(hu_uncertainty_evaluate(&alloc, &signals, &result), HU_OK);
    HU_ASSERT_TRUE(result.confidence > 0.53 && result.confidence < 0.55);
    hu_uncertainty_result_free(&alloc, &result);
}

static void test_verbalized_high_does_not_over_inflate(void) {
    /* Model claims 0.95, signals say 0.6 → stays near 0.6 (asymmetric rule) */
    hu_uncertainty_signals_t signals = {0};
    signals.fact_count = 3;
    signals.grounded_confidence = 0.6;
    signals.has_verbalized = true;
    signals.verbalized_confidence = 0.95;
    hu_allocator_t alloc = hu_system_allocator();
    hu_uncertainty_result_t result = {0};
    HU_ASSERT_EQ(hu_uncertainty_evaluate(&alloc, &signals, &result), HU_OK);
    HU_ASSERT_TRUE(result.confidence > 0.58 && result.confidence < 0.62);
    hu_uncertainty_result_free(&alloc, &result);
}

/* Task 3: Strip and parse verbalized confidence tags */
static void test_strip_verbalized_tag_at_response_tail(void) {
    char response[] = "She said Thursday. [conf=0.7]";
    size_t len = strlen(response);
    double parsed_conf = -1.0;
    bool found = hu_uncertainty_strip_verbalized(response, &len, &parsed_conf);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_TRUE(parsed_conf > 0.69 && parsed_conf < 0.71);
    response[len] = '\0';
    HU_ASSERT_STR_EQ(response, "She said Thursday.");
}

static void test_strip_verbalized_no_tag_returns_no_match(void) {
    char response[] = "Plain answer with no tag.";
    size_t len = strlen(response);
    double parsed_conf = -1.0;
    bool found = hu_uncertainty_strip_verbalized(response, &len, &parsed_conf);
    HU_ASSERT_FALSE(found);
    HU_ASSERT_EQ(len, strlen("Plain answer with no tag."));
}

static void test_strip_verbalized_malformed_no_closing_bracket(void) {
    /* [conf= without ] within 32-char lookback → returns false */
    char response[] = "Thursday is likely. [conf=0.7";
    size_t len = strlen(response);
    double parsed_conf = -1.0;
    bool found = hu_uncertainty_strip_verbalized(response, &len, &parsed_conf);
    HU_ASSERT_FALSE(found);
    HU_ASSERT_EQ(len, strlen(response)); /* response unchanged */
}

static void test_strip_verbalized_boundary_zero(void) {
    /* [conf=0.0] is valid (lower bound) */
    char response[] = "Low confidence. [conf=0.0]";
    size_t len = strlen(response);
    double parsed_conf = -1.0;
    bool found = hu_uncertainty_strip_verbalized(response, &len, &parsed_conf);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_TRUE(parsed_conf > -0.01 && parsed_conf < 0.01);
    response[len] = '\0';
    HU_ASSERT_STR_EQ(response, "Low confidence.");
}

static void test_strip_verbalized_boundary_one(void) {
    /* [conf=1.0] is valid (upper bound) */
    char response[] = "Completely certain. [conf=1.0]";
    size_t len = strlen(response);
    double parsed_conf = -1.0;
    bool found = hu_uncertainty_strip_verbalized(response, &len, &parsed_conf);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_TRUE(parsed_conf > 0.99 && parsed_conf < 1.01);
    response[len] = '\0';
    HU_ASSERT_STR_EQ(response, "Completely certain.");
}

static void test_strip_verbalized_whitespace_before_bracket(void) {
    /* Extra space before [ → still works (whitespace stripped) */
    char response[] = "Thursday.  [conf=0.7]";
    size_t len = strlen(response);
    double parsed_conf = -1.0;
    bool found = hu_uncertainty_strip_verbalized(response, &len, &parsed_conf);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_TRUE(parsed_conf > 0.69 && parsed_conf < 0.71);
    response[len] = '\0';
    HU_ASSERT_STR_EQ(response, "Thursday.");
}

static void test_strip_verbalized_out_of_range_high(void) {
    /* [conf=1.5] is out of range [0, 1] → returns false */
    char response[] = "Over-confident. [conf=1.5]";
    size_t len = strlen(response);
    double parsed_conf = -1.0;
    bool found = hu_uncertainty_strip_verbalized(response, &len, &parsed_conf);
    HU_ASSERT_FALSE(found);
    HU_ASSERT_EQ(len, strlen(response)); /* response unchanged */
}

static void test_strip_verbalized_out_of_range_negative(void) {
    /* [conf=-0.1] is out of range [0, 1] → returns false */
    char response[] = "Invalid. [conf=-0.1]";
    size_t len = strlen(response);
    double parsed_conf = -1.0;
    bool found = hu_uncertainty_strip_verbalized(response, &len, &parsed_conf);
    HU_ASSERT_FALSE(found);
    HU_ASSERT_EQ(len, strlen(response)); /* response unchanged */
}

/* Task 4: Persona overlay hedge phrase banks (tests 10-14) */
static void test_default_hedges_present_for_all_four_levels(void) {
    srand(42);
    HU_ASSERT_STR_EQ(hu_uncertainty_pick_hedge(HU_CONFIDENCE_HIGH, NULL), "");
    HU_ASSERT_NOT_NULL(hu_uncertainty_pick_hedge(HU_CONFIDENCE_MEDIUM, NULL));
    HU_ASSERT_NOT_NULL(hu_uncertainty_pick_hedge(HU_CONFIDENCE_LOW, NULL));
    HU_ASSERT_NOT_NULL(hu_uncertainty_pick_hedge(HU_CONFIDENCE_VERY_LOW, NULL));
}

static void test_persona_overlay_overrides_defaults(void) {
    hu_persona_overlay_t overlay = {0};
    static const char *custom[] = {"pretty sure tho — "};
    overlay.hedge_phrases[HU_CONFIDENCE_MEDIUM] = (char **)custom;
    overlay.hedge_phrase_counts[HU_CONFIDENCE_MEDIUM] = 1;
    srand(42);
    const char *picked = hu_uncertainty_pick_hedge(HU_CONFIDENCE_MEDIUM, &overlay);
    HU_ASSERT_STR_EQ(picked, "pretty sure tho — ");
}

static void test_persona_overlay_empty_array_falls_back_to_default(void) {
    hu_persona_overlay_t overlay = {0};
    srand(42);
    const char *picked = hu_uncertainty_pick_hedge(HU_CONFIDENCE_MEDIUM, &overlay);
    HU_ASSERT_NOT_NULL(picked);
}

static void test_hedge_selection_deterministic_with_seed(void) {
    srand(42);
    const char *first = hu_uncertainty_pick_hedge(HU_CONFIDENCE_LOW, NULL);
    srand(42);
    const char *second = hu_uncertainty_pick_hedge(HU_CONFIDENCE_LOW, NULL);
    HU_ASSERT_STR_EQ(first, second);
}

static void test_temporal_hedge_used_when_decay_material(void) {
    hu_uncertainty_signals_t signals = {0};
    signals.fact_count = 3;
    signals.grounded_confidence = 0.65;
    signals.has_temporal_decay = true;
    hu_allocator_t alloc = hu_system_allocator();
    hu_uncertainty_result_t result = {0};
    HU_ASSERT_EQ(hu_uncertainty_evaluate(&alloc, &signals, &result), HU_OK);
    HU_ASSERT_EQ(result.level, HU_CONFIDENCE_MEDIUM);
    HU_ASSERT_NOT_NULL(result.hedge_prefix);
    hu_uncertainty_result_free(&alloc, &result);
}

/* Task 6: ECE-ready logging (tests 15-17, gated on HU_ENABLE_SQLITE) */
#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

static void test_uncertainty_log_inserts_row(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    HU_ASSERT_EQ(hu_uncertainty_storage_migrate(db), HU_OK);

    hu_uncertainty_log_entry_t entry = {
        .turn_id = "turn_001",
        .channel = "imessage",
        .query_text = "did she say Thursday?",
        .response_text = "She said Thursday.",
        .stated_confidence = 0.65,
        .level = HU_CONFIDENCE_MEDIUM,
        .hedge_phrase_used = "I'm pretty sure — ",
        .signals_json = "{\"fact_count\":2,\"grounded_confidence\":0.7}",
        .created_at_ms = 1000,
    };
    HU_ASSERT_EQ(hu_uncertainty_log(db, &entry), HU_OK);

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM uncertainty_evaluations", -1, &st, NULL);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(st, 0), 1);
    sqlite3_finalize(st);
    sqlite3_close(db);
}

static void test_uncertainty_log_outcome_starts_null(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_uncertainty_storage_migrate(db);
    hu_uncertainty_log_entry_t entry = {
        .turn_id = "t1",
        .channel = "imessage",
        .stated_confidence = 0.5,
        .level = HU_CONFIDENCE_MEDIUM,
        .signals_json = "{}",
        .created_at_ms = 1000,
    };
    hu_uncertainty_log(db, &entry);

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT outcome_label FROM uncertainty_evaluations LIMIT 1", -1, &st,
                       NULL);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_type(st, 0), SQLITE_NULL);
    sqlite3_finalize(st);
    sqlite3_close(db);
}

static void test_uncertainty_log_outcome_can_be_backfilled(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_uncertainty_storage_migrate(db);
    hu_uncertainty_log_entry_t entry = {
        .turn_id = "t1",
        .channel = "imessage",
        .stated_confidence = 0.5,
        .level = HU_CONFIDENCE_MEDIUM,
        .signals_json = "{}",
        .created_at_ms = 1000,
    };
    hu_uncertainty_log(db, &entry);
    HU_ASSERT_EQ(hu_uncertainty_set_outcome(db, "t1", "correct", "user_reaction", 2000), HU_OK);

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
                       "SELECT outcome_label, outcome_source FROM uncertainty_evaluations LIMIT 1",
                       -1, &st, NULL);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(st, 0), "correct");
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(st, 1), "user_reaction");
    sqlite3_finalize(st);
    sqlite3_close(db);
}
#endif

void run_uncertainty_tests(void) {
    HU_TEST_SUITE("uncertainty");
    HU_RUN_TEST(test_score_unchanged_with_no_real_signals);
    HU_RUN_TEST(test_score_blend_at_one_fact);
    HU_RUN_TEST(test_score_blend_at_three_facts);
    HU_RUN_TEST(test_grounded_confidence_uses_effective_decay);
    HU_RUN_TEST(test_contradiction_penalty_applies);
    HU_RUN_TEST(test_contradiction_penalty_with_low_evidence_weight);
    HU_RUN_TEST(test_verbalized_low_pulls_score_down);
    HU_RUN_TEST(test_verbalized_high_does_not_over_inflate);
    HU_RUN_TEST(test_strip_verbalized_tag_at_response_tail);
    HU_RUN_TEST(test_strip_verbalized_no_tag_returns_no_match);
    HU_RUN_TEST(test_strip_verbalized_malformed_no_closing_bracket);
    HU_RUN_TEST(test_strip_verbalized_boundary_zero);
    HU_RUN_TEST(test_strip_verbalized_boundary_one);
    HU_RUN_TEST(test_strip_verbalized_whitespace_before_bracket);
    HU_RUN_TEST(test_strip_verbalized_out_of_range_high);
    HU_RUN_TEST(test_strip_verbalized_out_of_range_negative);
    HU_RUN_TEST(test_default_hedges_present_for_all_four_levels);
    HU_RUN_TEST(test_persona_overlay_overrides_defaults);
    HU_RUN_TEST(test_persona_overlay_empty_array_falls_back_to_default);
    HU_RUN_TEST(test_hedge_selection_deterministic_with_seed);
    HU_RUN_TEST(test_temporal_hedge_used_when_decay_material);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_uncertainty_log_inserts_row);
    HU_RUN_TEST(test_uncertainty_log_outcome_starts_null);
    HU_RUN_TEST(test_uncertainty_log_outcome_can_be_backfilled);
#endif
}
