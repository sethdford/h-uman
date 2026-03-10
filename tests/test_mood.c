#include "human/context/mood.h"
#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/engines.h"
#include "test_framework.h"
#include <string.h>

static void mood_context_with_emotions(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    HU_ASSERT_NOT_NULL(mem.ctx);

    static const char emotions_cat[] = "emotions";
    hu_memory_category_t cat = {
        .tag = HU_MEMORY_CATEGORY_CUSTOM,
        .data.custom = {.name = emotions_cat, .name_len = sizeof(emotions_cat) - 1},
    };

    const char *key1 = "emotion:contact_a:1000:joy";
    const char *content1 = "{\"tag\":\"joy\",\"intensity\":0.85,\"timestamp_ms\":1000}";
    mem.vtable->store(mem.ctx, key1, strlen(key1), content1, strlen(content1), &cat, "contact_a",
                      9);

    const char *key2 = "emotion:contact_a:1001:anxiety";
    const char *content2 = "{\"tag\":\"anxiety\",\"intensity\":0.50,\"timestamp_ms\":1001}";
    mem.vtable->store(mem.ctx, key2, strlen(key2), content2, strlen(content2), &cat, "contact_a",
                      9);

    const char *key3 = "emotion:contact_a:1002:excitement";
    const char *content3 = "{\"tag\":\"excitement\",\"intensity\":0.70,\"timestamp_ms\":1002}";
    mem.vtable->store(mem.ctx, key3, strlen(key3), content3, strlen(content3), &cat, "contact_a",
                      9);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_mood_build_context(&alloc, &mem, "contact_a", 9, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(out, "joy") != NULL);
    HU_ASSERT_TRUE(strstr(out, "anxiety") != NULL);
    HU_ASSERT_TRUE(strstr(out, "excitement") != NULL);
    HU_ASSERT_TRUE(strstr(out, "positive") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

static void mood_context_no_emotions(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    HU_ASSERT_NOT_NULL(mem.ctx);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_mood_build_context(&alloc, &mem, "contact_empty", 13, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(out_len, 0);

    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

static void mood_context_null_memory(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_mood_build_context(&alloc, NULL, "contact_a", 9, &out, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(out);
}

static void mood_context_negative_trend(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    HU_ASSERT_NOT_NULL(mem.ctx);

    static const char emotions_cat[] = "emotions";
    hu_memory_category_t cat = {
        .tag = HU_MEMORY_CATEGORY_CUSTOM,
        .data.custom = {.name = emotions_cat, .name_len = sizeof(emotions_cat) - 1},
    };

    const char *key1 = "emotion:contact_b:2000:sadness";
    const char *content1 = "{\"tag\":\"sadness\",\"intensity\":0.80,\"timestamp_ms\":2000}";
    mem.vtable->store(mem.ctx, key1, strlen(key1), content1, strlen(content1), &cat, "contact_b",
                      9);

    const char *key2 = "emotion:contact_b:2001:anger";
    const char *content2 = "{\"tag\":\"anger\",\"intensity\":0.65,\"timestamp_ms\":2001}";
    mem.vtable->store(mem.ctx, key2, strlen(key2), content2, strlen(content2), &cat, "contact_b",
                      9);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_mood_build_context(&alloc, &mem, "contact_b", 9, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "sadness") != NULL);
    HU_ASSERT_TRUE(strstr(out, "anger") != NULL);
    HU_ASSERT_TRUE(strstr(out, "negative") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

static void mood_context_null_alloc(void) {
    hu_memory_t mem = {0};
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_mood_build_context(NULL, &mem, "contact_a", 9, &out, &out_len);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void mood_context_empty_contact_id(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    HU_ASSERT_NOT_NULL(mem.ctx);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_mood_build_context(&alloc, &mem, "", 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(out);

    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

static void mood_context_null_output_ptrs(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_error_t err = hu_mood_build_context(&alloc, &mem, "contact_a", 9, NULL, NULL);
    HU_ASSERT_NEQ(err, HU_OK);

    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

static void mood_context_large_contact_id(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    HU_ASSERT_NOT_NULL(mem.ctx);

    char large_id[1025];
    memset(large_id, 'x', 1024);
    large_id[1024] = '\0';

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_mood_build_context(&alloc, &mem, large_id, 1024, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(out);

    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

void run_mood_tests(void) {
    HU_TEST_SUITE("mood");
    HU_RUN_TEST(mood_context_with_emotions);
    HU_RUN_TEST(mood_context_no_emotions);
    HU_RUN_TEST(mood_context_null_memory);
    HU_RUN_TEST(mood_context_negative_trend);
    HU_RUN_TEST(mood_context_null_alloc);
    HU_RUN_TEST(mood_context_empty_contact_id);
    HU_RUN_TEST(mood_context_null_output_ptrs);
    HU_RUN_TEST(mood_context_large_contact_id);
}
