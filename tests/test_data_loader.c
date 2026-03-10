#include "human/data/loader.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static void test_data_loader_loads_embedded_default(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_data_load_embedded(&alloc, "prompts/group_chat_hint.txt", &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 10);
    HU_ASSERT_NOT_NULL(strstr(out, "GROUP CHAT"));

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_data_loader_unknown_path_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_data_load_embedded(&alloc, "nonexistent/file.txt", &out, &out_len);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_NULL(out);
}

static void test_data_loader_falls_back_to_embedded(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_data_load(&alloc, "prompts/group_chat_hint.txt", &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 10);
    HU_ASSERT_NOT_NULL(strstr(out, "GROUP CHAT"));

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_data_loader_null_arguments(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    /* Test null allocator */
    hu_error_t err = hu_data_load_embedded(NULL, "prompts/group_chat_hint.txt", &out, &out_len);
    HU_ASSERT_NEQ(err, HU_OK);

    /* Test null path */
    err = hu_data_load_embedded(&alloc, NULL, &out, &out_len);
    HU_ASSERT_NEQ(err, HU_OK);

    /* Test null out pointer */
    err = hu_data_load_embedded(&alloc, "prompts/group_chat_hint.txt", NULL, &out_len);
    HU_ASSERT_NEQ(err, HU_OK);

    /* Test null out_len pointer */
    err = hu_data_load_embedded(&alloc, "prompts/group_chat_hint.txt", &out, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_data_loader_content_correct(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_data_load_embedded(&alloc, "prompts/group_chat_hint.txt", &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);

    /* Verify specific content */
    HU_ASSERT_NOT_NULL(strstr(out, "[GROUP CHAT]"));
    HU_ASSERT_NOT_NULL(strstr(out, "Keep responses to 1-2 sentences"));
    HU_ASSERT_NOT_NULL(strstr(out, "Don't dominate"));

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_data_loader_safety_rules(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_data_load_embedded(&alloc, "prompts/safety_rules.txt", &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_NOT_NULL(strstr(out, "## Safety"));
    HU_ASSERT_NOT_NULL(strstr(out, "Do not exfiltrate private data"));

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_data_loader_default_identity(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_data_load_embedded(&alloc, "prompts/default_identity.txt", &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_NOT_NULL(strstr(out, "You are Human"));

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_data_loader_reasoning_instruction(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_data_load_embedded(&alloc, "prompts/reasoning_instruction.txt", &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_NOT_NULL(strstr(out, "## Reasoning"));

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_data_loader_autonomy_readonly(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_data_load_embedded(&alloc, "prompts/autonomy_readonly.txt", &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_NOT_NULL(strstr(out, "readonly mode"));

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_data_loader_autonomy_supervised(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_data_load_embedded(&alloc, "prompts/autonomy_supervised.txt", &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_NOT_NULL(strstr(out, "supervised mode"));

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_data_loader_autonomy_full(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_data_load_embedded(&alloc, "prompts/autonomy_full.txt", &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_NOT_NULL(strstr(out, "full autonomy mode"));

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_data_loader_persona_reinforcement(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err = hu_data_load_embedded(&alloc, "prompts/persona_reinforcement.txt", &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_NOT_NULL(strstr(out, "real person"));

    alloc.free(alloc.ctx, out, out_len + 1);
}

void run_data_loader_tests(void) {
    HU_TEST_SUITE("data_loader");
    HU_RUN_TEST(test_data_loader_loads_embedded_default);
    HU_RUN_TEST(test_data_loader_unknown_path_returns_error);
    HU_RUN_TEST(test_data_loader_falls_back_to_embedded);
    HU_RUN_TEST(test_data_loader_null_arguments);
    HU_RUN_TEST(test_data_loader_content_correct);
    HU_RUN_TEST(test_data_loader_safety_rules);
    HU_RUN_TEST(test_data_loader_default_identity);
    HU_RUN_TEST(test_data_loader_reasoning_instruction);
    HU_RUN_TEST(test_data_loader_autonomy_readonly);
    HU_RUN_TEST(test_data_loader_autonomy_supervised);
    HU_RUN_TEST(test_data_loader_autonomy_full);
    HU_RUN_TEST(test_data_loader_persona_reinforcement);
}
