#ifdef HU_ENABLE_SQLITE

#include "human/context/social_graph.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/memory/graph.h"
#include "human/persona.h"
#include "test_framework.h"
#include <string.h>

static void test_social_graph_store_get_returns_it(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_relationship_t rel = {0};
    snprintf(rel.name, sizeof(rel.name), "Sarah");
    snprintf(rel.role, sizeof(rel.role), "sister");
    snprintf(rel.notes, sizeof(rel.notes), "going through a divorce");

    hu_error_t err = hu_social_graph_store(&alloc, &mem, "alice", 5, &rel);
    HU_ASSERT_EQ(err, HU_OK);

    hu_relationship_t *out = NULL;
    size_t count = 0;
    err = hu_social_graph_get(&alloc, &mem, "alice", 5, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out[0].name, "Sarah");
    HU_ASSERT_STR_EQ(out[0].role, "sister");
    HU_ASSERT_STR_EQ(out[0].notes, "going through a divorce");

    hu_social_graph_free(&alloc, out, count);
    mem.vtable->deinit(mem.ctx);
}

static void test_social_graph_build_directive_mom_sister_contains_both(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_relationship_t rels[2];
    memset(rels, 0, sizeof(rels));
    snprintf(rels[0].name, sizeof(rels[0].name), "Sarah");
    snprintf(rels[0].role, sizeof(rels[0].role), "sister");
    snprintf(rels[0].notes, sizeof(rels[0].notes), "going through a divorce");
    snprintf(rels[1].name, sizeof(rels[1].name), "mom");
    rels[1].role[0] = '\0';
    snprintf(rels[1].notes, sizeof(rels[1].notes), "ask how she's doing when appropriate");

    const char *contact = "Alice";
    size_t len = 0;
    char *dir = hu_social_graph_build_directive(&alloc, contact, 5, rels, 2, &len);
    HU_ASSERT_NOT_NULL(dir);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(dir, "Sarah") != NULL);
    HU_ASSERT_TRUE(strstr(dir, "mom") != NULL);
    HU_ASSERT_TRUE(strstr(dir, "sister") != NULL);
    HU_ASSERT_TRUE(strstr(dir, "SOCIAL") != NULL);

    hu_str_free(&alloc, dir);
}

static void test_social_graph_no_relationships_null_directive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_relationship_t rels[1];
    memset(rels, 0, sizeof(rels));

    size_t len = 0;
    char *dir = hu_social_graph_build_directive(&alloc, "Alice", 5, rels, 0, &len);
    HU_ASSERT_NULL(dir);
    HU_ASSERT_EQ(len, 0u);
}

static void test_social_graph_get_empty_returns_zero(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_relationship_t *out = NULL;
    size_t count = 0;
    hu_error_t err = hu_social_graph_get(&alloc, &mem, "alice", 5, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);
    HU_ASSERT_NULL(out);

    mem.vtable->deinit(mem.ctx);
}

static void test_social_graph_build_context_uses_graph(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, "x", 1, &g), HU_OK);

    int64_t alice = 0, bob = 0, acme = 0;
    hu_graph_upsert_entity(g, "", 0, "alice", 5, HU_ENTITY_PERSON, NULL, &alice);
    hu_graph_upsert_entity(g, "", 0, "bob", 3, HU_ENTITY_PERSON, NULL, &bob);
    hu_graph_upsert_entity(g, "", 0, "acme", 4, HU_ENTITY_ORGANIZATION, NULL, &acme);
    hu_graph_upsert_relation(g, "", 0, alice, acme, HU_REL_WORKS_AT, 1.0f, NULL, 0);
    hu_graph_upsert_relation(g, "", 0, alice, bob, HU_REL_KNOWS, 0.9f, "colleague", 9);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_social_graph_build_context(&alloc, g, "alice bob acme", 14, NULL, 0, 2,
                                                   4096, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(out, "Knowledge") != NULL || strstr(out, "alice") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
    hu_graph_close(g, &alloc);
}

static void test_social_graph_build_context_null_args_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, "x", 1, &g), HU_OK);

    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(
        hu_social_graph_build_context(NULL, g, "alice", 5, NULL, 0, 1, 1024, &out, &out_len),
        HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(
        hu_social_graph_build_context(&alloc, NULL, "alice", 5, NULL, 0, 1, 1024, &out, &out_len),
        HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(
        hu_social_graph_build_context(&alloc, g, NULL, 5, NULL, 0, 1, 1024, &out, &out_len),
        HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(
        hu_social_graph_build_context(&alloc, g, "alice", 5, NULL, 0, 1, 1024, NULL, &out_len),
        HU_ERR_INVALID_ARGUMENT);

    hu_graph_close(g, &alloc);
}

void run_social_graph_tests(void) {
    HU_TEST_SUITE("social_graph");
    HU_RUN_TEST(test_social_graph_store_get_returns_it);
    HU_RUN_TEST(test_social_graph_build_directive_mom_sister_contains_both);
    HU_RUN_TEST(test_social_graph_no_relationships_null_directive);
    HU_RUN_TEST(test_social_graph_get_empty_returns_zero);
    HU_RUN_TEST(test_social_graph_build_context_uses_graph);
    HU_RUN_TEST(test_social_graph_build_context_null_args_returns_error);
}

#else

void run_social_graph_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
