#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/engines.h"
#include "human/memory/vector/circuit_breaker.h"
#include "human/memory/vector_math.h"
#include "test_framework.h"
#include <math.h>
#include <string.h>

static void test_vector_cosine_identical(void) {
    float a[] = {1.0f, 2.0f, 3.0f};
    float sim = hu_vector_cosine_similarity(a, a, 3);
    HU_ASSERT_FLOAT_EQ(sim, 1.0f, 0.001f);
}

static void test_vector_cosine_orthogonal(void) {
    float a[] = {1.0f, 0.0f, 0.0f};
    float b[] = {0.0f, 1.0f, 0.0f};
    float sim = hu_vector_cosine_similarity(a, b, 3);
    HU_ASSERT(fabsf(sim) < 0.001f);
}

static void test_vector_cosine_empty(void) {
    float a[] = {1.0f};
    float b[] = {1.0f};
    float sim = hu_vector_cosine_similarity(a, b, 0);
    HU_ASSERT_EQ(sim, 0.0f);
}

static void test_vector_cosine_mismatched_len(void) {
    float sim = hu_vector_cosine_similarity(NULL, NULL, 5);
    HU_ASSERT_EQ(sim, 0.0f);
    float a[] = {1.0f};
    float b[] = {2.0f};
    sim = hu_vector_cosine_similarity(a, b, 1);
    HU_ASSERT(fabsf(sim - 1.0f) < 0.01f);
}

static void test_vector_cosine_opposite(void) {
    float a[] = {1.0f, 0.0f};
    float b[] = {-1.0f, 0.0f};
    float sim = hu_vector_cosine_similarity(a, b, 2);
    HU_ASSERT(sim >= 0.0f && sim <= 0.01f);
}

static void test_vector_cosine_similar(void) {
    float a[] = {1.0f, 2.0f, 3.0f};
    float b[] = {1.1f, 2.0f, 2.9f};
    float sim = hu_vector_cosine_similarity(a, b, 3);
    HU_ASSERT(sim > 0.99f);
}

static void test_vector_to_bytes_null_input(void) {
    hu_allocator_t alloc = hu_system_allocator();
    unsigned char *bytes = hu_vector_to_bytes(&alloc, NULL, 4);
    HU_ASSERT_NULL(bytes);
}

static void test_vector_from_bytes_null_input(void) {
    hu_allocator_t alloc = hu_system_allocator();
    float *v = hu_vector_from_bytes(&alloc, NULL, 8);
    HU_ASSERT_NULL(v);
}

static void test_vector_from_bytes_odd_length(void) {
    hu_allocator_t alloc = hu_system_allocator();
    unsigned char bytes[5] = {0, 0, 0, 0, 0};
    float *v = hu_vector_from_bytes(&alloc, bytes, 5);
    HU_ASSERT_NOT_NULL(v);
    alloc.free(alloc.ctx, v, 4);
}

#ifdef HU_HAS_MEMORY_LRU_ENGINE
static void test_memory_lru_basic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    HU_ASSERT_NOT_NULL(mem.ctx);
    HU_ASSERT_NOT_NULL(mem.vtable);
    HU_ASSERT_STR_EQ(mem.vtable->name(mem.ctx), "memory_lru");

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    hu_error_t err = mem.vtable->store(mem.ctx, "greeting", 8, "hello world", 11, &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    size_t count = 0;
    err = mem.vtable->count(mem.ctx, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, (size_t)1);

    hu_memory_entry_t ent;
    bool found = false;
    err = mem.vtable->get(mem.ctx, &alloc, "greeting", 8, &ent, &found);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(found);
    HU_ASSERT_STR_EQ(ent.key, "greeting");
    HU_ASSERT_STR_EQ(ent.content, "hello world");
    hu_memory_entry_free_fields(&alloc, &ent);

    mem.vtable->deinit(mem.ctx);
}

static void test_memory_lru_eviction(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_memory_lru_create(&alloc, 3);
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};

    mem.vtable->store(mem.ctx, "a", 1, "first", 5, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "b", 1, "second", 6, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "c", 1, "third", 5, &cat, NULL, 0);

    size_t count = 0;
    mem.vtable->count(mem.ctx, &count);
    HU_ASSERT_EQ(count, (size_t)3);

    mem.vtable->store(mem.ctx, "d", 1, "fourth", 6, &cat, NULL, 0);
    mem.vtable->count(mem.ctx, &count);
    HU_ASSERT_EQ(count, (size_t)3);

    hu_memory_entry_t ent;
    bool found = false;
    mem.vtable->get(mem.ctx, &alloc, "a", 1, &ent, &found);
    HU_ASSERT(!found);

    mem.vtable->get(mem.ctx, &alloc, "d", 1, &ent, &found);
    HU_ASSERT(found);
    hu_memory_entry_free_fields(&alloc, &ent);

    mem.vtable->deinit(mem.ctx);
}

static void test_memory_lru_recall(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};

    mem.vtable->store(mem.ctx, "user_pref", 9, "dark mode enabled", 16, &cat, NULL, 0);
    hu_memory_entry_t *out = NULL;
    size_t out_count = 0;
    hu_error_t err = mem.vtable->recall(mem.ctx, &alloc, "mode", 4, 10, NULL, 0, &out, &out_count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(out_count >= 1);
    if (out_count > 0) {
        HU_ASSERT_STR_EQ(out[0].key, "user_pref");
        for (size_t i = 0; i < out_count; i++)
            hu_memory_entry_free_fields(&alloc, &out[i]);
        alloc.free(alloc.ctx, out, out_count * sizeof(hu_memory_entry_t));
    }

    mem.vtable->deinit(mem.ctx);
}
#endif

/* ─── Circuit breaker ─────────────────────────────────────────────────────── */
static void test_circuit_breaker_init_closed(void) {
    hu_circuit_breaker_t cb;
    hu_circuit_breaker_init(&cb, 3, 1000);
    HU_ASSERT_TRUE(hu_circuit_breaker_allow(&cb));
    HU_ASSERT_FALSE(hu_circuit_breaker_is_open(&cb));
}

static void test_circuit_breaker_opens_after_threshold(void) {
    hu_circuit_breaker_t cb;
    hu_circuit_breaker_init(&cb, 3, 1000);
    hu_circuit_breaker_record_failure(&cb);
    HU_ASSERT_TRUE(hu_circuit_breaker_allow(&cb));
    hu_circuit_breaker_record_failure(&cb);
    HU_ASSERT_TRUE(hu_circuit_breaker_allow(&cb));
    hu_circuit_breaker_record_failure(&cb);
    HU_ASSERT_FALSE(hu_circuit_breaker_allow(&cb));
    HU_ASSERT_TRUE(hu_circuit_breaker_is_open(&cb));
}

static void test_circuit_breaker_record_success_resets(void) {
    hu_circuit_breaker_t cb;
    hu_circuit_breaker_init(&cb, 2, 1000);
    hu_circuit_breaker_record_failure(&cb);
    hu_circuit_breaker_record_success(&cb);
    HU_ASSERT_TRUE(hu_circuit_breaker_allow(&cb));
    hu_circuit_breaker_record_failure(&cb);
    hu_circuit_breaker_record_failure(&cb);
    HU_ASSERT_FALSE(hu_circuit_breaker_allow(&cb));
    hu_circuit_breaker_record_success(&cb);
    HU_ASSERT_TRUE(hu_circuit_breaker_allow(&cb));
}

static void test_circuit_breaker_threshold_one(void) {
    hu_circuit_breaker_t cb;
    hu_circuit_breaker_init(&cb, 1, 1000);
    hu_circuit_breaker_record_failure(&cb);
    HU_ASSERT_FALSE(hu_circuit_breaker_allow(&cb));
}

/* ─── Vector math ───────────────────────────────────────────────────────── */
static void test_vector_bytes_roundtrip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    float original[] = {1.0f, -2.5f, 3.14f, 0.0f};
    unsigned char *bytes = hu_vector_to_bytes(&alloc, original, 4);
    HU_ASSERT_NOT_NULL(bytes);

    float *restored = hu_vector_from_bytes(&alloc, bytes, 16);
    HU_ASSERT_NOT_NULL(restored);
    for (int i = 0; i < 4; i++) {
        HU_ASSERT_FLOAT_EQ(original[i], restored[i], 0.0001f);
    }
    alloc.free(alloc.ctx, bytes, 16);
    alloc.free(alloc.ctx, restored, 4 * sizeof(float));
}

#ifdef HU_HAS_MEMORY_LRU_ENGINE
static void test_memory_lru_forget(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    mem.vtable->store(mem.ctx, "x", 1, "y", 1, &cat, NULL, 0);
    bool deleted = false;
    hu_error_t err = mem.vtable->forget(mem.ctx, "x", 1, &deleted);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(deleted);
    hu_memory_entry_t ent;
    bool found = false;
    mem.vtable->get(mem.ctx, &alloc, "x", 1, &ent, &found);
    HU_ASSERT_FALSE(found);
    mem.vtable->deinit(mem.ctx);
}

static void test_memory_lru_count_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_memory_lru_create(&alloc, 10);
    size_t count = 0;
    mem.vtable->count(mem.ctx, &count);
    HU_ASSERT_EQ(count, (size_t)0);
    mem.vtable->deinit(mem.ctx);
}

static void test_memory_lru_insert_lookup(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    mem.vtable->store(mem.ctx, "k1", 2, "v1", 2, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "k2", 2, "v2", 2, &cat, NULL, 0);
    hu_memory_entry_t ent;
    bool found = false;
    mem.vtable->get(mem.ctx, &alloc, "k1", 2, &ent, &found);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_STR_EQ(ent.content, "v1");
    hu_memory_entry_free_fields(&alloc, &ent);
    mem.vtable->deinit(mem.ctx);
}
#endif

void run_memory_subsystems_tests(void) {
    HU_TEST_SUITE("Memory subsystems");
#ifdef HU_HAS_MEMORY_LRU_ENGINE
    HU_RUN_TEST(test_memory_lru_basic);
    HU_RUN_TEST(test_memory_lru_eviction);
    HU_RUN_TEST(test_memory_lru_recall);
    HU_RUN_TEST(test_memory_lru_forget);
    HU_RUN_TEST(test_memory_lru_count_empty);
    HU_RUN_TEST(test_memory_lru_insert_lookup);
#endif
    HU_RUN_TEST(test_vector_cosine_identical);
    HU_RUN_TEST(test_vector_cosine_orthogonal);
    HU_RUN_TEST(test_vector_cosine_empty);
    HU_RUN_TEST(test_vector_cosine_mismatched_len);
    HU_RUN_TEST(test_vector_cosine_opposite);
    HU_RUN_TEST(test_vector_cosine_similar);
    HU_RUN_TEST(test_vector_to_bytes_null_input);
    HU_RUN_TEST(test_vector_from_bytes_null_input);
    HU_RUN_TEST(test_vector_from_bytes_odd_length);
    HU_RUN_TEST(test_vector_bytes_roundtrip);
    HU_RUN_TEST(test_circuit_breaker_init_closed);
    HU_RUN_TEST(test_circuit_breaker_opens_after_threshold);
    HU_RUN_TEST(test_circuit_breaker_record_success_resets);
    HU_RUN_TEST(test_circuit_breaker_threshold_one);
}
