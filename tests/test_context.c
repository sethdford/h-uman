#include "seaclaw/context.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/provider.h"
#include "test_framework.h"
#include <string.h>

static void test_context_default_prompt(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *p = sc_context_build_system_prompt(&alloc, NULL, 0, NULL, 0);
    SC_ASSERT_NOT_NULL(p);
    SC_ASSERT_TRUE(strstr(p, "helpful") != NULL);
    alloc.free(alloc.ctx, p, strlen(p) + 1);
}

static void test_context_custom_prompt(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *p = sc_context_build_system_prompt(&alloc, "Be concise.", 11, NULL, 0);
    SC_ASSERT_NOT_NULL(p);
    SC_ASSERT_STR_EQ(p, "Be concise.");
    alloc.free(alloc.ctx, p, strlen(p) + 1);
}

static void test_context_format_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_chat_message_t *msgs = NULL;
    size_t count = 99;
    sc_error_t err = sc_context_format_messages(&alloc, NULL, 0, 10, &msgs, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 0u);
}

static void test_context_format_with_history(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_owned_message_t history[2] = {
        {.role = SC_ROLE_USER,
         .content = "hi",
         .content_len = 2,
         .name = NULL,
         .name_len = 0,
         .tool_call_id = NULL,
         .tool_call_id_len = 0,
         .tool_calls = NULL,
         .tool_calls_count = 0},
        {.role = SC_ROLE_ASSISTANT,
         .content = "hello",
         .content_len = 5,
         .name = NULL,
         .name_len = 0,
         .tool_call_id = NULL,
         .tool_call_id_len = 0,
         .tool_calls = NULL,
         .tool_calls_count = 0},
    };
    sc_chat_message_t *msgs = NULL;
    size_t count = 0;
    sc_error_t err = sc_context_format_messages(&alloc, history, 2, 10, &msgs, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 2u);
    SC_ASSERT_NOT_NULL(msgs);
    SC_ASSERT_EQ(msgs[0].role, SC_ROLE_USER);
    SC_ASSERT_EQ(msgs[0].content_len, 2u);
    SC_ASSERT_EQ(msgs[1].role, SC_ROLE_ASSISTANT);
    SC_ASSERT_EQ(msgs[1].content_len, 5u);
    alloc.free(alloc.ctx, msgs, count * sizeof(sc_chat_message_t));
}

static void test_context_format_respects_max_messages(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_owned_message_t history[4] = {
        {.role = SC_ROLE_USER,
         .content = "a",
         .content_len = 1,
         .name = NULL,
         .name_len = 0,
         .tool_call_id = NULL,
         .tool_call_id_len = 0,
         .tool_calls = NULL,
         .tool_calls_count = 0},
        {.role = SC_ROLE_ASSISTANT,
         .content = "b",
         .content_len = 1,
         .name = NULL,
         .name_len = 0,
         .tool_call_id = NULL,
         .tool_call_id_len = 0,
         .tool_calls = NULL,
         .tool_calls_count = 0},
        {.role = SC_ROLE_USER,
         .content = "c",
         .content_len = 1,
         .name = NULL,
         .name_len = 0,
         .tool_call_id = NULL,
         .tool_call_id_len = 0,
         .tool_calls = NULL,
         .tool_calls_count = 0},
        {.role = SC_ROLE_ASSISTANT,
         .content = "d",
         .content_len = 1,
         .name = NULL,
         .name_len = 0,
         .tool_call_id = NULL,
         .tool_call_id_len = 0,
         .tool_calls = NULL,
         .tool_calls_count = 0},
    };
    sc_chat_message_t *msgs = NULL;
    size_t count = 0;
    sc_error_t err = sc_context_format_messages(&alloc, history, 4, 2, &msgs, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 2u);
    SC_ASSERT_NOT_NULL(msgs);
    /* Should return last 2 messages (c and d) */
    SC_ASSERT_EQ(msgs[0].content_len, 1u);
    SC_ASSERT_EQ(msgs[0].content[0], 'c');
    SC_ASSERT_EQ(msgs[1].content[0], 'd');
    alloc.free(alloc.ctx, msgs, count * sizeof(sc_chat_message_t));
}

void run_context_tests(void) {
    SC_TEST_SUITE("Context");
    SC_RUN_TEST(test_context_default_prompt);
    SC_RUN_TEST(test_context_custom_prompt);
    SC_RUN_TEST(test_context_format_empty);
    SC_RUN_TEST(test_context_format_with_history);
    SC_RUN_TEST(test_context_format_respects_max_messages);
}
