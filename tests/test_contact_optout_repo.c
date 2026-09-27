/* Exercises hu_contact_optout_repo_{ensure_schema,record,is_suppressed,clear,
 * count} in src/memory/repos/contact_optout_repo_sqlite.c (October roadmap O5).
 * @covers-none — same basename-stripping collision as
 * test_proactive_decisions_repo.c ("contact_optout_repo" → "contact_optout"
 * → src/daemon/daemon_contact_optout.c); the production file is named above.
 */
#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include "human/memory/contact_optout_repo.h"
#include "test_framework.h"
#include <sqlite3.h>
#include <string.h>

static void test_contact_optout_repo_record_is_suppressed_count(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    int64_t count = -1;
    bool s = true;
    /* Pre: nothing suppressed, table may not even exist yet. */
    HU_ASSERT_EQ(hu_contact_optout_repo_count(db, &count), HU_OK);
    HU_ASSERT_EQ(count, 0);
    HU_ASSERT_EQ(hu_contact_optout_repo_is_suppressed(db, "+15551230000", &s), HU_OK);
    HU_ASSERT_TRUE(!s);

    HU_ASSERT_EQ(hu_contact_optout_repo_record(db, 1000, "+15551230000", "inbound_opt_out_phrase",
                                               "stop texting me"),
                 HU_OK);
    HU_ASSERT_EQ(hu_contact_optout_repo_is_suppressed(db, "+15551230000", &s), HU_OK);
    HU_ASSERT_TRUE(s);
    HU_ASSERT_EQ(hu_contact_optout_repo_is_suppressed(db, "+15559990000", &s), HU_OK);
    HU_ASSERT_TRUE(!s);

    /* Upsert: a second request from the same contact is one row, not two. */
    HU_ASSERT_EQ(hu_contact_optout_repo_record(db, 2000, "+15551230000", NULL, NULL), HU_OK);
    HU_ASSERT_EQ(hu_contact_optout_repo_count(db, &count), HU_OK);
    HU_ASSERT_EQ(count, 1);

    HU_ASSERT_EQ(hu_contact_optout_repo_record(db, 3000, "+15559990000", NULL, NULL), HU_OK);
    HU_ASSERT_EQ(hu_contact_optout_repo_count(db, &count), HU_OK);
    HU_ASSERT_EQ(count, 2);

    /* Clear reverses it; clearing an absent contact is not an error. */
    HU_ASSERT_EQ(hu_contact_optout_repo_clear(db, "+15551230000"), HU_OK);
    HU_ASSERT_EQ(hu_contact_optout_repo_is_suppressed(db, "+15551230000", &s), HU_OK);
    HU_ASSERT_TRUE(!s);
    HU_ASSERT_EQ(hu_contact_optout_repo_clear(db, "+10000000000"), HU_OK);
    HU_ASSERT_EQ(hu_contact_optout_repo_count(db, &count), HU_OK);
    HU_ASSERT_EQ(count, 1);
    mem.vtable->deinit(mem.ctx);
}

static void test_contact_optout_repo_rejects_bad_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    bool s = false;
    int64_t n = 0;
    HU_ASSERT_EQ(hu_contact_optout_repo_record(db, 1, "", NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_contact_optout_repo_record(db, 1, NULL, NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_contact_optout_repo_record(NULL, 1, "+1", NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_contact_optout_repo_is_suppressed(db, "", &s), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_contact_optout_repo_is_suppressed(db, "+1", NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_contact_optout_repo_count(NULL, &n), HU_ERR_INVALID_ARGUMENT);
    mem.vtable->deinit(mem.ctx);
}

void run_contact_optout_repo_tests(void) {
    HU_RUN_TEST(test_contact_optout_repo_record_is_suppressed_count);
    HU_RUN_TEST(test_contact_optout_repo_rejects_bad_args);
}
#else
void run_contact_optout_repo_tests(void) {}
#endif
