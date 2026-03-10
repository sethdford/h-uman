/* Episodic memory tests — session summarization, store/load */
#include "human/agent/episodic.h"
#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/provider.h"
#include "test_framework.h"
#include <string.h>

static void test_episodic_summarize_null(void) {
    HU_ASSERT_NULL(hu_episodic_summarize_session(NULL, NULL, NULL, 0, NULL));

    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_NULL(hu_episodic_summarize_session(&alloc, NULL, NULL, 0, NULL));
}

static void test_episodic_summarize_basic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *msgs[] = {"How do I compile human?", "Run cmake --build build"};
    size_t lens[] = {23, 23};

    size_t out_len = 0;
    char *summary = hu_episodic_summarize_session(&alloc, msgs, lens, 2, &out_len);
    HU_ASSERT_NOT_NULL(summary);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(summary, "Session topic:") != NULL);
    HU_ASSERT_TRUE(strstr(summary, "compile human") != NULL);
    alloc.free(alloc.ctx, summary, out_len + 1);
}

static void test_episodic_summarize_long_truncation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char long_msg[1024];
    memset(long_msg, 'A', sizeof(long_msg) - 1);
    long_msg[sizeof(long_msg) - 1] = '\0';

    const char *msgs[] = {long_msg};
    size_t lens[] = {sizeof(long_msg) - 1};

    size_t out_len = 0;
    char *summary = hu_episodic_summarize_session(&alloc, msgs, lens, 1, &out_len);
    HU_ASSERT_NOT_NULL(summary);
    HU_ASSERT_TRUE(out_len <= HU_EPISODIC_MAX_SUMMARY);
    alloc.free(alloc.ctx, summary, out_len + 1);
}

static void test_episodic_summarize_skips_empty_first(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *msgs[] = {"", "reply", "real question", "real answer"};
    size_t lens[] = {0, 5, 13, 11};

    size_t out_len = 0;
    char *summary = hu_episodic_summarize_session(&alloc, msgs, lens, 4, &out_len);
    HU_ASSERT_NOT_NULL(summary);
    HU_ASSERT_TRUE(strstr(summary, "real question") != NULL);
    alloc.free(alloc.ctx, summary, out_len + 1);
}

static void test_episodic_store_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_episodic_store(NULL, &alloc, "s1", 2, "sum", 3), HU_ERR_INVALID_ARGUMENT);
}

static void test_episodic_load_null_args(void) {
    HU_ASSERT_EQ(hu_episodic_load(NULL, NULL, NULL, NULL), HU_ERR_INVALID_ARGUMENT);

    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_episodic_load(NULL, &alloc, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(out);
}

static void test_episodic_store_with_session_id(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);

    hu_error_t err = hu_episodic_store(&mem, &alloc, "sess_abc", 8, "topic: test", 11);
    HU_ASSERT_EQ(err, HU_OK);
    if (mem.vtable && mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

static void test_episodic_store_without_session_id(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);

    hu_error_t err = hu_episodic_store(&mem, &alloc, NULL, 0, "global topic", 12);
    HU_ASSERT_EQ(err, HU_OK);
    if (mem.vtable && mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

static void test_episodic_key_prefix(void) {
    HU_ASSERT_STR_EQ(HU_EPISODIC_KEY_PREFIX, "_ep:");
    HU_ASSERT_EQ(HU_EPISODIC_KEY_PREFIX_LEN, 4);
}

static void test_episodic_summarize_llm_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov = {0};
    const char *msgs[] = {"How do I build?", "Run cmake --build build"};
    size_t lens[] = {16, 24};

    HU_ASSERT_NULL(hu_episodic_summarize_session_llm(NULL, &prov, msgs, lens, 2, NULL));
    HU_ASSERT_NULL(hu_episodic_summarize_session_llm(&alloc, &prov, NULL, lens, 2, NULL));
}

static void test_episodic_summarize_llm_basic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov = {0};
    const char *msgs[] = {"How do I compile human?", "Run cmake --build build"};
    size_t lens[] = {23, 23};

    size_t out_len = 0;
    char *summary = hu_episodic_summarize_session_llm(&alloc, &prov, msgs, lens, 2, &out_len);
    HU_ASSERT_NOT_NULL(summary);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(summary, "LLM summary:") != NULL);
    HU_ASSERT_TRUE(strstr(summary, "compile human") != NULL);
    alloc.free(alloc.ctx, summary, out_len + 1);
}

static void test_episodic_summarize_llm_fallback(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *msgs[] = {"What is the build command?", "Use cmake --build build"};
    size_t lens[] = {25, 22};

    size_t out_len = 0;
    char *summary = hu_episodic_summarize_session_llm(&alloc, NULL, msgs, lens, 2, &out_len);
    HU_ASSERT_NOT_NULL(summary);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(summary, "Session topic:") != NULL);
    HU_ASSERT_TRUE(strstr(summary, "build command") != NULL);
    alloc.free(alloc.ctx, summary, out_len + 1);
}

void run_episodic_tests(void) {
    HU_TEST_SUITE("Episodic");
    HU_RUN_TEST(test_episodic_summarize_null);
    HU_RUN_TEST(test_episodic_summarize_basic);
    HU_RUN_TEST(test_episodic_summarize_long_truncation);
    HU_RUN_TEST(test_episodic_summarize_skips_empty_first);
    HU_RUN_TEST(test_episodic_summarize_llm_null_args);
    HU_RUN_TEST(test_episodic_summarize_llm_basic);
    HU_RUN_TEST(test_episodic_summarize_llm_fallback);
    HU_RUN_TEST(test_episodic_store_null_args);
    HU_RUN_TEST(test_episodic_load_null_args);
    HU_RUN_TEST(test_episodic_store_with_session_id);
    HU_RUN_TEST(test_episodic_store_without_session_id);
    HU_RUN_TEST(test_episodic_key_prefix);
}
