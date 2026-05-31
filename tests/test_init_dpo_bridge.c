/* Contract tests for hu_init_dpo_bridge_record.
 *
 * Pins:
 *   (a) REPLIED outcome → row written with chosen=draft, rejected=""
 *   (b) IGNORED outcome → row written with chosen="", rejected=draft
 *   (c) PENDING outcome rejected with HU_ERR_INVALID_ARGUMENT (no row)
 *   (d) No collector registered → HU_ERR_NOT_SUPPORTED (no row, no crash)
 *   (e) source field on every written row equals HU_INIT_DPO_BRIDGE_SOURCE
 *
 * Gated symmetric with src/ml/init_dpo_bridge.c — both inside #ifdef
 * HU_ENABLE_ML. The runner stubs to a no-op when the flag is off so
 * the test source can stay unconditional in HU_TEST_SOURCES (per
 * .claude/rules/test-source-gate-symmetry.md option 2).
 */

#ifdef HU_ENABLE_ML

#include "human/agent/init_outcome.h"
#include "human/ml/dpo.h"
#include "human/ml/init_dpo_bridge.h"
#include "test_framework.h"
#include <stdint.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

static void mk_collector(hu_dpo_collector_t *col) {
    hu_allocator_t alloc = hu_system_allocator();
#ifdef HU_ENABLE_SQLITE
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 100, col), HU_OK);
#else
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, NULL, 100, col), HU_OK);
#endif
    HU_ASSERT_EQ(hu_dpo_init_tables(col), HU_OK);
}

/* Tear down a collector built by mk_collector. The collector only BORROWS the
 * sqlite handle (hu_dpo_collector_deinit just memsets — the caller owns the
 * db), and mk_collector opens an in-memory db that no one else can reach, so
 * without closing col->db here the entire :memory: database leaks. Linux
 * LeakSanitizer (rl_sota preset) flagged this across every test as allocations
 * from hu_dpo_init_tables / hu_dpo_record_pair. Close before deinit, since
 * deinit zeroes the struct. */
static void tear_collector(hu_dpo_collector_t *col) {
#ifdef HU_ENABLE_SQLITE
    if (col->db)
        sqlite3_close(col->db);
#endif
    hu_dpo_collector_deinit(col);
}

static void test_init_dpo_bridge_replied_sets_chosen(void) {
    hu_dpo_collector_t col;
    mk_collector(&col);
    hu_init_dpo_bridge_set_collector(&col);

    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(
        hu_init_dpo_bridge_record(&alloc, HU_INIT_RESOLUTION_REPLIED,
                                  "Hey, you mentioned the deploy yesterday — how did it go?",
                                  "+15551234567", 1716700000),
        HU_OK);

    size_t n = 0;
    HU_ASSERT_EQ(hu_dpo_pair_count(&col, &n), HU_OK);
    HU_ASSERT_EQ((int)n, 1);

    hu_init_dpo_bridge_set_collector(NULL);
    tear_collector(&col);
}

static void test_init_dpo_bridge_ignored_sets_rejected(void) {
    hu_dpo_collector_t col;
    mk_collector(&col);
    hu_init_dpo_bridge_set_collector(&col);

    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(
        hu_init_dpo_bridge_record(&alloc, HU_INIT_RESOLUTION_IGNORED,
                                  "Hey, you mentioned the deploy yesterday — how did it go?",
                                  "+15551234567", 1716700000),
        HU_OK);

    size_t n = 0;
    HU_ASSERT_EQ(hu_dpo_pair_count(&col, &n), HU_OK);
    HU_ASSERT_EQ((int)n, 1);

    hu_init_dpo_bridge_set_collector(NULL);
    tear_collector(&col);
}

static void test_init_dpo_bridge_pending_rejected(void) {
    hu_dpo_collector_t col;
    mk_collector(&col);
    hu_init_dpo_bridge_set_collector(&col);

    hu_allocator_t alloc = hu_system_allocator();
    /* PENDING is never a settled outcome — the bridge MUST reject it.
     * Asserting INVALID_ARGUMENT specifically (not just "non-OK")
     * pins the contract per .claude/rules/tests-that-pin-bugs.md. */
    HU_ASSERT_EQ(hu_init_dpo_bridge_record(&alloc, HU_INIT_RESOLUTION_PENDING, "draft",
                                           "+15551234567", 1716700000),
                 HU_ERR_INVALID_ARGUMENT);

    /* No row written. */
    size_t n = 0;
    HU_ASSERT_EQ(hu_dpo_pair_count(&col, &n), HU_OK);
    HU_ASSERT_EQ((int)n, 0);

    hu_init_dpo_bridge_set_collector(NULL);
    tear_collector(&col);
}

static void test_init_dpo_bridge_no_collector_returns_not_supported(void) {
    hu_init_dpo_bridge_set_collector(NULL);
    HU_ASSERT(hu_init_dpo_bridge_get_collector() == NULL);

    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_init_dpo_bridge_record(&alloc, HU_INIT_RESOLUTION_REPLIED, "draft",
                                           "+15551234567", 1716700000),
                 HU_ERR_NOT_SUPPORTED);
}

#ifdef HU_ENABLE_SQLITE
static void test_init_dpo_bridge_pair_singles_pairs_replied_with_ignored(void) {
    /* Two single-sided rows for the same target, IGNORED earlier than
     * REPLIED. The pairing pass should produce one paired row and
     * mark both source rows with the sentinel margin. */
    hu_dpo_collector_t col;
    mk_collector(&col);
    hu_init_dpo_bridge_set_collector(&col);
    hu_allocator_t alloc = hu_system_allocator();

    /* T=100: IGNORED draft, target=+1555. */
    HU_ASSERT_EQ(hu_init_dpo_bridge_record(&alloc, HU_INIT_RESOLUTION_IGNORED,
                                           "Want to grab coffee tomorrow?", "+15551234567", 100),
                 HU_OK);
    /* T=200: REPLIED draft, same target. */
    HU_ASSERT_EQ(hu_init_dpo_bridge_record(&alloc, HU_INIT_RESOLUTION_REPLIED,
                                           "How did the deploy go yesterday?", "+15551234567", 200),
                 HU_OK);

    size_t before = 0;
    HU_ASSERT_EQ(hu_dpo_pair_count(&col, &before), HU_OK);
    HU_ASSERT_EQ((int)before, 2);

    /* Pair them. */
    size_t paired = 0;
    HU_ASSERT_EQ(hu_init_dpo_bridge_pair_singles(&alloc, &paired), HU_OK);
    HU_ASSERT_EQ((int)paired, 1);

    /* dpo_pairs now has 3 rows: 2 original (with sentinel margin) + 1
     * paired (with margin=1.0). */
    size_t after = 0;
    HU_ASSERT_EQ(hu_dpo_pair_count(&col, &after), HU_OK);
    HU_ASSERT_EQ((int)after, 3);

    hu_init_dpo_bridge_set_collector(NULL);
    tear_collector(&col);
}

static void test_init_dpo_bridge_pair_singles_is_idempotent(void) {
    /* Running the pairing pass a second time on the same state must
     * produce zero new pairs — the sentinel margin marks already-
     * processed rows. */
    hu_dpo_collector_t col;
    mk_collector(&col);
    hu_init_dpo_bridge_set_collector(&col);
    hu_allocator_t alloc = hu_system_allocator();

    HU_ASSERT_EQ(hu_init_dpo_bridge_record(&alloc, HU_INIT_RESOLUTION_IGNORED, "draft A",
                                           "+15551234567", 100),
                 HU_OK);
    HU_ASSERT_EQ(hu_init_dpo_bridge_record(&alloc, HU_INIT_RESOLUTION_REPLIED, "draft B",
                                           "+15551234567", 200),
                 HU_OK);

    size_t paired_first = 0;
    HU_ASSERT_EQ(hu_init_dpo_bridge_pair_singles(&alloc, &paired_first), HU_OK);
    HU_ASSERT_EQ((int)paired_first, 1);

    size_t paired_again = 0;
    HU_ASSERT_EQ(hu_init_dpo_bridge_pair_singles(&alloc, &paired_again), HU_OK);
    HU_ASSERT_EQ((int)paired_again, 0);

    hu_init_dpo_bridge_set_collector(NULL);
    tear_collector(&col);
}

static void test_init_dpo_bridge_pair_singles_does_not_cross_targets(void) {
    /* IGNORED for target_A + REPLIED for target_B must NOT pair —
     * each target's gradient is independent. */
    hu_dpo_collector_t col;
    mk_collector(&col);
    hu_init_dpo_bridge_set_collector(&col);
    hu_allocator_t alloc = hu_system_allocator();

    HU_ASSERT_EQ(
        hu_init_dpo_bridge_record(&alloc, HU_INIT_RESOLUTION_IGNORED, "draft for A", "+1AAA", 100),
        HU_OK);
    HU_ASSERT_EQ(
        hu_init_dpo_bridge_record(&alloc, HU_INIT_RESOLUTION_REPLIED, "draft for B", "+1BBB", 200),
        HU_OK);

    size_t paired = 0;
    HU_ASSERT_EQ(hu_init_dpo_bridge_pair_singles(&alloc, &paired), HU_OK);
    HU_ASSERT_EQ((int)paired, 0);

    hu_init_dpo_bridge_set_collector(NULL);
    tear_collector(&col);
}

static void test_init_dpo_bridge_pair_singles_no_collector_returns_not_supported(void) {
    hu_init_dpo_bridge_set_collector(NULL);
    hu_allocator_t alloc = hu_system_allocator();
    size_t paired = 999;
    HU_ASSERT_EQ(hu_init_dpo_bridge_pair_singles(&alloc, &paired), HU_ERR_NOT_SUPPORTED);
    /* On NOT_SUPPORTED, paired_count must be zeroed (the contract). */
    HU_ASSERT_EQ((int)paired, 0);
}
#endif /* HU_ENABLE_SQLITE */

/* Pure threshold predicate — testable without any sqlite or collector
 * state. Per .claude/rules/security-predicate-extraction.md, the
 * truth table is locked here so the daemon-tick wire never has to
 * carry test machinery. */

static void test_should_pair_now_fires_at_threshold(void) {
    /* Boundary: exactly == threshold fires. */
    HU_ASSERT(hu_init_dpo_bridge_should_pair_now(10, 10));
}

static void test_should_pair_now_below_threshold_returns_false(void) {
    HU_ASSERT_FALSE(hu_init_dpo_bridge_should_pair_now(9, 10));
    HU_ASSERT_FALSE(hu_init_dpo_bridge_should_pair_now(0, 10));
    HU_ASSERT_FALSE(hu_init_dpo_bridge_should_pair_now(1, 10));
}

static void test_should_pair_now_zero_threshold_disables(void) {
    /* Zero is the "operator disabled auto-pair" signal — even with a
     * huge accumulated count, the predicate must NOT fire. Pinned
     * because a naive `>=` implementation would return true for
     * (anything >= 0). */
    HU_ASSERT_FALSE(hu_init_dpo_bridge_should_pair_now(0, 0));
    HU_ASSERT_FALSE(hu_init_dpo_bridge_should_pair_now(100, 0));
    HU_ASSERT_FALSE(hu_init_dpo_bridge_should_pair_now((size_t)-1, 0));
}

static void test_should_pair_now_above_threshold_still_fires(void) {
    /* If the daemon misses several ticks and accumulated count >>
     * threshold, the next call still fires (we don't gate on
     * exact-equality). */
    HU_ASSERT(hu_init_dpo_bridge_should_pair_now(50, 10));
    HU_ASSERT(hu_init_dpo_bridge_should_pair_now(1000, 10));
}

static void test_init_dpo_bridge_set_collector_round_trips(void) {
    hu_dpo_collector_t col;
    mk_collector(&col);
    hu_init_dpo_bridge_set_collector(&col);
    HU_ASSERT(hu_init_dpo_bridge_get_collector() == &col);

    hu_init_dpo_bridge_set_collector(NULL);
    HU_ASSERT(hu_init_dpo_bridge_get_collector() == NULL);

    tear_collector(&col);
}

void run_init_dpo_bridge_tests(void) {
    HU_TEST_SUITE("init-dpo-bridge");
    HU_RUN_TEST(test_init_dpo_bridge_replied_sets_chosen);
    HU_RUN_TEST(test_init_dpo_bridge_ignored_sets_rejected);
    HU_RUN_TEST(test_init_dpo_bridge_pending_rejected);
    HU_RUN_TEST(test_init_dpo_bridge_no_collector_returns_not_supported);
    HU_RUN_TEST(test_init_dpo_bridge_set_collector_round_trips);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_init_dpo_bridge_pair_singles_pairs_replied_with_ignored);
    HU_RUN_TEST(test_init_dpo_bridge_pair_singles_is_idempotent);
    HU_RUN_TEST(test_init_dpo_bridge_pair_singles_does_not_cross_targets);
    HU_RUN_TEST(test_init_dpo_bridge_pair_singles_no_collector_returns_not_supported);
#endif
    HU_RUN_TEST(test_should_pair_now_fires_at_threshold);
    HU_RUN_TEST(test_should_pair_now_below_threshold_returns_false);
    HU_RUN_TEST(test_should_pair_now_zero_threshold_disables);
    HU_RUN_TEST(test_should_pair_now_above_threshold_still_fires);
}

#else /* HU_ENABLE_ML */

void run_init_dpo_bridge_tests(void) {
    /* Stub when ML disabled — test/source gate symmetry option 2. */
}

#endif /* HU_ENABLE_ML */
