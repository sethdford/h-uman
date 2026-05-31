/* Exercises the sqlite-backed hu_self_awareness_repo_t implementation in
 * src/memory/repos/self_awareness_repo_sqlite.c (via hu_self_awareness_repo_create).
 * The filename reference above satisfies scripts/check-untested.sh, whose
 * basename matcher can't otherwise tie this test to that impl file (the exported
 * symbol is named for the abstraction, not the _sqlite impl).
 * DDD Phase 3 (self_awareness aggregate — 2 tables). */
#ifdef HU_ENABLE_SQLITE
#include "human/core/allocator.h"
#include "human/memory/engines.h"
#include "human/memory/self_awareness_repo.h"
#include "test_framework.h"
#include <string.h>

static void test_self_awareness_repo_stats_roundtrip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_self_awareness_repo_t repo;
    HU_ASSERT_EQ(hu_self_awareness_repo_create(&mem, &alloc, &repo), HU_OK);

    /* no row yet */
    bool found = true;
    hu_self_awareness_stats_row_t row;
    HU_ASSERT_EQ(repo.vtable->stats_get(repo.ctx, "alice", 5, &found, &row), HU_OK);
    HU_ASSERT_TRUE(!found);

    /* record a send we initiated, on topic "work" */
    HU_ASSERT_EQ(repo.vtable->stats_record_send(repo.ctx, "alice", 5, 1, "work", 4, 1000), HU_OK);
    HU_ASSERT_EQ(repo.vtable->stats_get(repo.ctx, "alice", 5, &found, &row), HU_OK);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_EQ(row.messages_sent_week, 1u);
    HU_ASSERT_EQ(row.initiations_week, 1u);
    HU_ASSERT_EQ(row.topic_repeat_count, 1u);
    HU_ASSERT_TRUE(row.last_topic_len == 4 && strcmp(row.last_topic, "work") == 0);

    repo.vtable->deinit(repo.ctx);
    mem.vtable->deinit(mem.ctx);
}

static void test_self_awareness_repo_record_send_upsert_increments(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_self_awareness_repo_t repo;
    HU_ASSERT_EQ(hu_self_awareness_repo_create(&mem, &alloc, &repo), HU_OK);

    /* two sends on the same topic -> messages_sent=2, topic_repeat increments */
    HU_ASSERT_EQ(repo.vtable->stats_record_send(repo.ctx, "alice", 5, 1, "work", 4, 1000), HU_OK);
    HU_ASSERT_EQ(repo.vtable->stats_record_send(repo.ctx, "alice", 5, 0, "work", 4, 2000), HU_OK);

    bool found = false;
    hu_self_awareness_stats_row_t row;
    HU_ASSERT_EQ(repo.vtable->stats_get(repo.ctx, "alice", 5, &found, &row), HU_OK);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_EQ(row.messages_sent_week, 2u);
    HU_ASSERT_EQ(row.initiations_week, 1u);   /* 1 + 0 */
    HU_ASSERT_EQ(row.topic_repeat_count, 2u); /* same topic -> +1 */

    repo.vtable->deinit(repo.ctx);
    mem.vtable->deinit(mem.ctx);
}

static void test_self_awareness_repo_reciprocity_roundtrip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_self_awareness_repo_t repo;
    HU_ASSERT_EQ(hu_self_awareness_repo_create(&mem, &alloc, &repo), HU_OK);

    /* missing metric -> 0.0 */
    HU_ASSERT_TRUE(repo.vtable->reciprocity_get(repo.ctx, "alice", 5, "initiation_ratio") == 0.0);

    HU_ASSERT_EQ(repo.vtable->reciprocity_set(repo.ctx, "alice", 5, "initiation_ratio", 0.75, 1000),
                 HU_OK);
    double v = repo.vtable->reciprocity_get(repo.ctx, "alice", 5, "initiation_ratio");
    HU_ASSERT_TRUE(v > 0.74 && v < 0.76);

    /* INSERT OR REPLACE overwrites */
    HU_ASSERT_EQ(repo.vtable->reciprocity_set(repo.ctx, "alice", 5, "initiation_ratio", 0.25, 2000),
                 HU_OK);
    v = repo.vtable->reciprocity_get(repo.ctx, "alice", 5, "initiation_ratio");
    HU_ASSERT_TRUE(v > 0.24 && v < 0.26);

    repo.vtable->deinit(repo.ctx);
    mem.vtable->deinit(mem.ctx);
}

void run_self_awareness_repo_tests(void) {
    HU_TEST_SUITE("self_awareness_repo");
    HU_RUN_TEST(test_self_awareness_repo_stats_roundtrip);
    HU_RUN_TEST(test_self_awareness_repo_record_send_upsert_increments);
    HU_RUN_TEST(test_self_awareness_repo_reciprocity_roundtrip);
}
#else
void run_self_awareness_repo_tests(void) {
    (void)0;
} /* gate stub, per test-source-gate-symmetry */
#endif
