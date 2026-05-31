/* Exercises the sqlite-backed hu_social_graph_repo_t implementation in
 * src/memory/repos/social_graph_repo_sqlite.c (via hu_social_graph_repo_create).
 * The filename reference above satisfies scripts/check-untested.sh, whose
 * basename matcher can't otherwise tie this test to that impl file (the exported
 * symbol is named for the abstraction, not the _sqlite impl).
 * DDD Phase 3 (one aggregate of ~21). */
#ifdef HU_ENABLE_SQLITE
#include "human/context/social_graph.h" /* hu_social_graph_free */
#include "human/core/allocator.h"
#include "human/memory/engines.h"
#include "human/memory/social_graph_repo.h"
#include "human/persona.h"
#include "test_framework.h"
#include <string.h>

static void test_social_graph_repo_upsert_and_get(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_social_graph_repo_t repo;
    HU_ASSERT_EQ(hu_social_graph_repo_create(&mem, &alloc, &repo), HU_OK);

    /* nothing yet */
    hu_relationship_t *rels = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(repo.vtable->get(repo.ctx, &alloc, "alice", 5, &rels, &n), HU_OK);
    HU_ASSERT_EQ(n, 0u);

    HU_ASSERT_EQ(repo.vtable->upsert(repo.ctx, "alice", 5, "Bob", "brother", 0, "lives in Ohio"),
                 HU_OK);
    HU_ASSERT_EQ(repo.vtable->get(repo.ctx, &alloc, "alice", 5, &rels, &n), HU_OK);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_NOT_NULL(rels);
    HU_ASSERT_TRUE(strcmp(rels[0].name, "Bob") == 0);
    HU_ASSERT_TRUE(strcmp(rels[0].role, "brother") == 0);
    HU_ASSERT_TRUE(strcmp(rels[0].notes, "lives in Ohio") == 0);
    hu_social_graph_free(&alloc, rels, n);

    repo.vtable->deinit(repo.ctx);
    mem.vtable->deinit(mem.ctx);
}

static void test_social_graph_repo_upsert_conflict_updates(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_social_graph_repo_t repo;
    HU_ASSERT_EQ(hu_social_graph_repo_create(&mem, &alloc, &repo), HU_OK);

    /* same (contact_id, person_name) twice -> ON CONFLICT updates role/notes */
    HU_ASSERT_EQ(repo.vtable->upsert(repo.ctx, "alice", 5, "Bob", "friend", 0, "old"), HU_OK);
    HU_ASSERT_EQ(repo.vtable->upsert(repo.ctx, "alice", 5, "Bob", "brother", 0, "new"), HU_OK);

    hu_relationship_t *rels = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(repo.vtable->get(repo.ctx, &alloc, "alice", 5, &rels, &n), HU_OK);
    HU_ASSERT_EQ(n, 1u); /* still one row — updated, not inserted */
    HU_ASSERT_TRUE(strcmp(rels[0].role, "brother") == 0);
    HU_ASSERT_TRUE(strcmp(rels[0].notes, "new") == 0);
    hu_social_graph_free(&alloc, rels, n);

    repo.vtable->deinit(repo.ctx);
    mem.vtable->deinit(mem.ctx);
}

static void test_social_graph_repo_multiple_and_scoped(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_social_graph_repo_t repo;
    HU_ASSERT_EQ(hu_social_graph_repo_create(&mem, &alloc, &repo), HU_OK);

    HU_ASSERT_EQ(repo.vtable->upsert(repo.ctx, "alice", 5, "Bob", "brother", 0, ""), HU_OK);
    HU_ASSERT_EQ(repo.vtable->upsert(repo.ctx, "alice", 5, "Carol", "mom", 0, ""), HU_OK);
    HU_ASSERT_EQ(repo.vtable->upsert(repo.ctx, "dave", 4, "Eve", "coworker", 0, ""), HU_OK);

    hu_relationship_t *rels = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(repo.vtable->get(repo.ctx, &alloc, "alice", 5, &rels, &n), HU_OK);
    HU_ASSERT_EQ(n, 2u); /* scoped to alice */
    hu_social_graph_free(&alloc, rels, n);

    HU_ASSERT_EQ(repo.vtable->get(repo.ctx, &alloc, "dave", 4, &rels, &n), HU_OK);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_TRUE(strcmp(rels[0].name, "Eve") == 0);
    hu_social_graph_free(&alloc, rels, n);

    repo.vtable->deinit(repo.ctx);
    mem.vtable->deinit(mem.ctx);
}

void run_social_graph_repo_tests(void) {
    HU_TEST_SUITE("social_graph_repo");
    HU_RUN_TEST(test_social_graph_repo_upsert_and_get);
    HU_RUN_TEST(test_social_graph_repo_upsert_conflict_updates);
    HU_RUN_TEST(test_social_graph_repo_multiple_and_scoped);
}
#else
void run_social_graph_repo_tests(void) {
    (void)0;
} /* gate stub, per test-source-gate-symmetry */
#endif
