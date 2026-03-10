#include "human/agent/commitment.h"
#include "human/agent/commitment_store.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/engines.h"
#include "test_framework.h"
#include <string.h>

static void commitment_detects_promise(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_commitment_detect_result_t result;
    const char *text = "I will finish the report";
    hu_error_t err =
        hu_commitment_detect(&alloc, text, strlen(text), "user", 4, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.count, 1);
    HU_ASSERT_EQ(result.commitments[0].type, HU_COMMITMENT_PROMISE);
    HU_ASSERT_NOT_NULL(result.commitments[0].summary);
    HU_ASSERT_TRUE(strstr(result.commitments[0].summary, "finish the report") != NULL);
    hu_commitment_detect_result_deinit(&result, &alloc);
}

static void commitment_detects_intention(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_commitment_detect_result_t result;
    const char *text = "I'm going to call her tomorrow";
    hu_error_t err =
        hu_commitment_detect(&alloc, text, strlen(text), "user", 4, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.count, 1);
    HU_ASSERT_EQ(result.commitments[0].type, HU_COMMITMENT_INTENTION);
    hu_commitment_detect_result_deinit(&result, &alloc);
}

static void commitment_detects_reminder(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_commitment_detect_result_t result;
    const char *text = "remind me to buy groceries";
    hu_error_t err =
        hu_commitment_detect(&alloc, text, strlen(text), "user", 4, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.count, 1);
    HU_ASSERT_EQ(result.commitments[0].type, HU_COMMITMENT_REMINDER);
    hu_commitment_detect_result_deinit(&result, &alloc);
}

static void commitment_detects_goal(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_commitment_detect_result_t result;
    const char *text = "I want to learn piano";
    hu_error_t err =
        hu_commitment_detect(&alloc, text, strlen(text), "user", 4, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.count, 1);
    HU_ASSERT_EQ(result.commitments[0].type, HU_COMMITMENT_GOAL);
    hu_commitment_detect_result_deinit(&result, &alloc);
}

static void commitment_ignores_negation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_commitment_detect_result_t result;
    const char *text = "I will not do that";
    hu_error_t err =
        hu_commitment_detect(&alloc, text, strlen(text), "user", 4, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.count, 0);
    hu_commitment_detect_result_deinit(&result, &alloc);
}

static void commitment_handles_empty_text(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_commitment_detect_result_t result;
    hu_error_t err = hu_commitment_detect(&alloc, "", 0, "user", 4, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.count, 0);
    hu_commitment_detect_result_deinit(&result, &alloc);
}

static void commitment_store_save_and_list(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    hu_commitment_store_t *store = NULL;
    HU_ASSERT_EQ(hu_commitment_store_create(&alloc, &mem, &store), HU_OK);
    HU_ASSERT_NOT_NULL(store);

    hu_commitment_detect_result_t result;
    const char *text1 = "I will finish the report";
    const char *text2 = "I want to learn piano";
    hu_commitment_detect(&alloc, text1, strlen(text1), "user", 4, &result);
    HU_ASSERT_EQ(result.count, 1);
    HU_ASSERT_EQ(hu_commitment_store_save(store, &result.commitments[0], NULL, 0), HU_OK);
    hu_commitment_detect_result_deinit(&result, &alloc);

    hu_commitment_detect(&alloc, text2, strlen(text2), "user", 4, &result);
    HU_ASSERT_EQ(result.count, 1);
    HU_ASSERT_EQ(hu_commitment_store_save(store, &result.commitments[0], NULL, 0), HU_OK);
    hu_commitment_detect_result_deinit(&result, &alloc);

    hu_commitment_t *active = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_commitment_store_list_active(store, &alloc, NULL, 0, &active, &count), HU_OK);
    HU_ASSERT_EQ(count, 2);
    for (size_t i = 0; i < count; i++)
        hu_commitment_deinit(&active[i], &alloc);
    alloc.free(alloc.ctx, active, count * sizeof(hu_commitment_t));

    hu_commitment_store_destroy(store);
    mem.vtable->deinit(mem.ctx);
}

static void commitment_store_build_context_formats(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    hu_commitment_store_t *store = NULL;
    HU_ASSERT_EQ(hu_commitment_store_create(&alloc, &mem, &store), HU_OK);

    hu_commitment_detect_result_t result;
    const char *text = "I will finish the report";
    hu_commitment_detect(&alloc, text, strlen(text), "user", 4, &result);
    HU_ASSERT_EQ(result.count, 1);
    HU_ASSERT_EQ(hu_commitment_store_save(store, &result.commitments[0], NULL, 0), HU_OK);
    hu_commitment_detect_result_deinit(&result, &alloc);

    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_commitment_store_build_context(store, &alloc, NULL, 0, &ctx, &ctx_len), HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(strstr(ctx, "finish the report") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "Active Commitments") != NULL);
    alloc.free(alloc.ctx, ctx, ctx_len + 1);

    hu_commitment_store_destroy(store);
    mem.vtable->deinit(mem.ctx);
}

static void commitment_detect_null_text_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_commitment_detect_result_t result;
    hu_error_t err = hu_commitment_detect(&alloc, NULL, 5, "user", 4, &result);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void commitment_detect_max_commitments(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_commitment_detect_result_t result;
    const char *text =
        "I will do A. I'll do B. I promise C. I'm going to D. I plan to E. I want to F.";
    hu_error_t err = hu_commitment_detect(&alloc, text, strlen(text), "user", 4, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.count <= HU_COMMITMENT_DETECT_MAX);
    hu_commitment_detect_result_deinit(&result, &alloc);
}

static void commitment_deinit_null_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_commitment_deinit(NULL, &alloc);
}

static void commitment_store_list_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    hu_commitment_store_t *store = NULL;
    HU_ASSERT_EQ(hu_commitment_store_create(&alloc, &mem, &store), HU_OK);

    hu_commitment_t *active = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_commitment_store_list_active(store, &alloc, NULL, 0, &active, &count), HU_OK);
    HU_ASSERT_EQ(count, 0u);
    HU_ASSERT_NULL(active);

    hu_commitment_store_destroy(store);
    mem.vtable->deinit(mem.ctx);
}

static void commitment_store_destroy_null_safe(void) {
    hu_commitment_store_destroy(NULL);
}

void run_commitment_tests(void) {
    HU_TEST_SUITE("commitment");
    HU_RUN_TEST(commitment_detects_promise);
    HU_RUN_TEST(commitment_detects_intention);
    HU_RUN_TEST(commitment_detects_reminder);
    HU_RUN_TEST(commitment_detects_goal);
    HU_RUN_TEST(commitment_ignores_negation);
    HU_RUN_TEST(commitment_handles_empty_text);
    HU_RUN_TEST(commitment_store_save_and_list);
    HU_RUN_TEST(commitment_store_build_context_formats);
    HU_RUN_TEST(commitment_detect_null_text_fails);
    HU_RUN_TEST(commitment_detect_max_commitments);
    HU_RUN_TEST(commitment_deinit_null_safe);
    HU_RUN_TEST(commitment_store_list_empty);
    HU_RUN_TEST(commitment_store_destroy_null_safe);
}
