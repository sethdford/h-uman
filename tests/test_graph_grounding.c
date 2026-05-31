#include "human/agent/graph_grounding.h"
#include "human/core/allocator.h"
#include "human/memory.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

static void seed_run(sqlite3 *db, const char *sql) {
    char *emsg = NULL;
    (void)sqlite3_exec(db, sql, NULL, NULL, &emsg);
    if (emsg)
        sqlite3_free(emsg);
}

static const char *kSeedSchema =
    "CREATE TABLE IF NOT EXISTS community_summaries ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT, contact_id TEXT NOT NULL DEFAULT '',"
    "community_id INTEGER NOT NULL, summary_text TEXT NOT NULL,"
    "entity_count INTEGER NOT NULL DEFAULT 0, edge_count INTEGER NOT NULL DEFAULT 0,"
    "generated_at INTEGER NOT NULL, schema_version INTEGER NOT NULL DEFAULT 1)";

static const char *kSeedRows = "INSERT INTO community_summaries (contact_id, community_id, "
                               "summary_text, entity_count, edge_count, generated_at) VALUES"
                               "('alice', 1, 'Climbing partner since 2019.', 9, 12, 1),"
                               "('alice', 2, 'Talks in short bursts.',        3,  4, 1),"
                               "('bob',   1, 'Should not appear.',            9,  9, 1)";

static void test_graph_ground_load_returns_contact_summaries(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_run(db, kSeedSchema);
    seed_run(db, kSeedRows);
    hu_memory_loader_t loader;
    hu_memory_loader_init(&loader, &alloc, &mem, NULL, 10, 4000);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_graph_ground_load(&loader, "alice", 5, 0, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "Climbing partner since 2019") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Talks in short bursts") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Should not appear") == NULL);
    HU_ASSERT_TRUE(strstr(out, "Climbing") < strstr(out, "short bursts"));
    alloc.free(alloc.ctx, out, out_len + 1);
    mem.vtable->deinit(mem.ctx);
}

static void test_graph_ground_load_empty_is_failopen(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    hu_memory_loader_t loader;
    hu_memory_loader_init(&loader, &alloc, &mem, NULL, 10, 4000);
    char *out = (char *)0x1;
    size_t out_len = 99;
    HU_ASSERT_EQ(hu_graph_ground_load(&loader, "nobody", 6, 0, &out, &out_len), HU_OK);
    HU_ASSERT_TRUE(out == NULL);
    HU_ASSERT_EQ((int)out_len, 0);
    mem.vtable->deinit(mem.ctx);
}
#endif

static void test_graph_grounding_mode_parse(void) {
    unsetenv("HU_GRAPH_GROUNDING");
    HU_ASSERT_EQ((int)hu_graph_grounding_mode(), (int)HU_GRAPH_GROUNDING_OFF);
    setenv("HU_GRAPH_GROUNDING", "shadow", 1);
    HU_ASSERT_EQ((int)hu_graph_grounding_mode(), (int)HU_GRAPH_GROUNDING_SHADOW);
    setenv("HU_GRAPH_GROUNDING", "on", 1);
    HU_ASSERT_EQ((int)hu_graph_grounding_mode(), (int)HU_GRAPH_GROUNDING_ON);
    setenv("HU_GRAPH_GROUNDING", "garbage", 1);
    HU_ASSERT_EQ((int)hu_graph_grounding_mode(), (int)HU_GRAPH_GROUNDING_OFF);
    unsetenv("HU_GRAPH_GROUNDING");
}

void run_graph_grounding_tests(void) {
    HU_TEST_SUITE("GraphRAG grounding");
    HU_RUN_TEST(test_graph_grounding_mode_parse);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_graph_ground_load_returns_contact_summaries);
    HU_RUN_TEST(test_graph_ground_load_empty_is_failopen);
#endif
}
