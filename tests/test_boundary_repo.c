#ifdef HU_ENABLE_SQLITE
#include "human/memory/boundary_repo.h"
#include "human/memory/engines.h"
#include "test_framework.h"
#include <string.h>

static void test_boundary_repo_records_and_reads(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_boundary_repo_t repo;
    HU_ASSERT_EQ(hu_boundary_repo_create(&mem, &alloc, &repo), HU_OK);

    bool is_b = true;
    HU_ASSERT_EQ(repo.vtable->is_boundary(repo.ctx, "alice", 5, "work", 4, &is_b), HU_OK);
    HU_ASSERT_TRUE(!is_b); /* nothing recorded yet */

    hu_boundary_t b = {.contact_id = "alice",
                       .contact_id_len = 5,
                       .topic = "work",
                       .topic_len = 4,
                       .type = "hard",
                       .type_len = 4,
                       .source = "user",
                       .source_len = 4,
                       .created_at = 1};
    HU_ASSERT_EQ(repo.vtable->add(repo.ctx, &b), HU_OK);

    HU_ASSERT_EQ(repo.vtable->is_boundary(repo.ctx, "alice", 5, "work", 4, &is_b), HU_OK);
    HU_ASSERT_TRUE(is_b);
    /* unrelated topic is not a boundary */
    HU_ASSERT_EQ(repo.vtable->is_boundary(repo.ctx, "alice", 5, "weather", 7, &is_b), HU_OK);
    HU_ASSERT_TRUE(!is_b);

    repo.vtable->deinit(repo.ctx);
    mem.vtable->deinit(mem.ctx);
}

void run_boundary_repo_tests(void) {
    HU_TEST_SUITE("boundary_repo");
    HU_RUN_TEST(test_boundary_repo_records_and_reads);
}
#else
void run_boundary_repo_tests(void) {
    (void)0;
} /* gate stub, per test-source-gate-symmetry */
#endif
