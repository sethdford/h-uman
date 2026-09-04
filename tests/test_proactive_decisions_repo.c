/* Exercises hu_proactive_decisions_repo_{ensure_schema,record,count} in
 * src/memory/repos/proactive_decisions_repo_sqlite.c. Contract C5, Part A.
 * @covers-none — scripts/check-test-references.sh's basename-stripping
 * heuristic collides "proactive_decisions_repo" -> "proactive_decisions" ->
 * "proactive" with the UNRELATED single match src/agent/proactive.c and
 * stops there (fast path on a single match), so it would otherwise demand
 * this file reference proactive.c symbols it has nothing to do with. The
 * real production file for this test is named above.
 */
#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include "human/memory/proactive_decisions_repo.h"
#include "test_framework.h"
#include <sqlite3.h>
#include <string.h>

static void test_proactive_decisions_repo_record_and_count(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    int64_t count = -1;
    HU_ASSERT_EQ(hu_proactive_decisions_repo_count(db, &count), HU_OK);
    HU_ASSERT_EQ(count, 0);

    HU_ASSERT_EQ(
        hu_proactive_decisions_repo_record(db, 1000, "+15551234567", "init_proposer_governor",
                                           HU_PROACTIVE_DECISION_DECLINE, "quiet_hours", 0, NULL),
        HU_OK);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_record(db, 1001, "+15551234567", "init_proposer_llm",
                                                    HU_PROACTIVE_DECISION_SEND, NULL, 1,
                                                    "checked in about the game"),
                 HU_OK);
    /* contact=NULL is valid — a tick-level decision not yet scoped to a
     * specific contact. */
    HU_ASSERT_EQ(hu_proactive_decisions_repo_record(db, 1002, NULL, "init_proposer_governor",
                                                    HU_PROACTIVE_DECISION_DEFER, "budget_exhausted",
                                                    0, NULL),
                 HU_OK);

    HU_ASSERT_EQ(hu_proactive_decisions_repo_count(db, &count), HU_OK);
    HU_ASSERT_EQ(count, 3);

    mem.vtable->deinit(mem.ctx);
}

static void test_proactive_decisions_repo_rejects_invalid_decision(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    /* Per reports-success-does-nothing.md: a typo'd decision string must be
     * REJECTED, not silently inserted as an uncategorized row. */
    HU_ASSERT_EQ(
        hu_proactive_decisions_repo_record(db, 1000, "c1", "trigger", "maybe", NULL, 0, NULL),
        HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_record(db, 1000, "c1", "trigger", NULL, NULL, 0, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_record(db, 1000, "c1", "", HU_PROACTIVE_DECISION_SEND,
                                                    NULL, 0, NULL),
                 HU_ERR_INVALID_ARGUMENT);

    int64_t count = -1;
    HU_ASSERT_EQ(hu_proactive_decisions_repo_count(db, &count), HU_OK);
    HU_ASSERT_EQ(count, 0);

    mem.vtable->deinit(mem.ctx);
}

static void test_proactive_decisions_repo_rejects_null_db(void) {
    int64_t count = -1;
    HU_ASSERT_EQ(hu_proactive_decisions_repo_count(NULL, &count), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_record(NULL, 0, NULL, "t", HU_PROACTIVE_DECISION_SEND,
                                                    NULL, 0, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_ensure_schema(NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_proactive_decisions_repo_ensure_schema_idempotent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    HU_ASSERT_EQ(hu_proactive_decisions_repo_ensure_schema(db), HU_OK);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_ensure_schema(db), HU_OK);
    HU_ASSERT_EQ(hu_proactive_decisions_repo_ensure_schema(db), HU_OK);

    mem.vtable->deinit(mem.ctx);
}

void run_proactive_decisions_repo_tests(void) {
    HU_TEST_SUITE("proactive_decisions_repo");
    HU_RUN_TEST(test_proactive_decisions_repo_record_and_count);
    HU_RUN_TEST(test_proactive_decisions_repo_rejects_invalid_decision);
    HU_RUN_TEST(test_proactive_decisions_repo_rejects_null_db);
    HU_RUN_TEST(test_proactive_decisions_repo_ensure_schema_idempotent);
}
#else
void run_proactive_decisions_repo_tests(void) {
    (void)0;
} /* gate stub, per test-source-gate-symmetry */
#endif
