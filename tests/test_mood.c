#include "seaclaw/context/mood.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/memory.h"
#include "seaclaw/memory/engines.h"
#include "test_framework.h"
#include <string.h>

static void mood_context_with_emotions(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_memory_lru_create(&alloc, 100);
    SC_ASSERT_NOT_NULL(mem.ctx);

    static const char emotions_cat[] = "emotions";
    sc_memory_category_t cat = {
        .tag = SC_MEMORY_CATEGORY_CUSTOM,
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
    sc_error_t err = sc_mood_build_context(&alloc, &mem, "contact_a", 9, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_TRUE(out_len > 0);
    SC_ASSERT_TRUE(strstr(out, "joy") != NULL);
    SC_ASSERT_TRUE(strstr(out, "anxiety") != NULL);
    SC_ASSERT_TRUE(strstr(out, "excitement") != NULL);
    SC_ASSERT_TRUE(strstr(out, "positive") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

static void mood_context_no_emotions(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_memory_lru_create(&alloc, 100);
    SC_ASSERT_NOT_NULL(mem.ctx);

    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_mood_build_context(&alloc, &mem, "contact_empty", 13, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NULL(out);
    SC_ASSERT_EQ(out_len, 0);

    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

static void mood_context_null_memory(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_mood_build_context(&alloc, NULL, "contact_a", 9, &out, &out_len);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_NULL(out);
}

static void mood_context_negative_trend(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_memory_lru_create(&alloc, 100);
    SC_ASSERT_NOT_NULL(mem.ctx);

    static const char emotions_cat[] = "emotions";
    sc_memory_category_t cat = {
        .tag = SC_MEMORY_CATEGORY_CUSTOM,
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
    sc_error_t err = sc_mood_build_context(&alloc, &mem, "contact_b", 9, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_TRUE(strstr(out, "sadness") != NULL);
    SC_ASSERT_TRUE(strstr(out, "anger") != NULL);
    SC_ASSERT_TRUE(strstr(out, "negative") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

void run_mood_tests(void) {
    SC_TEST_SUITE("mood");
    SC_RUN_TEST(mood_context_with_emotions);
    SC_RUN_TEST(mood_context_no_emotions);
    SC_RUN_TEST(mood_context_null_memory);
    SC_RUN_TEST(mood_context_negative_trend);
}
