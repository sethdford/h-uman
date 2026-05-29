/* Exercises the sqlite-backed hu_feed_items_repo_t implementation in
 * src/memory/repos/feed_items_repo_sqlite.c (via hu_feed_items_repo_create).
 * The filename reference above satisfies scripts/check-untested.sh. */
#ifdef HU_ENABLE_SQLITE
#include "human/memory/engines.h"
#include "human/memory/feed_items_repo.h"
#include "test_framework.h"
#include <sqlite3.h>

/* Count feed_items rows for a given source — direct query (test-only) so we
 * verify the repo actually persisted, not just that record() returned HU_OK. */
static int count_for_source(sqlite3 *db, const char *source) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM feed_items WHERE source=?1;", -1, &st, NULL) !=
        SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, source, -1, SQLITE_STATIC);
    int n = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st);
    return n;
}

static void test_feed_items_repo_records_a_row(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_feed_items_repo_t repo;
    HU_ASSERT_EQ(hu_feed_items_repo_create(&mem, &alloc, &repo), HU_OK);

    hu_feed_item_t item = {.source = "/inbox/note.txt",
                           .source_len = 15,
                           .content = "hello world",
                           .content_len = 11,
                           .ingested_at = 1717000000};
    HU_ASSERT_EQ(repo.vtable->record(repo.ctx, &item), HU_OK);
    HU_ASSERT_EQ(count_for_source(hu_sqlite_memory_get_db(&mem), "/inbox/note.txt"), 1);

    repo.vtable->deinit(repo.ctx);
    mem.vtable->deinit(mem.ctx);
}

/* Contract: record() is idempotent — same (source, content-prefix) twice must
 * succeed both times and leave exactly one row (INSERT OR IGNORE + the dedup
 * unique index), matching the prior inline behavior. */
static void test_feed_items_repo_record_is_idempotent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_feed_items_repo_t repo;
    HU_ASSERT_EQ(hu_feed_items_repo_create(&mem, &alloc, &repo), HU_OK);

    hu_feed_item_t item = {.source = "/inbox/dup.txt",
                           .source_len = 14,
                           .content = "same content",
                           .content_len = 12,
                           .ingested_at = 1717000001};
    HU_ASSERT_EQ(repo.vtable->record(repo.ctx, &item), HU_OK);
    HU_ASSERT_EQ(repo.vtable->record(repo.ctx, &item), HU_OK); /* second must not error */
    HU_ASSERT_EQ(count_for_source(hu_sqlite_memory_get_db(&mem), "/inbox/dup.txt"), 1);

    repo.vtable->deinit(repo.ctx);
    mem.vtable->deinit(mem.ctx);
}

/* NULL / empty source is rejected, not persisted. */
static void test_feed_items_repo_rejects_empty_source(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_feed_items_repo_t repo;
    HU_ASSERT_EQ(hu_feed_items_repo_create(&mem, &alloc, &repo), HU_OK);

    hu_feed_item_t bad = {.source = NULL, .source_len = 0, .content = "x", .content_len = 1};
    HU_ASSERT_EQ(repo.vtable->record(repo.ctx, &bad), HU_ERR_INVALID_ARGUMENT);

    repo.vtable->deinit(repo.ctx);
    mem.vtable->deinit(mem.ctx);
}

void run_feed_items_repo_tests(void) {
    HU_TEST_SUITE("feed_items_repo");
    HU_RUN_TEST(test_feed_items_repo_records_a_row);
    HU_RUN_TEST(test_feed_items_repo_record_is_idempotent);
    HU_RUN_TEST(test_feed_items_repo_rejects_empty_source);
}
#else
void run_feed_items_repo_tests(void) {
    (void)0;
} /* gate stub, per test-source-gate-symmetry */
#endif
