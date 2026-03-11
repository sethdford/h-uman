#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/memory/deep_memory.h"
#include "test_framework.h"
#include <math.h>
#include <string.h>

/* --- F70 Episodic Memory --- */
static void episodic_create_table_valid(void) {
    char buf[1024];
    size_t len = 0;
    hu_error_t err = hu_episodic_create_table_sql(buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "CREATE TABLE") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "episodes") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "emotional_arc") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "source_tag") != NULL);
}

static void episodic_insert_sql_valid(void) {
    hu_episode_t ep = {
        .summary = "Had coffee with user_a",
        .summary_len = 19,
        .emotional_arc = "started anxious, ended relieved",
        .emotional_arc_len = 29,
        .impact_score = 0.7,
        .participants = "[\"user_a\",\"user_b\"]",
        .participants_len = 19,
        .occurred_at = 1700000000u,
        .source_tag = "conversation",
        .source_tag_len = 12,
    };
    char buf[1024];
    size_t len = 0;
    hu_error_t err = hu_episodic_insert_sql(&ep, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "INSERT INTO") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "Had coffee") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "conversation") != NULL);
}

static void episodic_query_by_contact_sql_valid(void) {
    char buf[512];
    size_t len = 0;
    hu_error_t err =
        hu_episodic_query_by_contact_sql("user_a", 6, 10, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "participants LIKE") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "user_a") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "LIMIT 10") != NULL);
}

static void episodic_query_high_impact_sql_valid(void) {
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_episodic_query_high_impact_sql(0.8, 5, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "impact_score") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "LIMIT 5") != NULL);
}

/* --- F71 Associative Recall --- */
static void episodic_relevance_identical_returns_one(void) {
    const char *summary = "coffee meeting project deadline";
    const char *trigger = "coffee meeting project deadline";
    double score = hu_episodic_relevance_score(summary, 31, trigger, 31);
    HU_ASSERT_FLOAT_EQ(score, 1.0, 0.001);
}

static void episodic_relevance_no_overlap_returns_zero(void) {
    const char *summary = "alpha beta gamma";
    const char *trigger = "foo bar baz";
    double score = hu_episodic_relevance_score(summary, 15, trigger, 11);
    HU_ASSERT_FLOAT_EQ(score, 0.0, 0.001);
}

static void episodic_relevance_partial_overlap_returns_between(void) {
    const char *summary = "coffee meeting with team";
    const char *trigger = "coffee project";
    double score = hu_episodic_relevance_score(summary, 22, trigger, 13);
    HU_ASSERT_TRUE(score > 0.0);
    HU_ASSERT_TRUE(score < 1.0);
}

/* --- F72 Consolidation Engine --- */
static void consolidation_should_merge_identical_returns_true(void) {
    const char *a = "hello world";
    const char *b = "hello world";
    bool r = hu_consolidation_should_merge(a, 11, b, 11, 0.8);
    HU_ASSERT_TRUE(r);
}

static void consolidation_should_merge_different_returns_false(void) {
    const char *a = "alpha beta";
    const char *b = "foo bar";
    bool r = hu_consolidation_should_merge(a, 10, b, 7, 0.8);
    HU_ASSERT_FALSE(r);
}

static void consolidation_merge_sql_valid(void) {
    char buf[256];
    size_t len = 0;
    hu_error_t err = hu_consolidation_merge_sql(1, 2, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "DELETE") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "id=2") != NULL);
}

/* --- F74 Source Tagging --- */
static void episodic_source_tag_in_insert(void) {
    hu_episode_t ep = {
        .summary = "event",
        .summary_len = 5,
        .source_tag = "event",
        .source_tag_len = 5,
        .impact_score = 0.5,
        .occurred_at = 1000u,
    };
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_episodic_insert_sql(&ep, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "event") != NULL);
}

/* --- F75 Prospective Memory --- */
static void prospective_create_table_valid(void) {
    char buf[1024];
    size_t len = 0;
    hu_error_t err = hu_prospective_create_table_sql(buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "prospective_memory") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "trigger_type") != NULL);
}

static void prospective_insert_sql_valid(void) {
    hu_prospective_item_t item = {
        .description = "Call mom",
        .description_len = 8,
        .trigger_type = "time",
        .trigger_type_len = 4,
        .trigger_value = "2026-03-15",
        .trigger_value_len = 10,
        .created_at = 1700000000u,
        .completed = false,
    };
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_prospective_insert_sql(&item, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "Call mom") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "time") != NULL);
}

static void prospective_query_pending_valid(void) {
    char buf[256];
    size_t len = 0;
    hu_error_t err = hu_prospective_query_pending_sql(buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "completed=0") != NULL);
}

static void prospective_complete_sql_valid(void) {
    char buf[256];
    size_t len = 0;
    hu_error_t err = hu_prospective_complete_sql(42, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "completed=1") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "id=42") != NULL);
}

static void prospective_build_prompt_with_data(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prospective_item_t items[2] = {{0}};
    items[0].description = hu_strndup(&alloc, "Call mom", 8);
    items[0].description_len = 8;
    items[0].trigger_type = hu_strndup(&alloc, "time", 4);
    items[0].trigger_type_len = 4;
    items[0].trigger_value = hu_strndup(&alloc, "2026-03-15", 10);
    items[0].trigger_value_len = 10;
    items[1].description = hu_strndup(&alloc, "Send report", 11);
    items[1].description_len = 11;
    items[1].trigger_type = hu_strndup(&alloc, "topic", 5);
    items[1].trigger_type_len = 5;
    items[1].trigger_value = hu_strndup(&alloc, "they mention report", 18);
    items[1].trigger_value_len = 18;

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_prospective_build_prompt(&alloc, items, 2, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "PENDING REMINDERS") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Call mom") != NULL);

    hu_str_free(&alloc, out);
    hu_prospective_item_deinit(&alloc, &items[0]);
    hu_prospective_item_deinit(&alloc, &items[1]);
}

/* --- F76 Emotional Residue --- */
static void residue_create_table_valid(void) {
    char buf[1024];
    size_t len = 0;
    hu_error_t err = hu_residue_create_table_sql(buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "emotional_residue") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "decay_rate") != NULL);
}

static void residue_insert_sql_valid(void) {
    hu_emotional_residue_t r = {
        .contact_id = "user_a",
        .contact_id_len = 6,
        .emotion = "gratitude",
        .emotion_len = 9,
        .intensity = 0.8,
        .from_timestamp = 1700000000u,
        .decay_rate = 0.1,
    };
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_residue_insert_sql(&r, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "user_a") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "gratitude") != NULL);
}

static void residue_query_active_sql_valid(void) {
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_residue_query_active_sql("user_a", 6, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "contact_id") != NULL);
}

static void residue_current_intensity_decay(void) {
    double i0 = hu_residue_current_intensity(1.0, 0.1, 0.0);
    HU_ASSERT_FLOAT_EQ(i0, 1.0, 0.001);

    double i1 = hu_residue_current_intensity(1.0, 0.1, 10.0);
    HU_ASSERT_TRUE(i1 < 1.0);
    HU_ASSERT_TRUE(i1 > 0.0);

    double i2 = hu_residue_current_intensity(1.0, 0.5, 10.0);
    HU_ASSERT_TRUE(i2 < i1);
}

static void residue_build_prompt_with_data(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_emotional_residue_t residues[2] = {{0}};
    residues[0].contact_id = hu_strndup(&alloc, "user_a", 6);
    residues[0].contact_id_len = 6;
    residues[0].emotion = hu_strndup(&alloc, "gratitude", 9);
    residues[0].emotion_len = 9;
    residues[0].intensity = 0.8;
    residues[0].decay_rate = 0.1;
    residues[1].emotion = hu_strndup(&alloc, "warmth", 6);
    residues[1].emotion_len = 6;
    residues[1].intensity = 0.6;
    residues[1].decay_rate = 0.2;

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_residue_build_prompt(&alloc, residues, 2, 5.0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "EMOTIONAL RESIDUE") != NULL);
    HU_ASSERT_TRUE(strstr(out, "gratitude") != NULL);

    hu_str_free(&alloc, out);
    hu_emotional_residue_deinit(&alloc, &residues[0]);
    hu_emotional_residue_deinit(&alloc, &residues[1]);
}

static void episode_deinit_clears_fields(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_episode_t ep = {0};
    ep.summary = hu_strndup(&alloc, "test", 4);
    ep.summary_len = 4;
    ep.emotional_arc = hu_strndup(&alloc, "arc", 3);
    ep.emotional_arc_len = 3;
    hu_episode_deinit(&alloc, &ep);
    HU_ASSERT_NULL(ep.summary);
    HU_ASSERT_NULL(ep.emotional_arc);
    HU_ASSERT_EQ(ep.summary_len, 0u);
}

void run_deep_memory_tests(void) {
    HU_TEST_SUITE("deep_memory");
    HU_RUN_TEST(episodic_create_table_valid);
    HU_RUN_TEST(episodic_insert_sql_valid);
    HU_RUN_TEST(episodic_query_by_contact_sql_valid);
    HU_RUN_TEST(episodic_query_high_impact_sql_valid);
    HU_RUN_TEST(episodic_relevance_identical_returns_one);
    HU_RUN_TEST(episodic_relevance_no_overlap_returns_zero);
    HU_RUN_TEST(episodic_relevance_partial_overlap_returns_between);
    HU_RUN_TEST(consolidation_should_merge_identical_returns_true);
    HU_RUN_TEST(consolidation_should_merge_different_returns_false);
    HU_RUN_TEST(consolidation_merge_sql_valid);
    HU_RUN_TEST(episodic_source_tag_in_insert);
    HU_RUN_TEST(prospective_create_table_valid);
    HU_RUN_TEST(prospective_insert_sql_valid);
    HU_RUN_TEST(prospective_query_pending_valid);
    HU_RUN_TEST(prospective_complete_sql_valid);
    HU_RUN_TEST(prospective_build_prompt_with_data);
    HU_RUN_TEST(residue_create_table_valid);
    HU_RUN_TEST(residue_insert_sql_valid);
    HU_RUN_TEST(residue_query_active_sql_valid);
    HU_RUN_TEST(residue_current_intensity_decay);
    HU_RUN_TEST(residue_build_prompt_with_data);
    HU_RUN_TEST(episode_deinit_clears_fields);
}
