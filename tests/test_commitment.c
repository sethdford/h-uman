#include "seaclaw/agent/commitment.h"
#include "seaclaw/agent/commitment_store.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/memory/engines.h"
#include "test_framework.h"
#include <string.h>

static void commitment_detects_promise(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_commitment_detect_result_t result;
    const char *text = "I will finish the report";
    sc_error_t err =
        sc_commitment_detect(&alloc, text, strlen(text), "user", 4, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(result.count, 1);
    SC_ASSERT_EQ(result.commitments[0].type, SC_COMMITMENT_PROMISE);
    SC_ASSERT_NOT_NULL(result.commitments[0].summary);
    SC_ASSERT_TRUE(strstr(result.commitments[0].summary, "finish the report") != NULL);
    sc_commitment_detect_result_deinit(&result, &alloc);
}

static void commitment_detects_intention(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_commitment_detect_result_t result;
    const char *text = "I'm going to call her tomorrow";
    sc_error_t err =
        sc_commitment_detect(&alloc, text, strlen(text), "user", 4, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(result.count, 1);
    SC_ASSERT_EQ(result.commitments[0].type, SC_COMMITMENT_INTENTION);
    sc_commitment_detect_result_deinit(&result, &alloc);
}

static void commitment_detects_reminder(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_commitment_detect_result_t result;
    const char *text = "remind me to buy groceries";
    sc_error_t err =
        sc_commitment_detect(&alloc, text, strlen(text), "user", 4, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(result.count, 1);
    SC_ASSERT_EQ(result.commitments[0].type, SC_COMMITMENT_REMINDER);
    sc_commitment_detect_result_deinit(&result, &alloc);
}

static void commitment_detects_goal(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_commitment_detect_result_t result;
    const char *text = "I want to learn piano";
    sc_error_t err =
        sc_commitment_detect(&alloc, text, strlen(text), "user", 4, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(result.count, 1);
    SC_ASSERT_EQ(result.commitments[0].type, SC_COMMITMENT_GOAL);
    sc_commitment_detect_result_deinit(&result, &alloc);
}

static void commitment_ignores_negation(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_commitment_detect_result_t result;
    const char *text = "I will not do that";
    sc_error_t err =
        sc_commitment_detect(&alloc, text, strlen(text), "user", 4, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(result.count, 0);
    sc_commitment_detect_result_deinit(&result, &alloc);
}

static void commitment_handles_empty_text(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_commitment_detect_result_t result;
    sc_error_t err = sc_commitment_detect(&alloc, "", 0, "user", 4, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(result.count, 0);
    sc_commitment_detect_result_deinit(&result, &alloc);
}

static void commitment_store_save_and_list(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_memory_lru_create(&alloc, 100);
    sc_commitment_store_t *store = NULL;
    SC_ASSERT_EQ(sc_commitment_store_create(&alloc, &mem, &store), SC_OK);
    SC_ASSERT_NOT_NULL(store);

    sc_commitment_detect_result_t result;
    const char *text1 = "I will finish the report";
    const char *text2 = "I want to learn piano";
    sc_commitment_detect(&alloc, text1, strlen(text1), "user", 4, &result);
    SC_ASSERT_EQ(result.count, 1);
    SC_ASSERT_EQ(sc_commitment_store_save(store, &result.commitments[0], NULL, 0), SC_OK);
    sc_commitment_detect_result_deinit(&result, &alloc);

    sc_commitment_detect(&alloc, text2, strlen(text2), "user", 4, &result);
    SC_ASSERT_EQ(result.count, 1);
    SC_ASSERT_EQ(sc_commitment_store_save(store, &result.commitments[0], NULL, 0), SC_OK);
    sc_commitment_detect_result_deinit(&result, &alloc);

    sc_commitment_t *active = NULL;
    size_t count = 0;
    SC_ASSERT_EQ(sc_commitment_store_list_active(store, &alloc, NULL, 0, &active, &count), SC_OK);
    SC_ASSERT_EQ(count, 2);
    for (size_t i = 0; i < count; i++)
        sc_commitment_deinit(&active[i], &alloc);
    alloc.free(alloc.ctx, active, count * sizeof(sc_commitment_t));

    sc_commitment_store_destroy(store);
    mem.vtable->deinit(mem.ctx);
}

static void commitment_store_build_context_formats(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_memory_lru_create(&alloc, 100);
    sc_commitment_store_t *store = NULL;
    SC_ASSERT_EQ(sc_commitment_store_create(&alloc, &mem, &store), SC_OK);

    sc_commitment_detect_result_t result;
    const char *text = "I will finish the report";
    sc_commitment_detect(&alloc, text, strlen(text), "user", 4, &result);
    SC_ASSERT_EQ(result.count, 1);
    SC_ASSERT_EQ(sc_commitment_store_save(store, &result.commitments[0], NULL, 0), SC_OK);
    sc_commitment_detect_result_deinit(&result, &alloc);

    char *ctx = NULL;
    size_t ctx_len = 0;
    SC_ASSERT_EQ(sc_commitment_store_build_context(store, &alloc, NULL, 0, &ctx, &ctx_len), SC_OK);
    SC_ASSERT_NOT_NULL(ctx);
    SC_ASSERT_TRUE(strstr(ctx, "finish the report") != NULL);
    SC_ASSERT_TRUE(strstr(ctx, "Active Commitments") != NULL);
    alloc.free(alloc.ctx, ctx, ctx_len + 1);

    sc_commitment_store_destroy(store);
    mem.vtable->deinit(mem.ctx);
}

void run_commitment_tests(void) {
    SC_TEST_SUITE("commitment");
    SC_RUN_TEST(commitment_detects_promise);
    SC_RUN_TEST(commitment_detects_intention);
    SC_RUN_TEST(commitment_detects_reminder);
    SC_RUN_TEST(commitment_detects_goal);
    SC_RUN_TEST(commitment_ignores_negation);
    SC_RUN_TEST(commitment_handles_empty_text);
    SC_RUN_TEST(commitment_store_save_and_list);
    SC_RUN_TEST(commitment_store_build_context_formats);
}
