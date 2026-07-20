/*
 * Wave B: contact/session isolation for keyword + hybrid retrieval.
 * Fail-closed: require_contact_namespace without contact_id → error;
 * cross-contact keys must never appear in another contact's results.
 */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/memory/retrieval.h"
#include "human/memory/vector.h"
#include "test_framework.h"
#include <string.h>

#ifdef HU_ENABLE_SQLITE

static void test_require_contact_namespace_rejects_missing_id(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);

    hu_retrieval_options_t opts = {0};
    opts.mode = HU_RETRIEVAL_KEYWORD;
    opts.limit = 10;
    opts.require_contact_namespace = true;

    hu_retrieval_result_t out = {0};
    hu_error_t err = hu_keyword_retrieve(&alloc, &mem, "coffee", 6, &opts, &out);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(out.count, 0u);

    mem.vtable->deinit(mem.ctx);
}

static void test_keyword_cross_contact_isolation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);

    HU_ASSERT_EQ(hu_memory_store_for_contact(&mem, "alice", 5, "pref", 4, "alice likes coffee", 18,
                                             NULL, "", 0),
                 HU_OK);
    HU_ASSERT_EQ(
        hu_memory_store_for_contact(&mem, "bob", 3, "pref", 4, "bob likes coffee", 16, NULL, "", 0),
        HU_OK);

    hu_retrieval_options_t opts = {0};
    opts.mode = HU_RETRIEVAL_KEYWORD;
    opts.limit = 10;
    opts.contact_id = "alice";
    opts.contact_id_len = 5;
    opts.require_contact_namespace = true;

    hu_retrieval_result_t out = {0};
    HU_ASSERT_EQ(hu_keyword_retrieve(&alloc, &mem, "coffee", 6, &opts, &out), HU_OK);
    HU_ASSERT_TRUE(out.count >= 1);
    for (size_t i = 0; i < out.count; i++) {
        HU_ASSERT_TRUE(hu_retrieval_entry_in_contact_scope(&out.entries[i], "alice", 5));
        HU_ASSERT_TRUE(out.entries[i].content == NULL ||
                       strstr(out.entries[i].content, "bob") == NULL);
    }
    hu_retrieval_result_free(&alloc, &out);

    opts.contact_id = "bob";
    opts.contact_id_len = 3;
    HU_ASSERT_EQ(hu_keyword_retrieve(&alloc, &mem, "coffee", 6, &opts, &out), HU_OK);
    HU_ASSERT_TRUE(out.count >= 1);
    for (size_t i = 0; i < out.count; i++) {
        HU_ASSERT_TRUE(hu_retrieval_entry_in_contact_scope(&out.entries[i], "bob", 3));
        HU_ASSERT_TRUE(out.entries[i].content == NULL ||
                       strstr(out.entries[i].content, "alice") == NULL);
    }
    hu_retrieval_result_free(&alloc, &out);
    mem.vtable->deinit(mem.ctx);
}

static void test_semantic_vector_id_namespace(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_embedder_t emb = hu_embedder_local_create(&alloc);
    hu_vector_store_t store = hu_vector_store_mem_create(&alloc);
    HU_ASSERT_NOT_NULL(emb.vtable);
    HU_ASSERT_NOT_NULL(store.vtable);

    hu_embedding_t ea = {0}, eb = {0};
    HU_ASSERT_EQ(emb.vtable->embed(emb.ctx, &alloc, "alice coffee", 12, &ea), HU_OK);
    HU_ASSERT_EQ(emb.vtable->embed(emb.ctx, &alloc, "bob coffee", 10, &eb), HU_OK);

    const char *ida = "contact:alice:pref";
    const char *idb = "contact:bob:pref";
    HU_ASSERT_EQ(store.vtable->insert(store.ctx, &alloc, ida, strlen(ida), &ea, "alice coffee", 12),
                 HU_OK);
    HU_ASSERT_EQ(store.vtable->insert(store.ctx, &alloc, idb, strlen(idb), &eb, "bob coffee", 10),
                 HU_OK);

    hu_retrieval_options_t opts = {0};
    opts.mode = HU_RETRIEVAL_SEMANTIC;
    opts.limit = 10;
    opts.contact_id = "alice";
    opts.contact_id_len = 5;
    opts.require_contact_namespace = true;

    hu_retrieval_result_t out = {0};
    HU_ASSERT_EQ(hu_semantic_retrieve(&alloc, &emb, &store, "coffee", 6, &opts, &out), HU_OK);
    for (size_t i = 0; i < out.count; i++) {
        HU_ASSERT_TRUE(hu_retrieval_entry_in_contact_scope(&out.entries[i], "alice", 5));
    }
    hu_retrieval_result_free(&alloc, &out);

    hu_embedding_free(&alloc, &ea);
    hu_embedding_free(&alloc, &eb);
    store.vtable->deinit(store.ctx, &alloc);
    emb.vtable->deinit(emb.ctx, &alloc);
}

static void test_hybrid_require_namespace(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);

    hu_retrieval_options_t opts = {0};
    opts.mode = HU_RETRIEVAL_HYBRID;
    opts.limit = 5;
    opts.require_contact_namespace = true;

    hu_retrieval_result_t out = {0};
    hu_error_t err = hu_hybrid_retrieve(&alloc, &mem, NULL, NULL, NULL, "hi", 2, &opts, &out);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    mem.vtable->deinit(mem.ctx);
}

#endif /* HU_ENABLE_SQLITE */

void run_retrieval_contact_isolation_tests(void) {
    HU_TEST_SUITE("retrieval contact isolation");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_require_contact_namespace_rejects_missing_id);
    HU_RUN_TEST(test_keyword_cross_contact_isolation);
    HU_RUN_TEST(test_semantic_vector_id_namespace);
    HU_RUN_TEST(test_hybrid_require_namespace);
#endif
}
