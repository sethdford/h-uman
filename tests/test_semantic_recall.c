/* 5e — gate default and attach contract. No network: attach never embeds. */
#include "human/core/allocator.h"
#include "human/memory/semantic_recall.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

static void test_gate_defaults_off_and_parses(void) {
    unsetenv("HU_SEMANTIC_RECALL");
    HU_ASSERT_EQ((int)hu_semantic_recall_mode(), (int)HU_GATE_OFF);
    setenv("HU_SEMANTIC_RECALL", "shadow", 1);
    HU_ASSERT_EQ((int)hu_semantic_recall_mode(), (int)HU_GATE_SHADOW);
    setenv("HU_SEMANTIC_RECALL", "live", 1);
    HU_ASSERT_EQ((int)hu_semantic_recall_mode(), (int)HU_GATE_LIVE);
    unsetenv("HU_SEMANTIC_RECALL");
    unsetenv("HU_SEMANTIC_EMBED_URL");
    HU_ASSERT_STR_EQ(hu_semantic_recall_embed_url(), "http://127.0.0.1:8741");
    setenv("HU_SEMANTIC_EMBED_URL", "http://127.0.0.1:8749", 1);
    HU_ASSERT_STR_EQ(hu_semantic_recall_embed_url(), "http://127.0.0.1:8749");
    unsetenv("HU_SEMANTIC_EMBED_URL");
}

#ifdef HU_ENABLE_SQLITE
static void test_attach_to_sqlite_engine_creates_index_tables(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    hu_embedder_t emb = {0};
    hu_vector_store_t vs = {0};
    HU_ASSERT_EQ(hu_semantic_recall_attach(&alloc, &mem, &emb, &vs), HU_OK);
    HU_ASSERT_NOT_NULL(emb.ctx);
    HU_ASSERT_NOT_NULL(vs.ctx);
    HU_ASSERT_EQ((long)vs.vtable->count(vs.ctx), 0L);
    /* A store() now tries to embed over HTTP; the test transport is a mock, so
     * the index insert fails and is LOGGED, but the row itself must be stored. */
    HU_ASSERT_EQ(mem.vtable->store(mem.ctx, "k", 1, "hello", 5, NULL, "", 0), HU_OK);
    vs.vtable->deinit(vs.ctx, &alloc);
    emb.vtable->deinit(emb.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}
#endif

void run_semantic_recall_tests(void) {
    HU_TEST_SUITE("semantic_recall");
    HU_RUN_TEST(test_gate_defaults_off_and_parses);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_attach_to_sqlite_engine_creates_index_tables);
#endif
}
