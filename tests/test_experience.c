#include "human/experience.h"
#include "human/memory/vector.h"
#include "test_framework.h"
#ifdef HU_ENABLE_SQLITE
#include "human/intelligence/distiller.h"
#include "human/memory.h"
#include <sqlite3.h>
#endif
#include <string.h>

static void test_experience_init_deinit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_experience_store_t store;
    HU_ASSERT_EQ(hu_experience_store_init(&alloc, NULL, &store), HU_OK);
    HU_ASSERT(store.alloc != NULL);
    HU_ASSERT_EQ(store.stored_count, (size_t)0);
    hu_experience_store_deinit(&store);
    HU_ASSERT(store.alloc == NULL);
}

static void test_experience_record(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_experience_store_t store;
    HU_ASSERT_EQ(hu_experience_store_init(&alloc, NULL, &store), HU_OK);
    HU_ASSERT_EQ(
        hu_experience_record(&store, "send email", 10, "used gmail", 10, "delivered", 9, 0.95),
        HU_OK);
    HU_ASSERT_EQ(store.stored_count, (size_t)1);
    HU_ASSERT_EQ(hu_experience_record(&store, "schedule", 8, "opened cal", 10, "created", 7, 0.88),
                 HU_OK);
    HU_ASSERT_EQ(store.stored_count, (size_t)2);
    hu_experience_store_deinit(&store);
}

static void test_experience_recall(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_experience_store_t store;
    hu_experience_store_init(&alloc, NULL, &store);
    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_experience_recall_similar(&store, "task", 4, &ctx, &ctx_len), HU_OK);
    HU_ASSERT(ctx == NULL);
    hu_experience_record(&store, "task", 4, "action", 6, "result", 6, 0.9);
    HU_ASSERT_EQ(hu_experience_recall_similar(&store, "task", 4, &ctx, &ctx_len), HU_OK);
    HU_ASSERT(ctx != NULL);
    HU_ASSERT(ctx_len > 0);
    alloc.free(alloc.ctx, ctx, ctx_len + 1);
    hu_experience_store_deinit(&store);
}

static void test_experience_build_prompt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_experience_store_t store;
    hu_experience_store_init(&alloc, NULL, &store);
    char *prompt = NULL;
    size_t plen = 0;
    HU_ASSERT_EQ(hu_experience_build_prompt(&store, "current task", 12, &prompt, &plen), HU_OK);
    HU_ASSERT(prompt != NULL);
    HU_ASSERT(plen == 0);
    alloc.free(alloc.ctx, prompt, 1);
    hu_experience_record(&store, "current task", 12, "action", 6, "result", 6, 0.9);
    HU_ASSERT_EQ(hu_experience_build_prompt(&store, "current task", 12, &prompt, &plen), HU_OK);
    HU_ASSERT(prompt != NULL);
    HU_ASSERT(plen > 0);
    alloc.free(alloc.ctx, prompt, plen + 1);
    hu_experience_store_deinit(&store);
}

static void test_experience_null_args(void) {
    hu_experience_store_t store;
    HU_ASSERT_EQ(hu_experience_store_init(NULL, NULL, &store), HU_ERR_INVALID_ARGUMENT);
}

#ifdef HU_ENABLE_SQLITE
static void test_experience_memory_store_recall(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_experience_store_t store;
    HU_ASSERT_EQ(hu_experience_store_init(&alloc, &mem, &store), HU_OK);
    HU_ASSERT_EQ(
        hu_experience_record(&store, "send email", 10, "used gmail", 10, "delivered", 9, 0.95),
        HU_OK);
    HU_ASSERT_EQ(store.stored_count, (size_t)1);
    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_experience_recall_similar(&store, "send email", 10, &ctx, &ctx_len), HU_OK);
    HU_ASSERT(ctx != NULL);
    HU_ASSERT(ctx_len > 0);
    HU_ASSERT_TRUE(ctx_len >= 40);
    alloc.free(alloc.ctx, ctx, ctx_len + 1);
    hu_experience_store_deinit(&store);
    mem.vtable->deinit(mem.ctx);
}

/* 2026-10-02: the research agent recorded its whole 7,680-char task prompt
 * (persona, beats, raw Twitter feed) as one "experience:" row, and recall put
 * it into personal iMessage replies. The in-memory path always capped each
 * field at HU_EXP_TEXT_MAX; the persisted path did not. */
static void test_experience_persisted_fields_are_capped(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_experience_store_t store;
    HU_ASSERT_EQ(hu_experience_store_init(&alloc, &mem, &store), HU_OK);
    static char task[8000];
    memset(task, 'x', sizeof(task));
    memcpy(task, "You are the h-uman Research Agent", 33);
    HU_ASSERT_EQ(hu_experience_record(&store, task, sizeof(task), "read feed", 9, "done", 4, 0.5),
                 HU_OK);
    hu_memory_entry_t e;
    memset(&e, 0, sizeof(e));
    bool found = false;
    const char key[] = "experience:You are the h-um";
    HU_ASSERT_EQ(mem.vtable->get(mem.ctx, &alloc, key, strlen(key), &e, &found), HU_OK);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_TRUE(e.content_len <= 3 * 512 + 64);
    HU_ASSERT_NOT_NULL(strstr(e.content, "Outcome: done")); /* the tail survives the cap */
    hu_memory_entry_free_fields(&alloc, &e);
    hu_experience_store_deinit(&store);
    mem.vtable->deinit(mem.ctx);
}
#endif

static void test_experience_record_recall_verifies_content(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_experience_store_t store;
    hu_experience_store_init(&alloc, NULL, &store);

    hu_experience_record(&store, "debug segfault", 14, "used gdb backtrace", 18,
                         "found null pointer", 18, 0.95);
    hu_experience_record(&store, "optimize query", 14, "added index", 11, "50x speedup", 11, 0.99);

    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_experience_recall_similar(&store, "debug crash", 11, &ctx, &ctx_len), HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(ctx_len > 0);
    HU_ASSERT_TRUE(strstr(ctx, "debug") != NULL || strstr(ctx, "segfault") != NULL ||
                   strstr(ctx, "gdb") != NULL || strstr(ctx, "null pointer") != NULL);
    alloc.free(alloc.ctx, ctx, ctx_len + 1);

    ctx = NULL;
    ctx_len = 0;
    HU_ASSERT_EQ(hu_experience_recall_similar(&store, "optimize database", 17, &ctx, &ctx_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(ctx_len > 0);
    HU_ASSERT_TRUE(strstr(ctx, "optimize") != NULL || strstr(ctx, "index") != NULL ||
                   strstr(ctx, "speedup") != NULL);
    alloc.free(alloc.ctx, ctx, ctx_len + 1);

    hu_experience_store_deinit(&store);
}

static void test_experience_init_semantic_null_embedder(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_experience_store_t store;
    HU_ASSERT_EQ(hu_experience_store_init_semantic(&alloc, NULL, NULL, NULL, &store), HU_OK);
    HU_ASSERT_NULL(store.embedder);
    HU_ASSERT_NULL(store.vec_store);
    hu_experience_record(&store, "test task", 9, "act", 3, "out", 3, 0.8);
    HU_ASSERT_EQ(store.stored_count, (size_t)1);
    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_experience_recall_similar(&store, "test task", 9, &ctx, &ctx_len), HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(ctx_len > 0);
    alloc.free(alloc.ctx, ctx, ctx_len + 1);
    hu_experience_store_deinit(&store);
}

static void test_experience_semantic_record_recall(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_embedder_t embedder = hu_embedder_local_create(&alloc);
    hu_vector_store_t vec = hu_vector_store_mem_create(&alloc);
    hu_experience_store_t store;
    HU_ASSERT_EQ(hu_experience_store_init_semantic(&alloc, NULL, &embedder, &vec, &store), HU_OK);
    HU_ASSERT_NOT_NULL(store.embedder);
    HU_ASSERT_NOT_NULL(store.vec_store);

    hu_experience_record(&store, "deploy kubernetes pods", 22, "kubectl apply", 13, "pods running",
                         12, 0.95);
    hu_experience_record(&store, "debug memory leak", 17, "used valgrind", 13, "found leak", 10,
                         0.9);
    hu_experience_record(&store, "optimize database queries", 25, "added indexes", 13,
                         "faster queries", 14, 0.88);

    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_experience_recall_similar(&store, "kubernetes deployment", 20, &ctx, &ctx_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(ctx_len > 0);
    alloc.free(alloc.ctx, ctx, ctx_len + 1);

    hu_experience_store_deinit(&store);
    if (embedder.vtable && embedder.vtable->deinit)
        embedder.vtable->deinit(embedder.ctx, &alloc);
    if (vec.vtable && vec.vtable->deinit)
        vec.vtable->deinit(vec.ctx, &alloc);
}

static void test_experience_semantic_deinit_cleanup(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_embedder_t embedder = hu_embedder_local_create(&alloc);
    hu_vector_store_t vec = hu_vector_store_mem_create(&alloc);
    hu_experience_store_t store;
    hu_experience_store_init_semantic(&alloc, NULL, &embedder, &vec, &store);
    hu_experience_record(&store, "task", 4, "act", 3, "out", 3, 1.0);
    hu_experience_store_deinit(&store);
    HU_ASSERT_NULL(store.alloc);
    HU_ASSERT_NULL(store.embedder);
    HU_ASSERT_NULL(store.vec_store);
    if (embedder.vtable && embedder.vtable->deinit)
        embedder.vtable->deinit(embedder.ctx, &alloc);
    if (vec.vtable && vec.vtable->deinit)
        vec.vtable->deinit(vec.ctx, &alloc);
}

#ifdef HU_ENABLE_SQLITE
static void test_experience_explog_persistence(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    HU_ASSERT_NOT_NULL(db);

    hu_distiller_init_tables(db);

    hu_experience_store_t store;
    hu_experience_store_init(&alloc, NULL, &store);
    store.db = db;

    hu_experience_record(&store, "network debug", 13, "tcpdump", 7, "fixed", 5, 0.9);
    hu_experience_record(&store, "api integration", 15, "curl tests", 10, "working", 7, 0.95);

    const char *sql = "SELECT COUNT(*) FROM experience_log";
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    HU_ASSERT_EQ(count, 2);

    hu_experience_store_deinit(&store);
    sqlite3_close(db);
}
#endif

void run_experience_tests(void) {
    HU_TEST_SUITE("Experience Store");
    HU_RUN_TEST(test_experience_init_deinit);
    HU_RUN_TEST(test_experience_record);
    HU_RUN_TEST(test_experience_recall);
    HU_RUN_TEST(test_experience_build_prompt);
    HU_RUN_TEST(test_experience_null_args);
    HU_RUN_TEST(test_experience_record_recall_verifies_content);
    HU_RUN_TEST(test_experience_init_semantic_null_embedder);
    HU_RUN_TEST(test_experience_semantic_record_recall);
    HU_RUN_TEST(test_experience_semantic_deinit_cleanup);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_experience_memory_store_recall);
    HU_RUN_TEST(test_experience_persisted_fields_are_capped);
    HU_RUN_TEST(test_experience_explog_persistence);
#endif
}
