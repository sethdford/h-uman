#include "human/context.h"
#include "human/core/allocator.h"
#include "human/provider.h"
#include "test_framework.h"
#include <string.h>

static void test_context_default_prompt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *p = hu_context_build_system_prompt(&alloc, NULL, 0, NULL, 0);
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_TRUE(strstr(p, "helpful") != NULL);
    alloc.free(alloc.ctx, p, strlen(p) + 1);
}

static void test_context_custom_prompt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *p = hu_context_build_system_prompt(&alloc, "Be concise.", 11, NULL, 0);
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_STR_EQ(p, "Be concise.");
    alloc.free(alloc.ctx, p, strlen(p) + 1);
}

static void test_context_format_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_chat_message_t *msgs = NULL;
    size_t count = 99;
    hu_error_t err = hu_context_format_messages(&alloc, NULL, 0, 10, &msgs, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);
}

static void test_context_format_with_history(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_owned_message_t history[2] = {
        {.role = HU_ROLE_USER,
         .content = "hi",
         .content_len = 2,
         .name = NULL,
         .name_len = 0,
         .tool_call_id = NULL,
         .tool_call_id_len = 0,
         .tool_calls = NULL,
         .tool_calls_count = 0},
        {.role = HU_ROLE_ASSISTANT,
         .content = "hello",
         .content_len = 5,
         .name = NULL,
         .name_len = 0,
         .tool_call_id = NULL,
         .tool_call_id_len = 0,
         .tool_calls = NULL,
         .tool_calls_count = 0},
    };
    hu_chat_message_t *msgs = NULL;
    size_t count = 0;
    hu_error_t err = hu_context_format_messages(&alloc, history, 2, 10, &msgs, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_NOT_NULL(msgs);
    HU_ASSERT_EQ(msgs[0].role, HU_ROLE_USER);
    HU_ASSERT_EQ(msgs[0].content_len, 2u);
    HU_ASSERT_EQ(msgs[1].role, HU_ROLE_ASSISTANT);
    HU_ASSERT_EQ(msgs[1].content_len, 5u);
    alloc.free(alloc.ctx, msgs, count * sizeof(hu_chat_message_t));
}

static void test_context_format_respects_max_messages(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_owned_message_t history[4] = {
        {.role = HU_ROLE_USER,
         .content = "a",
         .content_len = 1,
         .name = NULL,
         .name_len = 0,
         .tool_call_id = NULL,
         .tool_call_id_len = 0,
         .tool_calls = NULL,
         .tool_calls_count = 0},
        {.role = HU_ROLE_ASSISTANT,
         .content = "b",
         .content_len = 1,
         .name = NULL,
         .name_len = 0,
         .tool_call_id = NULL,
         .tool_call_id_len = 0,
         .tool_calls = NULL,
         .tool_calls_count = 0},
        {.role = HU_ROLE_USER,
         .content = "c",
         .content_len = 1,
         .name = NULL,
         .name_len = 0,
         .tool_call_id = NULL,
         .tool_call_id_len = 0,
         .tool_calls = NULL,
         .tool_calls_count = 0},
        {.role = HU_ROLE_ASSISTANT,
         .content = "d",
         .content_len = 1,
         .name = NULL,
         .name_len = 0,
         .tool_call_id = NULL,
         .tool_call_id_len = 0,
         .tool_calls = NULL,
         .tool_calls_count = 0},
    };
    hu_chat_message_t *msgs = NULL;
    size_t count = 0;
    hu_error_t err = hu_context_format_messages(&alloc, history, 4, 2, &msgs, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_NOT_NULL(msgs);
    /* Should return last 2 messages (c and d) */
    HU_ASSERT_EQ(msgs[0].content_len, 1u);
    HU_ASSERT_EQ(msgs[0].content[0], 'c');
    HU_ASSERT_EQ(msgs[1].content[0], 'd');
    alloc.free(alloc.ctx, msgs, count * sizeof(hu_chat_message_t));
}

void run_context_tests(void) {
    HU_TEST_SUITE("Context");
    HU_RUN_TEST(test_context_default_prompt);
    HU_RUN_TEST(test_context_custom_prompt);
    HU_RUN_TEST(test_context_format_empty);
    HU_RUN_TEST(test_context_format_with_history);
    HU_RUN_TEST(test_context_format_respects_max_messages);
}
