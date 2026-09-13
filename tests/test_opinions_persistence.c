/* Covers src/memory/evolved_opinions.c (hu_evolved_opinion_* below). The
 * check-test-references name heuristic resolves "opinions" to
 * src/memory/opinions.c, a different module — hence the opt-out. */
// @covers-none
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

#include "human/memory/evolved_opinions.h"
#include <sqlite3.h>

static sqlite3 *s_db = NULL;

static void setup_db(void) {
    if (s_db) {
        sqlite3_close(s_db);
        s_db = NULL;
    }
    sqlite3_open(":memory:", &s_db);
    hu_evolved_opinions_ensure_table(s_db);
}

static void teardown_db(void) {
    if (s_db) {
        sqlite3_close(s_db);
        s_db = NULL;
    }
}

#define S(lit) (lit), (sizeof(lit) - 1)

static void opinions_ensure_table(void) {
    setup_db();
    HU_ASSERT(s_db != NULL);
    /* Table should exist — try inserting directly */
    hu_error_t err = hu_evolved_opinion_upsert(s_db, S("testing"), S("important"), 0.7, 1000);
    HU_ASSERT_EQ(err, HU_OK);
    teardown_db();
}

static void opinions_upsert_new(void) {
    setup_db();
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err =
        hu_evolved_opinion_upsert(s_db, S("remote work"), S("generally positive"), 0.6, 1000);
    HU_ASSERT_EQ(err, HU_OK);

    hu_evolved_opinion_t *ops = NULL;
    size_t count = 0;
    err = hu_evolved_opinions_get(&alloc, s_db, 0.0, 10, &ops, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_STR_EQ(ops[0].topic, "remote work");
    HU_ASSERT_STR_EQ(ops[0].stance, "generally positive");
    HU_ASSERT(ops[0].conviction > 0.5);
    HU_ASSERT_EQ(ops[0].interactions, 1);
    hu_evolved_opinions_free(&alloc, ops, count);
    teardown_db();
}

static void opinions_upsert_update_blends(void) {
    setup_db();
    hu_allocator_t alloc = hu_system_allocator();
    hu_evolved_opinion_upsert(s_db, S("TDD"), S("overrated"), 0.5, 1000);
    hu_evolved_opinion_upsert(s_db, S("TDD"), S("has its place"), 0.8, 2000);

    hu_evolved_opinion_t *ops = NULL;
    size_t count = 0;
    hu_evolved_opinions_get(&alloc, s_db, 0.0, 10, &ops, &count);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_STR_EQ(ops[0].stance, "has its place");
    /* Conviction should be blended: (0.5 + 0.8) / 2 = 0.65 */
    HU_ASSERT(ops[0].conviction > 0.6);
    HU_ASSERT(ops[0].conviction < 0.7);
    HU_ASSERT_EQ(ops[0].interactions, 2);
    hu_evolved_opinions_free(&alloc, ops, count);
    teardown_db();
}

static void opinions_get_filters_by_conviction(void) {
    setup_db();
    hu_allocator_t alloc = hu_system_allocator();
    hu_evolved_opinion_upsert(s_db, S("topic-a"), S("weak opinion"), 0.2, 1000);
    hu_evolved_opinion_upsert(s_db, S("topic-b"), S("strong opinion"), 0.9, 1000);

    hu_evolved_opinion_t *ops = NULL;
    size_t count = 0;
    hu_evolved_opinions_get(&alloc, s_db, 0.5, 10, &ops, &count);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_STR_EQ(ops[0].topic, "topic-b");
    hu_evolved_opinions_free(&alloc, ops, count);
    teardown_db();
}

static void opinions_get_respects_limit(void) {
    setup_db();
    hu_allocator_t alloc = hu_system_allocator();
    for (int i = 0; i < 10; i++) {
        char topic[32], stance[32];
        snprintf(topic, sizeof(topic), "topic-%d", i);
        snprintf(stance, sizeof(stance), "stance-%d", i);
        hu_evolved_opinion_upsert(s_db, topic, strlen(topic), stance, strlen(stance), 0.8, 1000);
    }

    hu_evolved_opinion_t *ops = NULL;
    size_t count = 0;
    hu_evolved_opinions_get(&alloc, s_db, 0.0, 3, &ops, &count);
    HU_ASSERT_EQ(count, 3);
    hu_evolved_opinions_free(&alloc, ops, count);
    teardown_db();
}

static void opinions_get_empty_table(void) {
    setup_db();
    hu_allocator_t alloc = hu_system_allocator();
    hu_evolved_opinion_t *ops = NULL;
    size_t count = 0;
    hu_error_t err = hu_evolved_opinions_get(&alloc, s_db, 0.0, 10, &ops, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0);
    teardown_db();
}

static void opinions_upsert_null_args(void) {
    setup_db();
    hu_error_t err = hu_evolved_opinion_upsert(s_db, NULL, 0, S("stance"), 0.5, 1000);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
    err = hu_evolved_opinion_upsert(s_db, S("topic"), NULL, 0, 0.5, 1000);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
    err = hu_evolved_opinion_upsert(NULL, S("topic"), S("stance"), 0.5, 1000);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
    teardown_db();
}

static void opinions_get_null_args(void) {
    setup_db();
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_evolved_opinions_get(NULL, s_db, 0.0, 10, NULL, NULL);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
    hu_evolved_opinion_t *ops = NULL;
    size_t count = 0;
    err = hu_evolved_opinions_get(&alloc, NULL, 0.0, 10, &ops, &count);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
    teardown_db();
}

static void opinions_conviction_clamped(void) {
    setup_db();
    hu_allocator_t alloc = hu_system_allocator();
    hu_evolved_opinion_upsert(s_db, S("over"), S("too high"), 2.0, 1000);
    hu_evolved_opinion_upsert(s_db, S("under"), S("too low"), -1.0, 1000);

    hu_evolved_opinion_t *ops = NULL;
    size_t count = 0;
    hu_evolved_opinions_get(&alloc, s_db, 0.0, 10, &ops, &count);
    HU_ASSERT_EQ(count, 2);
    for (size_t i = 0; i < count; i++) {
        HU_ASSERT(ops[i].conviction >= 0.0);
        HU_ASSERT(ops[i].conviction <= 1.0);
    }
    hu_evolved_opinions_free(&alloc, ops, count);
    teardown_db();
}

static void opinions_multiple_updates_increment(void) {
    setup_db();
    hu_allocator_t alloc = hu_system_allocator();
    for (int i = 0; i < 5; i++) {
        hu_evolved_opinion_upsert(s_db, S("evolving"), S("still thinking"), 0.6, 1000 + i);
    }

    hu_evolved_opinion_t *ops = NULL;
    size_t count = 0;
    hu_evolved_opinions_get(&alloc, s_db, 0.0, 10, &ops, &count);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_EQ(ops[0].interactions, 5);
    hu_evolved_opinions_free(&alloc, ops, count);
    teardown_db();
}

static void opinions_ordered_by_conviction(void) {
    setup_db();
    hu_allocator_t alloc = hu_system_allocator();
    hu_evolved_opinion_upsert(s_db, S("low"), S("meh"), 0.3, 1000);
    hu_evolved_opinion_upsert(s_db, S("high"), S("strong"), 0.9, 1000);
    hu_evolved_opinion_upsert(s_db, S("mid"), S("okay"), 0.6, 1000);

    hu_evolved_opinion_t *ops = NULL;
    size_t count = 0;
    hu_evolved_opinions_get(&alloc, s_db, 0.0, 10, &ops, &count);
    HU_ASSERT_EQ(count, 3);
    HU_ASSERT(ops[0].conviction >= ops[1].conviction);
    HU_ASSERT(ops[1].conviction >= ops[2].conviction);
    hu_evolved_opinions_free(&alloc, ops, count);
    teardown_db();
}

static void extract_finds_i_think(void) {
    setup_db();
    hu_allocator_t alloc = hu_system_allocator();
    const char *resp = "I think remote work is generally better for deep focus. "
                       "But offices have their place too.";
    hu_evolved_opinions_extract_and_store(s_db, resp, strlen(resp), 1000);

    hu_evolved_opinion_t *ops = NULL;
    size_t count = 0;
    hu_evolved_opinions_get(&alloc, s_db, 0.0, 10, &ops, &count);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT(ops[0].topic_len > 0);
    HU_ASSERT(ops[0].stance_len > 0);
    hu_evolved_opinions_free(&alloc, ops, count);
    teardown_db();
}

static void extract_finds_multiple_markers(void) {
    setup_db();
    hu_allocator_t alloc = hu_system_allocator();
    const char *resp = "I believe testing is essential for quality. "
                       "I prefer integration tests over unit tests. "
                       "Simple code needs less coverage.";
    hu_evolved_opinions_extract_and_store(s_db, resp, strlen(resp), 1000);

    hu_evolved_opinion_t *ops = NULL;
    size_t count = 0;
    hu_evolved_opinions_get(&alloc, s_db, 0.0, 10, &ops, &count);
    HU_ASSERT(count >= 2);
    hu_evolved_opinions_free(&alloc, ops, count);
    teardown_db();
}

static void extract_caps_at_three(void) {
    setup_db();
    hu_allocator_t alloc = hu_system_allocator();
    const char *resp = "I think A is good. I think B is good. "
                       "I think C is good. I think D is good.";
    hu_evolved_opinions_extract_and_store(s_db, resp, strlen(resp), 1000);

    hu_evolved_opinion_t *ops = NULL;
    size_t count = 0;
    hu_evolved_opinions_get(&alloc, s_db, 0.0, 10, &ops, &count);
    HU_ASSERT(count <= 3);
    hu_evolved_opinions_free(&alloc, ops, count);
    teardown_db();
}

static void extract_no_opinions_in_factual(void) {
    setup_db();
    hu_allocator_t alloc = hu_system_allocator();
    const char *resp = "The function returns an error code. "
                       "You can handle it with a switch statement.";
    hu_evolved_opinions_extract_and_store(s_db, resp, strlen(resp), 1000);

    hu_evolved_opinion_t *ops = NULL;
    size_t count = 0;
    hu_evolved_opinions_get(&alloc, s_db, 0.0, 10, &ops, &count);
    HU_ASSERT_EQ(count, 0);
    teardown_db();
}

/* The two rows production actually stored (memory.db, 2026-05-12 and
 * 2026-05-17): the twin's own identity slips, harvested as "opinions" and
 * injected into every live prompt until 2026-09-13. Must store nothing. */
static void extract_rejects_the_twins_identity_slips(void) {
    setup_db();
    hu_allocator_t alloc = hu_system_allocator();
    const char *slips[] = {"lol wait. I think I just glitched for a second. what were we saying",
                           "I think you might be confusing me with someone else. I never said that",
                           "As an AI language model I think that is a fair point.", NULL};
    for (int k = 0; slips[k]; k++)
        hu_evolved_opinions_extract_and_store(s_db, slips[k], strlen(slips[k]), 1000);
    hu_evolved_opinion_t *ops = NULL;
    size_t count = 0;
    hu_evolved_opinions_get(&alloc, s_db, 0.0, 10, &ops, &count);
    HU_ASSERT_EQ(count, 0);
    teardown_db();
}

static void stance_predicate_truth_table(void) {
    /* topic opens on a pronoun -> not a position about anything */
    HU_ASSERT_FALSE(hu_evolved_opinion_stance_is_usable(
        "I just glitched for a", 21, "I think I just glitched for a second", 36));
    HU_ASSERT_FALSE(hu_evolved_opinion_stance_is_usable("you might be confusing me", 25,
                                                        "I think you might be confusing me", 33));
    HU_ASSERT_FALSE(hu_evolved_opinion_stance_is_usable("it's fine", 9, "I think it's fine", 17));
    /* an AI slip anywhere in the stance */
    HU_ASSERT_FALSE(hu_evolved_opinion_stance_is_usable(
        "remote work is fine", 19, "I think remote work is fine, as an AI", 37));
    /* a real position survives, including one that mentions "you" later */
    HU_ASSERT_TRUE(hu_evolved_opinion_stance_is_usable(
        "remote work is generally better", 31,
        "I think remote work is generally better for deep focus", 54));
    HU_ASSERT_TRUE(hu_evolved_opinion_stance_is_usable(
        "hybrid beats full remote", 24, "I think hybrid beats full remote for you", 40));
    HU_ASSERT_FALSE(hu_evolved_opinion_stance_is_usable(NULL, 0, "x", 1));
    HU_ASSERT_FALSE(hu_evolved_opinion_stance_is_usable("x", 1, NULL, 0));
}

static void extract_null_args(void) {
    setup_db();
    hu_error_t err = hu_evolved_opinions_extract_and_store(NULL, "test", 4, 1000);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
    err = hu_evolved_opinions_extract_and_store(s_db, NULL, 0, 1000);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
    teardown_db();
}

static void extract_repeated_topic_blends(void) {
    setup_db();
    hu_allocator_t alloc = hu_system_allocator();
    hu_evolved_opinions_extract_and_store(s_db, "I think remote work is great for focus.", 40,
                                          1000);
    hu_evolved_opinions_extract_and_store(
        s_db, "I think remote work is essential for productivity.", 50, 2000);

    hu_evolved_opinion_t *ops = NULL;
    size_t count = 0;
    hu_evolved_opinions_get(&alloc, s_db, 0.0, 10, &ops, &count);
    /* Same topic should blend, not duplicate */
    HU_ASSERT(count <= 2);
    hu_evolved_opinions_free(&alloc, ops, count);
    teardown_db();
}

#endif /* HU_ENABLE_SQLITE */

int run_opinions_persistence_tests(void) {
    HU_TEST_SUITE("opinions_persistence");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(opinions_ensure_table);
    HU_RUN_TEST(opinions_upsert_new);
    HU_RUN_TEST(opinions_upsert_update_blends);
    HU_RUN_TEST(opinions_get_filters_by_conviction);
    HU_RUN_TEST(opinions_get_respects_limit);
    HU_RUN_TEST(opinions_get_empty_table);
    HU_RUN_TEST(opinions_upsert_null_args);
    HU_RUN_TEST(opinions_get_null_args);
    HU_RUN_TEST(opinions_conviction_clamped);
    HU_RUN_TEST(opinions_multiple_updates_increment);
    HU_RUN_TEST(opinions_ordered_by_conviction);
    HU_RUN_TEST(extract_finds_i_think);
    HU_RUN_TEST(extract_finds_multiple_markers);
    HU_RUN_TEST(extract_caps_at_three);
    HU_RUN_TEST(extract_no_opinions_in_factual);
    HU_RUN_TEST(extract_null_args);
    HU_RUN_TEST(extract_repeated_topic_blends);
    HU_RUN_TEST(extract_rejects_the_twins_identity_slips);
    HU_RUN_TEST(stance_predicate_truth_table);
#endif
    return 0;
}
