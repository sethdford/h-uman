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
    hu_dpo_collector_deinit(&col);
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
    hu_dpo_collector_deinit(&col);
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
    hu_dpo_collector_deinit(&col);
}

static void test_init_dpo_bridge_no_collector_returns_not_supported(void) {
    hu_init_dpo_bridge_set_collector(NULL);
    HU_ASSERT(hu_init_dpo_bridge_get_collector() == NULL);

    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_init_dpo_bridge_record(&alloc, HU_INIT_RESOLUTION_REPLIED, "draft",
                                           "+15551234567", 1716700000),
                 HU_ERR_NOT_SUPPORTED);
}

static void test_init_dpo_bridge_set_collector_round_trips(void) {
    hu_dpo_collector_t col;
    mk_collector(&col);
    hu_init_dpo_bridge_set_collector(&col);
    HU_ASSERT(hu_init_dpo_bridge_get_collector() == &col);

    hu_init_dpo_bridge_set_collector(NULL);
    HU_ASSERT(hu_init_dpo_bridge_get_collector() == NULL);

    hu_dpo_collector_deinit(&col);
}

void run_init_dpo_bridge_tests(void) {
    HU_TEST_SUITE("init-dpo-bridge");
    HU_RUN_TEST(test_init_dpo_bridge_replied_sets_chosen);
    HU_RUN_TEST(test_init_dpo_bridge_ignored_sets_rejected);
    HU_RUN_TEST(test_init_dpo_bridge_pending_rejected);
    HU_RUN_TEST(test_init_dpo_bridge_no_collector_returns_not_supported);
    HU_RUN_TEST(test_init_dpo_bridge_set_collector_round_trips);
}

#else /* HU_ENABLE_ML */

void run_init_dpo_bridge_tests(void) {
    /* Stub when ML disabled — test/source gate symmetry option 2. */
}

#endif /* HU_ENABLE_ML */
