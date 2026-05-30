/* Exercises the sqlite-backed hu_opinions_repo_t implementation in
 * src/memory/repos/opinions_repo_sqlite.c (via hu_opinions_repo_create).
 * The filename reference above satisfies scripts/check-untested.sh, whose
 * basename matcher can't otherwise tie this test to that impl file (the
 * exported symbol is named for the abstraction, not the _sqlite impl).
 * DDD Phase 3 (one aggregate of ~24). */
#ifdef HU_ENABLE_SQLITE
#include "human/core/string.h"
#include "human/memory/engines.h"
#include "human/memory/opinions.h"
#include "human/memory/opinions_repo.h"
#include "test_framework.h"
#include <string.h>

static void test_opinions_repo_insert_and_query_active(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_opinions_repo_t repo;
    HU_ASSERT_EQ(hu_opinions_repo_create(&mem, &alloc, &repo), HU_OK);

    int64_t id = 0;
    HU_ASSERT_EQ(repo.vtable->insert(repo.ctx, "coffee", 6, "good", 4, 0.8f, 100, 100, &id), HU_OK);
    HU_ASSERT_TRUE(id > 0);

    hu_opinion_t *ops = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(repo.vtable->query(repo.ctx, &alloc, "coffee", 6, false, &ops, &n), HU_OK);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_NOT_NULL(ops);
    HU_ASSERT_EQ(ops[0].id, id);
    HU_ASSERT_TRUE(ops[0].position_len == 4 && memcmp(ops[0].position, "good", 4) == 0);
    HU_ASSERT_TRUE(ops[0].confidence > 0.79f && ops[0].confidence < 0.81f);
    HU_ASSERT_EQ(ops[0].superseded_by, 0); /* active */
    hu_opinions_free(&alloc, ops, n);

    repo.vtable->deinit(repo.ctx);
    mem.vtable->deinit(mem.ctx);
}

static void test_opinions_repo_find_active(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_opinions_repo_t repo;
    HU_ASSERT_EQ(hu_opinions_repo_create(&mem, &alloc, &repo), HU_OK);

    /* nothing yet */
    bool found = true;
    char *pos = NULL;
    size_t pos_len = 0;
    int64_t id = -1;
    HU_ASSERT_EQ(repo.vtable->find_active(repo.ctx, &alloc, "tea", 3, &found, &id, &pos, &pos_len),
                 HU_OK);
    HU_ASSERT_TRUE(!found);
    HU_ASSERT_TRUE(pos == NULL);

    int64_t ins_id = 0;
    HU_ASSERT_EQ(repo.vtable->insert(repo.ctx, "tea", 3, "lovely", 6, 0.5f, 1, 1, &ins_id), HU_OK);
    HU_ASSERT_EQ(repo.vtable->find_active(repo.ctx, &alloc, "tea", 3, &found, &id, &pos, &pos_len),
                 HU_OK);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_EQ(id, ins_id);
    HU_ASSERT_TRUE(pos_len == 6 && pos && memcmp(pos, "lovely", 6) == 0);
    hu_str_free(&alloc, pos);

    repo.vtable->deinit(repo.ctx);
    mem.vtable->deinit(mem.ctx);
}

static void test_opinions_repo_update_last_expressed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_opinions_repo_t repo;
    HU_ASSERT_EQ(hu_opinions_repo_create(&mem, &alloc, &repo), HU_OK);

    int64_t id = 0;
    HU_ASSERT_EQ(repo.vtable->insert(repo.ctx, "dogs", 4, "great", 5, 0.5f, 10, 10, &id), HU_OK);
    HU_ASSERT_EQ(repo.vtable->update_last_expressed(repo.ctx, id, 999, 0.95f), HU_OK);

    hu_opinion_t *ops = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(repo.vtable->query(repo.ctx, &alloc, "dogs", 4, false, &ops, &n), HU_OK);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_EQ(ops[0].last_expressed, 999);
    HU_ASSERT_TRUE(ops[0].confidence > 0.94f && ops[0].confidence < 0.96f);
    hu_opinions_free(&alloc, ops, n);

    repo.vtable->deinit(repo.ctx);
    mem.vtable->deinit(mem.ctx);
}

static void test_opinions_repo_supersede(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_opinions_repo_t repo;
    HU_ASSERT_EQ(hu_opinions_repo_create(&mem, &alloc, &repo), HU_OK);

    int64_t old_id = 0;
    HU_ASSERT_EQ(repo.vtable->insert(repo.ctx, "tabs", 4, "spaces", 6, 0.6f, 1, 1, &old_id), HU_OK);

    int64_t new_id = 0;
    HU_ASSERT_EQ(
        repo.vtable->insert_superseding(repo.ctx, "tabs", 4, "tabs", 4, 0.9f, 50, old_id, &new_id),
        HU_OK);
    HU_ASSERT_TRUE(new_id > 0 && new_id != old_id);

    /* active query returns only the new opinion */
    hu_opinion_t *act = NULL;
    size_t na = 0;
    HU_ASSERT_EQ(repo.vtable->query(repo.ctx, &alloc, "tabs", 4, false, &act, &na), HU_OK);
    HU_ASSERT_EQ(na, 1u);
    HU_ASSERT_EQ(act[0].id, new_id);
    HU_ASSERT_TRUE(act[0].position_len == 4 && memcmp(act[0].position, "tabs", 4) == 0);
    hu_opinions_free(&alloc, act, na);

    /* superseded query returns the old opinion, pointing at the new one */
    hu_opinion_t *sup = NULL;
    size_t ns = 0;
    HU_ASSERT_EQ(repo.vtable->query(repo.ctx, &alloc, "tabs", 4, true, &sup, &ns), HU_OK);
    HU_ASSERT_EQ(ns, 1u);
    HU_ASSERT_EQ(sup[0].id, old_id);
    HU_ASSERT_EQ(sup[0].superseded_by, new_id);
    hu_opinions_free(&alloc, sup, ns);

    repo.vtable->deinit(repo.ctx);
    mem.vtable->deinit(mem.ctx);
}

void run_opinions_repo_tests(void) {
    HU_TEST_SUITE("opinions_repo");
    HU_RUN_TEST(test_opinions_repo_insert_and_query_active);
    HU_RUN_TEST(test_opinions_repo_find_active);
    HU_RUN_TEST(test_opinions_repo_update_last_expressed);
    HU_RUN_TEST(test_opinions_repo_supersede);
}
#else
void run_opinions_repo_tests(void) {
    (void)0;
} /* gate stub, per test-source-gate-symmetry */
#endif
