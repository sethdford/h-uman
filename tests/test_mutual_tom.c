#include "human/agent/theory_of_mind.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "test_framework.h"
#include <string.h>

/* --- hu_tom_record_user_expectation --- */

static void record_expectation_stores_topic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tom_belief_state_t state;
    memset(&state, 0, sizeof(state));
    hu_error_t err = hu_tom_init(&state, &alloc, "alice", 5);
    HU_ASSERT_EQ(err, HU_OK);

    err = hu_tom_record_user_expectation(&state, &alloc, "birthday", 8, HU_TOM_EXPECT_REMEMBERS);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(state.expectation_count, 1u);
    HU_ASSERT_STR_EQ(state.expectations[0].topic, "birthday");
    HU_ASSERT_EQ((int)state.expectations[0].knowledge_type, (int)HU_TOM_EXPECT_REMEMBERS);

    hu_tom_deinit(&state, &alloc);
}

static void record_expectation_updates_existing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tom_belief_state_t state;
    memset(&state, 0, sizeof(state));
    HU_ASSERT_EQ(hu_tom_init(&state, &alloc, "bob", 3), HU_OK);

    hu_tom_record_user_expectation(&state, &alloc, "job search", 10, HU_TOM_EXPECT_TRACKS);
    hu_tom_record_user_expectation(&state, &alloc, "job search", 10, HU_TOM_EXPECT_REMEMBERS);
    HU_ASSERT_EQ(state.expectation_count, 1u);
    HU_ASSERT_EQ((int)state.expectations[0].knowledge_type, (int)HU_TOM_EXPECT_REMEMBERS);

    hu_tom_deinit(&state, &alloc);
}

static void record_expectation_null_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_tom_record_user_expectation(NULL, &alloc, "x", 1, HU_TOM_EXPECT_REMEMBERS);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

/* --- hu_tom_detect_gaps --- */

static void detect_gaps_finds_missing_knowledge(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tom_belief_state_t state;
    memset(&state, 0, sizeof(state));
    hu_tom_init(&state, &alloc, "carol", 5);

    /* User expects AI knows birthday, but AI has no belief about it */
    hu_tom_record_user_expectation(&state, &alloc, "birthday", 8, HU_TOM_EXPECT_REMEMBERS);

    hu_tom_gap_t *gaps = NULL;
    size_t gap_count = 0;
    hu_error_t err = hu_tom_detect_gaps(&state, &alloc, &gaps, &gap_count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(gap_count, 1u);
    HU_ASSERT_NOT_NULL(gaps);
    HU_ASSERT_STR_EQ(gaps[0].topic, "birthday");
    HU_ASSERT_EQ((int)gaps[0].knowledge_type, (int)HU_TOM_EXPECT_REMEMBERS);

    hu_tom_gaps_free(&alloc, gaps, gap_count);
    hu_tom_deinit(&state, &alloc);
}

static void detect_gaps_no_gap_when_ai_knows(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tom_belief_state_t state;
    memset(&state, 0, sizeof(state));
    hu_tom_init(&state, &alloc, "dave", 4);

    /* AI knows about birthday */
    hu_tom_record_belief(&state, &alloc, "birthday", 8, HU_BELIEF_KNOWS, 0.9f);
    /* User also expects AI knows birthday */
    hu_tom_record_user_expectation(&state, &alloc, "birthday", 8, HU_TOM_EXPECT_REMEMBERS);

    hu_tom_gap_t *gaps = NULL;
    size_t gap_count = 0;
    hu_error_t err = hu_tom_detect_gaps(&state, &alloc, &gaps, &gap_count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(gap_count, 0u);
    HU_ASSERT_NULL(gaps);

    hu_tom_deinit(&state, &alloc);
}

static void detect_gaps_low_confidence_still_gap(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tom_belief_state_t state;
    memset(&state, 0, sizeof(state));
    hu_tom_init(&state, &alloc, "eve", 3);

    /* AI has low-confidence knowledge */
    hu_tom_record_belief(&state, &alloc, "birthday", 8, HU_BELIEF_KNOWS, 0.3f);
    hu_tom_record_user_expectation(&state, &alloc, "birthday", 8, HU_TOM_EXPECT_REMEMBERS);

    hu_tom_gap_t *gaps = NULL;
    size_t gap_count = 0;
    hu_error_t err = hu_tom_detect_gaps(&state, &alloc, &gaps, &gap_count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(gap_count, 1u); /* low confidence = still a gap */

    hu_tom_gaps_free(&alloc, gaps, gap_count);
    hu_tom_deinit(&state, &alloc);
}

static void detect_gaps_empty_expectations_returns_zero(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tom_belief_state_t state;
    memset(&state, 0, sizeof(state));
    hu_tom_init(&state, &alloc, "frank", 5);

    hu_tom_gap_t *gaps = NULL;
    size_t gap_count = 0;
    hu_error_t err = hu_tom_detect_gaps(&state, &alloc, &gaps, &gap_count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(gap_count, 0u);

    hu_tom_deinit(&state, &alloc);
}

/* --- hu_tom_build_gap_directive --- */

static void build_gap_directive_produces_context(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tom_belief_state_t state;
    memset(&state, 0, sizeof(state));
    hu_tom_init(&state, &alloc, "grace", 5);

    hu_tom_record_user_expectation(&state, &alloc, "birthday", 8, HU_TOM_EXPECT_REMEMBERS);
    hu_tom_record_user_expectation(&state, &alloc, "job search", 10, HU_TOM_EXPECT_TRACKS);

    hu_tom_gap_t *gaps = NULL;
    size_t gap_count = 0;
    hu_tom_detect_gaps(&state, &alloc, &gaps, &gap_count);
    HU_ASSERT_EQ(gap_count, 2u);

    size_t dir_len = 0;
    char *directive = hu_tom_build_gap_directive(&alloc, gaps, gap_count, &dir_len);
    HU_ASSERT_NOT_NULL(directive);
    HU_ASSERT_TRUE(dir_len > 0);
    HU_ASSERT_TRUE(strstr(directive, "birthday") != NULL);
    HU_ASSERT_TRUE(strstr(directive, "job search") != NULL);
    HU_ASSERT_TRUE(strstr(directive, "honest") != NULL);
    HU_ASSERT_TRUE(strstr(directive, "remembers") != NULL);
    HU_ASSERT_TRUE(strstr(directive, "tracks") != NULL);

    hu_str_free(&alloc, directive);
    hu_tom_gaps_free(&alloc, gaps, gap_count);
    hu_tom_deinit(&state, &alloc);
}

static void build_gap_directive_null_on_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t len = 0;
    char *d = hu_tom_build_gap_directive(&alloc, NULL, 0, &len);
    HU_ASSERT_NULL(d);
}

/* --- hu_tom_detect_user_expectation --- */

static void detect_expectation_remember_when(void) {
    const char *text = "Do you remember when we went hiking?";
    size_t text_len = strlen(text);
    const char *topic = NULL;
    size_t topic_len = 0;
    hu_tom_expected_knowledge_t type;

    bool found = hu_tom_detect_user_expectation(text, text_len, &topic, &topic_len, &type);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_TRUE(topic_len > 0);
    HU_ASSERT_EQ((int)type, (int)HU_TOM_EXPECT_REMEMBERS);
    /* Topic should be "we went hiking" */
    HU_ASSERT_TRUE(strncmp(topic, "we went hiking", 14) == 0);
}

static void detect_expectation_you_know_my(void) {
    const char *text = "you know my favorite color";
    size_t text_len = strlen(text);
    const char *topic = NULL;
    size_t topic_len = 0;
    hu_tom_expected_knowledge_t type;

    bool found = hu_tom_detect_user_expectation(text, text_len, &topic, &topic_len, &type);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_EQ((int)type, (int)HU_TOM_EXPECT_REMEMBERS);
    HU_ASSERT_TRUE(topic_len > 0);
}

static void detect_expectation_case_insensitive(void) {
    const char *text = "As You Know, I'm working on a project";
    size_t text_len = strlen(text);
    const char *topic = NULL;
    size_t topic_len = 0;
    hu_tom_expected_knowledge_t type;

    bool found = hu_tom_detect_user_expectation(text, text_len, &topic, &topic_len, &type);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_EQ((int)type, (int)HU_TOM_EXPECT_UNDERSTANDS);
}

static void detect_expectation_no_match(void) {
    const char *text = "How is the weather today?";
    size_t text_len = strlen(text);
    const char *topic = NULL;
    size_t topic_len = 0;
    hu_tom_expected_knowledge_t type;

    bool found = hu_tom_detect_user_expectation(text, text_len, &topic, &topic_len, &type);
    HU_ASSERT_FALSE(found);
}

static void detect_expectation_tracking(void) {
    const char *text = "you've been following my marathon training";
    size_t text_len = strlen(text);
    const char *topic = NULL;
    size_t topic_len = 0;
    hu_tom_expected_knowledge_t type;

    bool found = hu_tom_detect_user_expectation(text, text_len, &topic, &topic_len, &type);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_EQ((int)type, (int)HU_TOM_EXPECT_TRACKS);
}

/* --- deinit frees expectations --- */

static void deinit_frees_expectations(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tom_belief_state_t state;
    memset(&state, 0, sizeof(state));
    hu_tom_init(&state, &alloc, "heidi", 5);

    hu_tom_record_user_expectation(&state, &alloc, "topic_a", 7, HU_TOM_EXPECT_REMEMBERS);
    hu_tom_record_user_expectation(&state, &alloc, "topic_b", 7, HU_TOM_EXPECT_TRACKS);
    HU_ASSERT_EQ(state.expectation_count, 2u);

    hu_tom_deinit(&state, &alloc);
    HU_ASSERT_EQ(state.expectation_count, 0u);
}

/* Pins the 2026-07 service-loop SIGABRT: at_append_tom_directive passed an
 * uninitialized stack struct to hu_tom_init, which historically initialized
 * beliefs but NOT expectations — hu_tom_deinit then freed garbage
 * expectations[i].topic pointers. hu_tom_init must fully initialize the
 * struct without requiring a caller-side memset. */
static void init_fully_initializes_dirty_struct(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tom_belief_state_t state;
    memset(&state, 0xAB, sizeof(state)); /* deliberately NOT zeroed */

    HU_ASSERT_EQ(hu_tom_init(&state, &alloc, "ivan", 4), HU_OK);
    HU_ASSERT_EQ(state.expectation_count, 0u);
    HU_ASSERT_EQ(state.belief_count, 0u);
    HU_ASSERT_NULL(state.expectations[0].topic);
    HU_ASSERT_NULL(state.expectations[HU_TOM_MAX_EXPECTATIONS - 1].topic);

    /* The crash path: deinit on a freshly-inited state must be safe. */
    hu_tom_deinit(&state, &alloc);
}

void run_mutual_tom_tests(void) {
    HU_TEST_SUITE("mutual_tom");
    HU_RUN_TEST(record_expectation_stores_topic);
    HU_RUN_TEST(record_expectation_updates_existing);
    HU_RUN_TEST(record_expectation_null_returns_error);
    HU_RUN_TEST(detect_gaps_finds_missing_knowledge);
    HU_RUN_TEST(detect_gaps_no_gap_when_ai_knows);
    HU_RUN_TEST(detect_gaps_low_confidence_still_gap);
    HU_RUN_TEST(detect_gaps_empty_expectations_returns_zero);
    HU_RUN_TEST(build_gap_directive_produces_context);
    HU_RUN_TEST(build_gap_directive_null_on_empty);
    HU_RUN_TEST(detect_expectation_remember_when);
    HU_RUN_TEST(detect_expectation_you_know_my);
    HU_RUN_TEST(detect_expectation_case_insensitive);
    HU_RUN_TEST(detect_expectation_no_match);
    HU_RUN_TEST(detect_expectation_tracking);
    HU_RUN_TEST(deinit_frees_expectations);
    HU_RUN_TEST(init_fully_initializes_dirty_struct);
}
