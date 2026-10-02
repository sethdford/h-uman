// @covers-none — covers src/memory/tiers.c (hu_tier_manager_*); the name
// heuristic maps "memory_tiers" to src/memory/memory.c instead.
#include "human/memory/tiers.h"
#include "test_framework.h"
#include <string.h>

static void tier_create_and_load_core(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tier_manager_t mgr;
    HU_ASSERT_EQ(hu_tier_manager_create(&alloc, NULL, &mgr), HU_OK);
    HU_ASSERT_EQ((int)mgr.core_token_budget, 500);
    HU_ASSERT_EQ((int)mgr.recall_token_budget, 2000);
    HU_ASSERT_EQ(hu_tier_manager_load_core(&mgr), HU_OK);
    hu_tier_manager_deinit(&mgr);
}

static void tier_update_core_field(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tier_manager_t mgr;
    HU_ASSERT_EQ(hu_tier_manager_create(&alloc, NULL, &mgr), HU_OK);
    HU_ASSERT_EQ(hu_tier_manager_update_core(&mgr, "user_name", 9, "Alice", 5), HU_OK);
    HU_ASSERT_STR_EQ(mgr.core.user_name, "Alice");
    hu_tier_manager_deinit(&mgr);
}

#ifdef HU_ENABLE_SQLITE
static void tier_store_to_recall(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_tier_manager_t mgr;
    HU_ASSERT_EQ(hu_tier_manager_create(&alloc, db, &mgr), HU_OK);
    HU_ASSERT_EQ(hu_tier_manager_init_tables(&mgr), HU_OK);
    HU_ASSERT_EQ(hu_tier_manager_store(&mgr, HU_TIER_RECALL, "fact1", 5, "some fact", 9), HU_OK);
    hu_tier_manager_deinit(&mgr);
    sqlite3_close(db);
}

static void tier_store_to_archival(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_tier_manager_t mgr;
    HU_ASSERT_EQ(hu_tier_manager_create(&alloc, db, &mgr), HU_OK);
    HU_ASSERT_EQ(hu_tier_manager_init_tables(&mgr), HU_OK);
    HU_ASSERT_EQ(hu_tier_manager_store(&mgr, HU_TIER_ARCHIVAL, "doc1", 4, "long doc", 8), HU_OK);
    hu_tier_manager_deinit(&mgr);
    sqlite3_close(db);
}

static void tier_promote_recall_to_core(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_tier_manager_t mgr;
    HU_ASSERT_EQ(hu_tier_manager_create(&alloc, db, &mgr), HU_OK);
    HU_ASSERT_EQ(hu_tier_manager_init_tables(&mgr), HU_OK);
    HU_ASSERT_EQ(hu_tier_manager_store(&mgr, HU_TIER_RECALL, "k1", 2, "v1", 2), HU_OK);
    HU_ASSERT_EQ(hu_tier_manager_promote(&mgr, "k1", 2, HU_TIER_RECALL, HU_TIER_CORE), HU_OK);
    hu_tier_manager_deinit(&mgr);
    sqlite3_close(db);
}

static void tier_demote_core_to_recall(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_tier_manager_t mgr;
    HU_ASSERT_EQ(hu_tier_manager_create(&alloc, db, &mgr), HU_OK);
    HU_ASSERT_EQ(hu_tier_manager_init_tables(&mgr), HU_OK);
    HU_ASSERT_EQ(hu_tier_manager_store(&mgr, HU_TIER_CORE, "k2", 2, "v2", 2), HU_OK);
    HU_ASSERT_EQ(hu_tier_manager_demote(&mgr, "k2", 2, HU_TIER_CORE, HU_TIER_RECALL), HU_OK);
    hu_tier_manager_deinit(&mgr);
    sqlite3_close(db);
}
#endif

static void tier_build_core_prompt_includes_all_fields(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tier_manager_t mgr;
    HU_ASSERT_EQ(hu_tier_manager_create(&alloc, NULL, &mgr), HU_OK);
    hu_tier_manager_update_core(&mgr, "user_name", 9, "Bob", 3);
    hu_tier_manager_update_core(&mgr, "user_bio", 8, "Engineer", 8);
    hu_tier_manager_update_core(&mgr, "user_preferences", 16, "dark mode", 9);
    hu_tier_manager_update_core(&mgr, "relationship_summary", 20, "colleague", 9);
    hu_tier_manager_update_core(&mgr, "active_goals", 12, "ship v2", 7);
    char buf[2048];
    size_t len = 0;
    HU_ASSERT_EQ(hu_tier_manager_build_core_prompt(&mgr, buf, sizeof(buf), &len), HU_OK);
    HU_ASSERT(len > 0);
    HU_ASSERT(strstr(buf, "Bob") != NULL);
    HU_ASSERT(strstr(buf, "Engineer") != NULL);
    HU_ASSERT(strstr(buf, "dark mode") != NULL);
    HU_ASSERT(strstr(buf, "colleague") != NULL);
    HU_ASSERT(strstr(buf, "ship v2") != NULL);
    hu_tier_manager_deinit(&mgr);
}

/* Every reply prompt carried a bare "[Core Memory]" header with nothing under
 * it (2026-10-02): an empty core must render nothing. */
static void tier_build_core_prompt_empty_core_renders_nothing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tier_manager_t mgr;
    HU_ASSERT_EQ(hu_tier_manager_create(&alloc, NULL, &mgr), HU_OK);
    char buf[256];
    size_t len = 99;
    HU_ASSERT_EQ(hu_tier_manager_build_core_prompt(&mgr, buf, sizeof(buf), &len), HU_OK);
    HU_ASSERT_EQ(len, (size_t)0);
    hu_tier_manager_update_core(&mgr, "user_name", 9, "Bob", 3);
    HU_ASSERT_EQ(hu_tier_manager_build_core_prompt(&mgr, buf, sizeof(buf), &len), HU_OK);
    HU_ASSERT_NOT_NULL(strstr(buf, "[Core Memory]\nName: Bob"));
    hu_tier_manager_deinit(&mgr);
}

static void tier_auto_tier_name_goes_to_core(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tier_manager_t mgr;
    HU_ASSERT_EQ(hu_tier_manager_create(&alloc, NULL, &mgr), HU_OK);
    hu_memory_tier_t assigned;
    /* auto_tier calls store, which needs db — without db it returns NOT_SUPPORTED.
       Just check the tier assignment logic by checking the assigned value. */
    hu_error_t err = hu_tier_manager_auto_tier(&mgr, "k", 1, "my name is Alice", 16, &assigned);
    /* Without SQLite db, store returns NOT_SUPPORTED, but assigned should be set */
    (void)err;
    HU_ASSERT_EQ((int)assigned, (int)HU_TIER_CORE);
    hu_tier_manager_deinit(&mgr);
}

static void tier_auto_tier_conversation_goes_to_recall(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tier_manager_t mgr;
    HU_ASSERT_EQ(hu_tier_manager_create(&alloc, NULL, &mgr), HU_OK);
    hu_memory_tier_t assigned;
    hu_tier_manager_auto_tier(&mgr, "k", 1, "we talked about the weather", 27, &assigned);
    HU_ASSERT_EQ((int)assigned, (int)HU_TIER_RECALL);
    hu_tier_manager_deinit(&mgr);
}

static void tier_auto_tier_old_content_goes_to_archival(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tier_manager_t mgr;
    HU_ASSERT_EQ(hu_tier_manager_create(&alloc, NULL, &mgr), HU_OK);
    char big[600];
    memset(big, 'x', sizeof(big));
    big[sizeof(big) - 1] = '\0';
    hu_memory_tier_t assigned;
    hu_tier_manager_auto_tier(&mgr, "k", 1, big, sizeof(big) - 1, &assigned);
    HU_ASSERT_EQ((int)assigned, (int)HU_TIER_ARCHIVAL);
    hu_tier_manager_deinit(&mgr);
}

static void tier_core_token_budget_respected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tier_manager_t mgr;
    HU_ASSERT_EQ(hu_tier_manager_create(&alloc, NULL, &mgr), HU_OK);
    HU_ASSERT_EQ((int)mgr.core_token_budget, 500);
    hu_tier_manager_deinit(&mgr);
}

static void tier_null_db_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tier_manager_t mgr;
    HU_ASSERT_EQ(hu_tier_manager_create(&alloc, NULL, &mgr), HU_OK);
    HU_ASSERT_EQ(hu_tier_manager_store(&mgr, HU_TIER_RECALL, "k", 1, "v", 1), HU_ERR_NOT_SUPPORTED);
    hu_tier_manager_deinit(&mgr);
}

static void tier_update_nonexistent_field_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tier_manager_t mgr;
    HU_ASSERT_EQ(hu_tier_manager_create(&alloc, NULL, &mgr), HU_OK);
    HU_ASSERT_EQ(hu_tier_manager_update_core(&mgr, "nonexistent", 11, "val", 3),
                 HU_ERR_INVALID_ARGUMENT);
    hu_tier_manager_deinit(&mgr);
}

static void tier_promote_already_at_tier_is_noop(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tier_manager_t mgr;
    HU_ASSERT_EQ(hu_tier_manager_create(&alloc, NULL, &mgr), HU_OK);
    HU_ASSERT_EQ(hu_tier_manager_promote(&mgr, "k", 1, HU_TIER_CORE, HU_TIER_CORE), HU_OK);
    hu_tier_manager_deinit(&mgr);
}

static void tier_manager_deinit_cleans_up(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tier_manager_t mgr;
    HU_ASSERT_EQ(hu_tier_manager_create(&alloc, NULL, &mgr), HU_OK);
    hu_tier_manager_update_core(&mgr, "user_name", 9, "Test", 4);
    hu_tier_manager_deinit(&mgr);
    HU_ASSERT_EQ((int)mgr.core.user_name[0], 0);
}

void run_memory_tiers_tests(void) {
    HU_TEST_SUITE("Memory Tiers");
    HU_RUN_TEST(tier_create_and_load_core);
    HU_RUN_TEST(tier_update_core_field);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(tier_store_to_recall);
    HU_RUN_TEST(tier_store_to_archival);
    HU_RUN_TEST(tier_promote_recall_to_core);
    HU_RUN_TEST(tier_demote_core_to_recall);
#endif
    HU_RUN_TEST(tier_build_core_prompt_includes_all_fields);
    HU_RUN_TEST(tier_build_core_prompt_empty_core_renders_nothing);
    HU_RUN_TEST(tier_auto_tier_name_goes_to_core);
    HU_RUN_TEST(tier_auto_tier_conversation_goes_to_recall);
    HU_RUN_TEST(tier_auto_tier_old_content_goes_to_archival);
    HU_RUN_TEST(tier_core_token_budget_respected);
    HU_RUN_TEST(tier_null_db_returns_error);
    HU_RUN_TEST(tier_update_nonexistent_field_returns_error);
    HU_RUN_TEST(tier_promote_already_at_tier_is_noop);
    HU_RUN_TEST(tier_manager_deinit_cleans_up);
}
