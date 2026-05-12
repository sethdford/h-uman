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
    hu_opinion_history_ensure_table(s_db);
}

static void teardown_db(void) {
    if (s_db) {
        sqlite3_close(s_db);
        s_db = NULL;
    }
}

#define S(lit) (lit), (sizeof(lit) - 1)

/* ── hu_opinion_history_ensure_table ─────────────────────────────────── */

static void history_table_creates_ok(void) {
    setup_db();
    hu_error_t err = hu_opinion_history_ensure_table(s_db);
    HU_ASSERT_EQ(err, HU_OK);
    /* Calling again is idempotent */
    err = hu_opinion_history_ensure_table(s_db);
    HU_ASSERT_EQ(err, HU_OK);
    teardown_db();
}

static void history_table_null_db_fails(void) {
    HU_ASSERT_EQ(hu_opinion_history_ensure_table(NULL), HU_ERR_INVALID_ARGUMENT);
}

/* ── hu_opinion_history_record ───────────────────────────────────────── */

static void history_record_stores_entry(void) {
    setup_db();
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_opinion_history_record(s_db, S("TDD"), S("overrated"), S("has its place"),
                                               S("experience"), 2000);
    HU_ASSERT_EQ(err, HU_OK);

    hu_opinion_history_entry_t *entries = NULL;
    size_t count = 0;
    err = hu_evolved_opinion_history(&alloc, s_db, S("TDD"), &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_STR_EQ(entries[0].old_stance, "overrated");
    HU_ASSERT_STR_EQ(entries[0].new_stance, "has its place");
    HU_ASSERT_STR_EQ(entries[0].change_reason, "experience");
    HU_ASSERT_EQ(entries[0].changed_at, 2000);
    hu_opinion_history_free(&alloc, entries, count);
    teardown_db();
}

static void history_record_null_args_fails(void) {
    setup_db();
    HU_ASSERT_EQ(hu_opinion_history_record(NULL, S("t"), S("a"), S("b"), S("r"), 1000),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_opinion_history_record(s_db, NULL, 0, S("a"), S("b"), S("r"), 1000),
                 HU_ERR_INVALID_ARGUMENT);
    teardown_db();
}

/* ── hu_evolved_opinion_history ──────────────────────────────────────── */

static void history_retrieves_multiple_entries_ordered(void) {
    setup_db();
    hu_allocator_t alloc = hu_system_allocator();
    hu_opinion_history_record(s_db, S("AI"), S("tool"), S("partner"), S("first shift"), 1000);
    hu_opinion_history_record(s_db, S("AI"), S("partner"), S("collaborator"), S("second shift"),
                              2000);

    hu_opinion_history_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = hu_evolved_opinion_history(&alloc, s_db, S("AI"), &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2);
    /* Ordered by changed_at ASC */
    HU_ASSERT(entries[0].changed_at <= entries[1].changed_at);
    HU_ASSERT_STR_EQ(entries[0].old_stance, "tool");
    HU_ASSERT_STR_EQ(entries[1].old_stance, "partner");
    hu_opinion_history_free(&alloc, entries, count);
    teardown_db();
}

static void history_empty_for_unknown_topic(void) {
    setup_db();
    hu_allocator_t alloc = hu_system_allocator();
    hu_opinion_history_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = hu_evolved_opinion_history(&alloc, s_db, S("nonexistent"), &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0);
    teardown_db();
}

static void history_null_args_fails(void) {
    setup_db();
    hu_allocator_t alloc = hu_system_allocator();
    hu_opinion_history_entry_t *entries = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_evolved_opinion_history(NULL, s_db, S("x"), &entries, &count),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_evolved_opinion_history(&alloc, NULL, S("x"), &entries, &count),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_evolved_opinion_history(&alloc, s_db, NULL, 0, &entries, &count),
                 HU_ERR_INVALID_ARGUMENT);
    teardown_db();
}

/* ── hu_evolved_opinion_upsert_with_history ──────────────────────────── */

static void upsert_with_history_gradual_shift_fires_narrative(void) {
    setup_db();
    hu_allocator_t alloc = hu_system_allocator();
    /* First: establish opinion with conviction 0.9 */
    hu_evolved_opinion_upsert(s_db, S("remote work"), S("great for focus"), 0.9, 1000);

    /* Now upsert with very different conviction (0.3) -> shift > 0.2 */
    size_t len = 0;
    char *narrative = hu_evolved_opinion_upsert_with_history(
        &alloc, s_db, S("remote work"), S("has drawbacks"), 0.3, 2000, S("experience"), 0, &len);
    HU_ASSERT_NOT_NULL(narrative);
    HU_ASSERT(len > 0);
    HU_ASSERT(strstr(narrative, "shifted") != NULL);
    HU_ASSERT(strstr(narrative, "rethinking") != NULL);
    alloc.free(alloc.ctx, narrative, len + 1);
    teardown_db();
}

static void upsert_with_history_small_shift_no_narrative(void) {
    setup_db();
    hu_allocator_t alloc = hu_system_allocator();
    hu_evolved_opinion_upsert(s_db, S("testing"), S("important"), 0.7, 1000);

    /* Small shift: conviction 0.8 -> blend (0.7+0.8)/2=0.75, shift 0.05 */
    size_t len = 0;
    char *narrative = hu_evolved_opinion_upsert_with_history(
        &alloc, s_db, S("testing"), S("very important"), 0.8, 2000, NULL, 0, 0, &len);
    HU_ASSERT_NULL(narrative);
    teardown_db();
}

static void upsert_with_history_rate_limited_at_two(void) {
    setup_db();
    hu_allocator_t alloc = hu_system_allocator();
    hu_evolved_opinion_upsert(s_db, S("topic-a"), S("stance-a"), 0.9, 1000);

    /* Already had 2 opinion changes this conversation -> rate limited */
    size_t len = 0;
    char *narrative = hu_evolved_opinion_upsert_with_history(
        &alloc, s_db, S("topic-a"), S("new stance"), 0.1, 2000, S("reason"), 2, &len);
    HU_ASSERT_NULL(narrative);
    teardown_db();
}

static void upsert_with_history_records_in_history_table(void) {
    setup_db();
    hu_allocator_t alloc = hu_system_allocator();
    hu_evolved_opinion_upsert(s_db, S("design"), S("minimalist"), 0.8, 1000);

    /* The 0.6-magnitude shift here triggers narrative generation
     * (evolved_opinions.c:556-574 allocates HU_NARRATIVE_CAP and
     * realloc-shrinks to ~70 b). Capture and free — discarding the
     * return value leaked 70 b on PR55's ASan run. */
    size_t narr_len = 0;
    char *narr = hu_evolved_opinion_upsert_with_history(
        &alloc, s_db, S("design"), S("expressive"), 0.2, 2000, S("grew bored"), 0, &narr_len);
    if (narr)
        alloc.free(alloc.ctx, narr, narr_len + 1);

    /* Check history was recorded */
    hu_opinion_history_entry_t *entries = NULL;
    size_t count = 0;
    hu_evolved_opinion_history(&alloc, s_db, S("design"), &entries, &count);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_STR_EQ(entries[0].old_stance, "minimalist");
    HU_ASSERT_STR_EQ(entries[0].new_stance, "expressive");
    hu_opinion_history_free(&alloc, entries, count);
    teardown_db();
}

static void upsert_with_history_new_topic_no_narrative(void) {
    setup_db();
    hu_allocator_t alloc = hu_system_allocator();

    /* Brand new topic — no prior opinion, so no shift */
    size_t len = 0;
    char *narrative = hu_evolved_opinion_upsert_with_history(
        &alloc, s_db, S("new topic"), S("initial stance"), 0.6, 1000, NULL, 0, 0, &len);
    HU_ASSERT_NULL(narrative);
    teardown_db();
}

#endif /* HU_ENABLE_SQLITE */

void run_opinion_history_tests(void) {
    HU_TEST_SUITE("opinion_history");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(history_table_creates_ok);
    HU_RUN_TEST(history_table_null_db_fails);
    HU_RUN_TEST(history_record_stores_entry);
    HU_RUN_TEST(history_record_null_args_fails);
    HU_RUN_TEST(history_retrieves_multiple_entries_ordered);
    HU_RUN_TEST(history_empty_for_unknown_topic);
    HU_RUN_TEST(history_null_args_fails);
    HU_RUN_TEST(upsert_with_history_gradual_shift_fires_narrative);
    HU_RUN_TEST(upsert_with_history_small_shift_no_narrative);
    HU_RUN_TEST(upsert_with_history_rate_limited_at_two);
    HU_RUN_TEST(upsert_with_history_records_in_history_table);
    HU_RUN_TEST(upsert_with_history_new_topic_no_narrative);
#endif
}
