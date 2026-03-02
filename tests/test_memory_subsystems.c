#include "test_framework.h"
#include "seaclaw/memory/vector_math.h"
#include "seaclaw/memory/vector/circuit_breaker.h"
#include "seaclaw/memory/engines.h"
#include "seaclaw/memory.h"
#include "seaclaw/core/allocator.h"
#include <string.h>
#include <math.h>

static void test_vector_cosine_identical(void) {
    float a[] = {1.0f, 2.0f, 3.0f};
    float sim = sc_vector_cosine_similarity(a, a, 3);
    SC_ASSERT_FLOAT_EQ(sim, 1.0f, 0.001f);
}

static void test_vector_cosine_orthogonal(void) {
    float a[] = {1.0f, 0.0f, 0.0f};
    float b[] = {0.0f, 1.0f, 0.0f};
    float sim = sc_vector_cosine_similarity(a, b, 3);
    SC_ASSERT(fabsf(sim) < 0.001f);
}

static void test_vector_cosine_empty(void) {
    float a[] = {1.0f};
    float b[] = {1.0f};
    float sim = sc_vector_cosine_similarity(a, b, 0);
    SC_ASSERT_EQ(sim, 0.0f);
}

static void test_vector_cosine_mismatched_len(void) {
    float sim = sc_vector_cosine_similarity(NULL, NULL, 5);
    SC_ASSERT_EQ(sim, 0.0f);
    float a[] = {1.0f};
    float b[] = {2.0f};
    sim = sc_vector_cosine_similarity(a, b, 1);
    SC_ASSERT(fabsf(sim - 1.0f) < 0.01f);
}

static void test_vector_cosine_opposite(void) {
    float a[] = {1.0f, 0.0f};
    float b[] = {-1.0f, 0.0f};
    float sim = sc_vector_cosine_similarity(a, b, 2);
    SC_ASSERT(sim >= 0.0f && sim <= 0.01f);
}

static void test_vector_cosine_similar(void) {
    float a[] = {1.0f, 2.0f, 3.0f};
    float b[] = {1.1f, 2.0f, 2.9f};
    float sim = sc_vector_cosine_similarity(a, b, 3);
    SC_ASSERT(sim > 0.99f);
}

static void test_vector_to_bytes_null_input(void) {
    sc_allocator_t alloc = sc_system_allocator();
    unsigned char *bytes = sc_vector_to_bytes(&alloc, NULL, 4);
    SC_ASSERT_NULL(bytes);
}

static void test_vector_from_bytes_null_input(void) {
    sc_allocator_t alloc = sc_system_allocator();
    float *v = sc_vector_from_bytes(&alloc, NULL, 8);
    SC_ASSERT_NULL(v);
}

static void test_vector_from_bytes_odd_length(void) {
    sc_allocator_t alloc = sc_system_allocator();
    unsigned char bytes[5] = {0, 0, 0, 0, 0};
    float *v = sc_vector_from_bytes(&alloc, bytes, 5);
    SC_ASSERT_NOT_NULL(v);
    alloc.free(alloc.ctx, v, 4);
}

#ifdef SC_HAS_MEMORY_LRU_ENGINE
static void test_memory_lru_basic(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_memory_lru_create(&alloc, 100);
    SC_ASSERT_NOT_NULL(mem.ctx);
    SC_ASSERT_NOT_NULL(mem.vtable);
    SC_ASSERT_STR_EQ(mem.vtable->name(mem.ctx), "memory_lru");

    sc_memory_category_t cat = { .tag = SC_MEMORY_CATEGORY_CORE };
    sc_error_t err = mem.vtable->store(mem.ctx, "greeting", 8, "hello world", 11, &cat, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);

    size_t count = 0;
    err = mem.vtable->count(mem.ctx, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, (size_t)1);

    sc_memory_entry_t ent;
    bool found = false;
    err = mem.vtable->get(mem.ctx, &alloc, "greeting", 8, &ent, &found);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(found);
    SC_ASSERT_STR_EQ(ent.key, "greeting");
    SC_ASSERT_STR_EQ(ent.content, "hello world");
    sc_memory_entry_free_fields(&alloc, &ent);

    mem.vtable->deinit(mem.ctx);
}

static void test_memory_lru_eviction(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_memory_lru_create(&alloc, 3);
    sc_memory_category_t cat = { .tag = SC_MEMORY_CATEGORY_CORE };

    mem.vtable->store(mem.ctx, "a", 1, "first", 5, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "b", 1, "second", 6, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "c", 1, "third", 5, &cat, NULL, 0);

    size_t count = 0;
    mem.vtable->count(mem.ctx, &count);
    SC_ASSERT_EQ(count, (size_t)3);

    mem.vtable->store(mem.ctx, "d", 1, "fourth", 6, &cat, NULL, 0);
    mem.vtable->count(mem.ctx, &count);
    SC_ASSERT_EQ(count, (size_t)3);

    sc_memory_entry_t ent;
    bool found = false;
    mem.vtable->get(mem.ctx, &alloc, "a", 1, &ent, &found);
    SC_ASSERT(!found);

    mem.vtable->get(mem.ctx, &alloc, "d", 1, &ent, &found);
    SC_ASSERT(found);
    sc_memory_entry_free_fields(&alloc, &ent);

    mem.vtable->deinit(mem.ctx);
}

static void test_memory_lru_recall(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_memory_lru_create(&alloc, 100);
    sc_memory_category_t cat = { .tag = SC_MEMORY_CATEGORY_CORE };

    mem.vtable->store(mem.ctx, "user_pref", 9, "dark mode enabled", 16, &cat, NULL, 0);
    sc_memory_entry_t *out = NULL;
    size_t out_count = 0;
    sc_error_t err = mem.vtable->recall(mem.ctx, &alloc, "mode", 4, 10, NULL, 0, &out, &out_count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(out_count >= 1);
    if (out_count > 0) {
        SC_ASSERT_STR_EQ(out[0].key, "user_pref");
        for (size_t i = 0; i < out_count; i++)
            sc_memory_entry_free_fields(&alloc, &out[i]);
        alloc.free(alloc.ctx, out, out_count * sizeof(sc_memory_entry_t));
    }

    mem.vtable->deinit(mem.ctx);
}
#endif

/* ─── Circuit breaker ─────────────────────────────────────────────────────── */
static void test_circuit_breaker_init_closed(void) {
    sc_circuit_breaker_t cb;
    sc_circuit_breaker_init(&cb, 3, 1000);
    SC_ASSERT_TRUE(sc_circuit_breaker_allow(&cb));
    SC_ASSERT_FALSE(sc_circuit_breaker_is_open(&cb));
}

static void test_circuit_breaker_opens_after_threshold(void) {
    sc_circuit_breaker_t cb;
    sc_circuit_breaker_init(&cb, 3, 1000);
    sc_circuit_breaker_record_failure(&cb);
    SC_ASSERT_TRUE(sc_circuit_breaker_allow(&cb));
    sc_circuit_breaker_record_failure(&cb);
    SC_ASSERT_TRUE(sc_circuit_breaker_allow(&cb));
    sc_circuit_breaker_record_failure(&cb);
    SC_ASSERT_FALSE(sc_circuit_breaker_allow(&cb));
    SC_ASSERT_TRUE(sc_circuit_breaker_is_open(&cb));
}

static void test_circuit_breaker_record_success_resets(void) {
    sc_circuit_breaker_t cb;
    sc_circuit_breaker_init(&cb, 2, 1000);
    sc_circuit_breaker_record_failure(&cb);
    sc_circuit_breaker_record_success(&cb);
    SC_ASSERT_TRUE(sc_circuit_breaker_allow(&cb));
    sc_circuit_breaker_record_failure(&cb);
    sc_circuit_breaker_record_failure(&cb);
    SC_ASSERT_FALSE(sc_circuit_breaker_allow(&cb));
    sc_circuit_breaker_record_success(&cb);
    SC_ASSERT_TRUE(sc_circuit_breaker_allow(&cb));
}

static void test_circuit_breaker_threshold_one(void) {
    sc_circuit_breaker_t cb;
    sc_circuit_breaker_init(&cb, 1, 1000);
    sc_circuit_breaker_record_failure(&cb);
    SC_ASSERT_FALSE(sc_circuit_breaker_allow(&cb));
}

/* ─── Vector math ───────────────────────────────────────────────────────── */
static void test_vector_bytes_roundtrip(void) {
    sc_allocator_t alloc = sc_system_allocator();
    float original[] = {1.0f, -2.5f, 3.14f, 0.0f};
    unsigned char *bytes = sc_vector_to_bytes(&alloc, original, 4);
    SC_ASSERT_NOT_NULL(bytes);

    float *restored = sc_vector_from_bytes(&alloc, bytes, 16);
    SC_ASSERT_NOT_NULL(restored);
    for (int i = 0; i < 4; i++) {
        SC_ASSERT_FLOAT_EQ(original[i], restored[i], 0.0001f);
    }
    alloc.free(alloc.ctx, bytes, 16);
    alloc.free(alloc.ctx, restored, 4 * sizeof(float));
}

#ifdef SC_HAS_MEMORY_LRU_ENGINE
static void test_memory_lru_forget(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_memory_lru_create(&alloc, 100);
    sc_memory_category_t cat = { .tag = SC_MEMORY_CATEGORY_CORE };
    mem.vtable->store(mem.ctx, "x", 1, "y", 1, &cat, NULL, 0);
    bool deleted = false;
    sc_error_t err = mem.vtable->forget(mem.ctx, "x", 1, &deleted);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(deleted);
    sc_memory_entry_t ent;
    bool found = false;
    mem.vtable->get(mem.ctx, &alloc, "x", 1, &ent, &found);
    SC_ASSERT_FALSE(found);
    mem.vtable->deinit(mem.ctx);
}

static void test_memory_lru_count_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_memory_lru_create(&alloc, 10);
    size_t count = 0;
    mem.vtable->count(mem.ctx, &count);
    SC_ASSERT_EQ(count, (size_t)0);
    mem.vtable->deinit(mem.ctx);
}

static void test_memory_lru_insert_lookup(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_memory_lru_create(&alloc, 100);
    sc_memory_category_t cat = { .tag = SC_MEMORY_CATEGORY_CORE };
    mem.vtable->store(mem.ctx, "k1", 2, "v1", 2, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "k2", 2, "v2", 2, &cat, NULL, 0);
    sc_memory_entry_t ent;
    bool found = false;
    mem.vtable->get(mem.ctx, &alloc, "k1", 2, &ent, &found);
    SC_ASSERT_TRUE(found);
    SC_ASSERT_STR_EQ(ent.content, "v1");
    sc_memory_entry_free_fields(&alloc, &ent);
    mem.vtable->deinit(mem.ctx);
}
#endif

void run_memory_subsystems_tests(void) {
    SC_TEST_SUITE("Memory subsystems");
#ifdef SC_HAS_MEMORY_LRU_ENGINE
    SC_RUN_TEST(test_memory_lru_basic);
    SC_RUN_TEST(test_memory_lru_eviction);
    SC_RUN_TEST(test_memory_lru_recall);
    SC_RUN_TEST(test_memory_lru_forget);
    SC_RUN_TEST(test_memory_lru_count_empty);
    SC_RUN_TEST(test_memory_lru_insert_lookup);
#endif
    SC_RUN_TEST(test_vector_cosine_identical);
    SC_RUN_TEST(test_vector_cosine_orthogonal);
    SC_RUN_TEST(test_vector_cosine_empty);
    SC_RUN_TEST(test_vector_cosine_mismatched_len);
    SC_RUN_TEST(test_vector_cosine_opposite);
    SC_RUN_TEST(test_vector_cosine_similar);
    SC_RUN_TEST(test_vector_to_bytes_null_input);
    SC_RUN_TEST(test_vector_from_bytes_null_input);
    SC_RUN_TEST(test_vector_from_bytes_odd_length);
    SC_RUN_TEST(test_vector_bytes_roundtrip);
    SC_RUN_TEST(test_circuit_breaker_init_closed);
    SC_RUN_TEST(test_circuit_breaker_opens_after_threshold);
    SC_RUN_TEST(test_circuit_breaker_record_success_resets);
    SC_RUN_TEST(test_circuit_breaker_threshold_one);
}
