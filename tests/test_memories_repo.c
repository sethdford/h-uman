/* Exercises the sqlite-backed hu_memories_repo_t implementation in
 * src/memory/repos/memories_repo_sqlite.c (via hu_memories_repo_create).
 * The filename reference above satisfies scripts/check-untested.sh. */
#ifdef HU_ENABLE_SQLITE
#include "human/memory/engines.h"
#include "human/memory/memories_repo.h"
#include "test_framework.h"
#include <sqlite3.h>

static void seed(sqlite3 *db, const char *id, const char *category, const char *updated_at) {
    sqlite3_stmt *st = NULL;
    /* memories has NOT NULL key/content/created_at/updated_at + an AFTER UPDATE
     * FTS trigger; provide every required column so the trigger fires cleanly. */
    sqlite3_prepare_v2(db,
                       "INSERT INTO memories(id,key,content,category,created_at,updated_at) "
                       "VALUES(?1,?1,'c',?2,?3,?3);",
                       -1, &st, NULL);
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, category, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, updated_at, -1, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static int count_category(sqlite3 *db, const char *category) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM memories WHERE category=?1;", -1, &st, NULL) !=
        SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, category, -1, SQLITE_STATIC);
    int n = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st);
    return n;
}

/* Contract: promote_tier moves the N most-recently-updated rows of the source
 * category to the target category, leaving older rows untouched. */
static void test_memories_repo_promote_tier_moves_recent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);

    seed(db, "a", "short_term", "2026-01-01T00:00:00Z");
    seed(db, "b", "short_term", "2026-01-02T00:00:00Z");
    seed(db, "c", "short_term", "2026-01-03T00:00:00Z");
    seed(db, "d", "long_term", "2026-01-01T00:00:00Z");

    hu_memories_repo_t repo;
    HU_ASSERT_EQ(hu_memories_repo_create(&mem, &alloc, &repo), HU_OK);

    /* Promote the 2 most-recent short_term rows (b, c) to long_term. */
    HU_ASSERT_EQ(repo.vtable->promote_tier(repo.ctx, "short_term", 10, "long_term", 9, 2), HU_OK);

    HU_ASSERT_EQ(count_category(db, "long_term"), 3);  /* d + b + c */
    HU_ASSERT_EQ(count_category(db, "short_term"), 1); /* only a (oldest) remains */

    repo.vtable->deinit(repo.ctx);
    mem.vtable->deinit(mem.ctx);
}

/* max_count == 0 promotes all rows of the source category. */
static void test_memories_repo_promote_tier_zero_means_all(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);

    seed(db, "a", "short_term", "2026-01-01T00:00:00Z");
    seed(db, "b", "short_term", "2026-01-02T00:00:00Z");

    hu_memories_repo_t repo;
    HU_ASSERT_EQ(hu_memories_repo_create(&mem, &alloc, &repo), HU_OK);
    HU_ASSERT_EQ(repo.vtable->promote_tier(repo.ctx, "short_term", 10, "long_term", 9, 0), HU_OK);

    HU_ASSERT_EQ(count_category(db, "short_term"), 0);
    HU_ASSERT_EQ(count_category(db, "long_term"), 2);

    repo.vtable->deinit(repo.ctx);
    mem.vtable->deinit(mem.ctx);
}

void run_memories_repo_tests(void) {
    HU_TEST_SUITE("memories_repo");
    HU_RUN_TEST(test_memories_repo_promote_tier_moves_recent);
    HU_RUN_TEST(test_memories_repo_promote_tier_zero_means_all);
}
#else
void run_memories_repo_tests(void) {
    (void)0;
} /* gate stub, per test-source-gate-symmetry */
#endif
