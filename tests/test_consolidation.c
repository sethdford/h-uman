#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/consolidation.h"
#include "human/memory/engines.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

static void similarity_identical_returns_100(void) {
    const char *a = "hello world";
    const char *b = "hello world";
    uint32_t score = hu_similarity_score(a, 11, b, 11);
    HU_ASSERT_EQ(score, 100u);
}

static void similarity_completely_different_returns_0(void) {
    const char *a = "alpha beta gamma";
    const char *b = "foo bar baz";
    uint32_t score = hu_similarity_score(a, 15, b, 11);
    HU_ASSERT_EQ(score, 0u);
}

static void similarity_partial_overlap_returns_reasonable(void) {
    const char *a = "hello world foo";
    const char *b = "hello world bar";
    uint32_t score = hu_similarity_score(a, 14, b, 14);
    HU_ASSERT_TRUE(score >= 50u);
    HU_ASSERT_TRUE(score <= 100u);
}

#ifdef HU_ENABLE_SQLITE
static void consolidation_removes_duplicates(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    HU_ASSERT_NOT_NULL(mem.vtable);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    const char *dup = "user likes coffee and tea every morning";
    const char *unique = "user went hiking in the mountains last weekend";

    hu_error_t err = mem.vtable->store(mem.ctx, "cons_dup_a", 10, dup, strlen(dup), &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    err = mem.vtable->store(mem.ctx, "cons_dup_b", 10, dup, strlen(dup), &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    err = mem.vtable->store(mem.ctx, "cons_unique", 11, unique, strlen(unique), &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    size_t count_before = 0;
    err = mem.vtable->count(mem.ctx, &count_before);
    HU_ASSERT_EQ(err, HU_OK);

    hu_consolidation_config_t config = HU_CONSOLIDATION_DEFAULTS;
    config.dedup_threshold = 80;
    config.max_entries = 100;
    config.decay_days = 0;
    config.decay_factor = 1.0;

    err = hu_memory_consolidate(&alloc, &mem, &config);
    HU_ASSERT_EQ(err, HU_OK);

    size_t count_after = 0;
    err = mem.vtable->count(mem.ctx, &count_after);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count_after <= count_before);

    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}
#endif /* HU_ENABLE_SQLITE */

static void similarity_null_a_returns_0(void) {
    uint32_t score = hu_similarity_score(NULL, 0, "hello", 5);
    HU_ASSERT_EQ(score, 0u);
}

static void similarity_null_b_returns_0(void) {
    uint32_t score = hu_similarity_score("hello", 5, NULL, 0);
    HU_ASSERT_EQ(score, 0u);
}

static void similarity_both_empty_returns_100(void) {
    uint32_t score = hu_similarity_score("", 0, "", 0);
    HU_ASSERT_EQ(score, 100u);
}

static void similarity_same_prefix_reasonable(void) {
    const char *a = "hello world";
    const char *b = "hello there";
    uint32_t score = hu_similarity_score(a, 11, b, 11);
    HU_ASSERT_TRUE(score >= 30u);
    HU_ASSERT_TRUE(score <= 80u);
}

static void consolidation_null_memory_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_consolidation_config_t config = HU_CONSOLIDATION_DEFAULTS;
    hu_error_t err = hu_memory_consolidate(&alloc, NULL, &config);
    HU_ASSERT_NEQ(err, HU_OK);
}

void run_consolidation_tests(void) {
    HU_TEST_SUITE("consolidation");
    HU_RUN_TEST(similarity_identical_returns_100);
    HU_RUN_TEST(similarity_completely_different_returns_0);
    HU_RUN_TEST(similarity_partial_overlap_returns_reasonable);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(consolidation_removes_duplicates);
#endif
    HU_RUN_TEST(similarity_null_a_returns_0);
    HU_RUN_TEST(similarity_null_b_returns_0);
    HU_RUN_TEST(similarity_both_empty_returns_100);
    HU_RUN_TEST(similarity_same_prefix_reasonable);
    HU_RUN_TEST(consolidation_null_memory_returns_error);
}
